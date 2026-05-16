/**************************************************************************/
/*  gs_gpu_test_runner.cpp                                                */
/*  Phase 2 of the Gaussian Splatting GPU test harness. Builds on the     */
/*  Phase 1 spike (offscreen RenderingDevice via Main::test_setup() plus  */
/*  RenderingContextDriverVulkan) by integrating doctest::Context so the  */
/*  existing [RequiresGPU] TEST_CASEs actually execute instead of         */
/*  skipping. Argv pass-through mirrors tests/test_main.cpp:266-300.      */
/**************************************************************************/

#ifdef TESTS_ENABLED

#include "core/error/error_macros.h"
#include "core/os/os.h"
#include "core/string/print_string.h"
#include "core/string/ustring.h"
#include "core/templates/local_vector.h"
#include "main/main.h"

#include "tests/test_macros.h"

#if defined(RD_ENABLED)
#include "servers/rendering/rendering_device.h"
#if defined(VULKAN_ENABLED)
#include "drivers/vulkan/rendering_context_driver_vulkan.h"
#endif
#ifdef D3D12_ENABLED
#include "drivers/d3d12/rendering_context_driver_d3d12.h"
#endif
#endif

#include <cstring>

namespace {

// Exit codes — stable contract for run_gpu_harness.py to interpret.
constexpr int GS_GPU_EXIT_FAIL_TEST_SETUP = 70;
constexpr int GS_GPU_EXIT_NO_DRIVER = 64;
constexpr int GS_GPU_EXIT_DRIVER_INIT_FAILED = 65;
constexpr int GS_GPU_EXIT_RD_INIT_FAILED = 66;
constexpr int GS_GPU_EXIT_RD_NOT_COMPILED = 71;

#if defined(RD_ENABLED)
static RenderingContextDriver *_rcd = nullptr;
static RenderingDevice *_rd = nullptr;
static const char *_driver_label = "none";
#endif

struct GsGpuTestOptions {
	String preferred_driver;
	bool inject_default_filter = true;
};

GsGpuTestOptions _parse_options(int argc, char *argv[]) {
	GsGpuTestOptions opt;
	for (int x = 0; x < argc; x++) {
		const char *a = argv[x];
		if (strncmp(a, "--gs-gpu-driver=", 16) == 0) {
			opt.preferred_driver = String(a + 16);
		} else if (strncmp(a, "--test-case=", 12) == 0 || strncmp(a, "--test-case-exclude=", 20) == 0) {
			opt.inject_default_filter = false;
		}
	}
	return opt;
}

#if defined(RD_ENABLED)
int _bootstrap_rd(const GsGpuTestOptions &opt) {
	const String pref = opt.preferred_driver.to_lower().strip_edges();

#if defined(VULKAN_ENABLED)
	if (pref.is_empty() || pref == "vulkan") {
		_rcd = memnew(RenderingContextDriverVulkan);
		_driver_label = "vulkan";
	}
#endif
#ifdef D3D12_ENABLED
	if (_rcd == nullptr && (pref.is_empty() || pref == "d3d12")) {
		_rcd = memnew(RenderingContextDriverD3D12);
		_driver_label = "d3d12";
	}
#endif

	if (_rcd == nullptr) {
		print_error(vformat("[GS-GPU] FAIL: no rendering context driver compiled for preference '%s'",
				pref.is_empty() ? String("auto") : pref));
		return GS_GPU_EXIT_NO_DRIVER;
	}

	Error err = _rcd->initialize();
	if (err != OK) {
		print_error(vformat("[GS-GPU] FAIL: %s rcd->initialize() returned %d", _driver_label, int(err)));
		memdelete(_rcd);
		_rcd = nullptr;
		return GS_GPU_EXIT_DRIVER_INIT_FAILED;
	}

	_rd = memnew(RenderingDevice);
	err = _rd->initialize(_rcd);
	if (err != OK) {
		print_error(vformat("[GS-GPU] FAIL: %s rd->initialize() returned %d", _driver_label, int(err)));
		memdelete(_rd);
		_rd = nullptr;
		memdelete(_rcd);
		_rcd = nullptr;
		return GS_GPU_EXIT_RD_INIT_FAILED;
	}

	print_line(vformat("[GS-GPU] driver=%s adapter=\"%s\" vendor=\"%s\"",
			_driver_label,
			_rd->get_device_name(),
			_rd->get_device_vendor_name()));
	return 0;
}

void _teardown_rd() {
	if (_rd) {
		memdelete(_rd);
		_rd = nullptr;
	}
	if (_rcd) {
		memdelete(_rcd);
		_rcd = nullptr;
	}
}
#endif // RD_ENABLED

int _run_doctest(int argc, char *argv[], const GsGpuTestOptions &opt) {
	doctest::Context ctx;
	LocalVector<String> test_args;

	for (int x = 0; x < argc; x++) {
		String arg = String(argv[x]);
		if (arg == "--gs-gpu-test") {
			continue;
		}
		if (arg.begins_with("--gs-gpu-driver=")) {
			continue;
		}
		test_args.push_back(arg);
	}

	if (opt.inject_default_filter) {
		test_args.push_back(String("--test-case=*[RequiresGPU]*"));
	}

	if (test_args.size() > 0) {
		char **doctest_args = new char *[test_args.size()];
		for (uint32_t x = 0; x < test_args.size(); x++) {
			CharString cs = test_args[x].utf8();
			const char *str = cs.get_data();
			doctest_args[x] = new char[strlen(str) + 1];
			memcpy(doctest_args[x], str, strlen(str) + 1);
		}
		ctx.applyCommandLine(test_args.size(), doctest_args);
		for (uint32_t x = 0; x < test_args.size(); x++) {
			delete[] doctest_args[x];
		}
		delete[] doctest_args;
	}

	return ctx.run();
}

// doctest reporter that snapshots GPU memory before and after each test case
// and emits an advisory MESSAGE when a test leaves > 1 MiB of GPU memory behind.
// Registered globally; no-ops when RenderingDevice::get_singleton() is null
// (so it stays quiet under the existing `--test` path).
struct GsGpuRidLeakListener : public doctest::IReporter {
	explicit GsGpuRidLeakListener(const doctest::ContextOptions &) {}

	uint64_t case_start_memory = 0;

	void test_case_start(const doctest::TestCaseData &) override {
		case_start_memory = 0;
#if defined(RD_ENABLED)
		if (RenderingDevice *rd = RenderingDevice::get_singleton()) {
			case_start_memory = rd->get_memory_usage(RenderingDevice::MEMORY_TOTAL);
		}
#endif
	}

	void test_case_end(const doctest::CurrentTestCaseStats &) override {
#if defined(RD_ENABLED)
		if (RenderingDevice *rd = RenderingDevice::get_singleton()) {
			const uint64_t end_memory = rd->get_memory_usage(RenderingDevice::MEMORY_TOTAL);
			if (end_memory > case_start_memory) {
				const uint64_t delta = end_memory - case_start_memory;
				if (delta > (1ull << 20)) {
					print_line(vformat("[GS-GPU][RID-LEAK?] test left %s bytes of GPU memory allocated",
							String::num_uint64(delta)));
				}
			}
		}
#endif
	}

	void report_query(const doctest::QueryData &) override {}
	void test_run_start() override {}
	void test_run_end(const doctest::TestRunStats &) override {}
	void test_case_reenter(const doctest::TestCaseData &) override {}
	void test_case_exception(const doctest::TestCaseException &) override {}
	void subcase_start(const doctest::SubcaseSignature &) override {}
	void subcase_end() override {}
	void log_assert(const doctest::AssertData &) override {}
	void log_message(const doctest::MessageData &) override {}
	void test_case_skipped(const doctest::TestCaseData &) override {}
};

REGISTER_LISTENER("GsGpuRidLeakListener", 1, GsGpuRidLeakListener);

} // namespace

int gs_gpu_test_main(int argc, char *argv[]) {
#if !defined(RD_ENABLED)
	(void)argc;
	(void)argv;
	print_error("[GS-GPU] FAIL: build is missing RD_ENABLED; no RenderingDevice support compiled");
	return GS_GPU_EXIT_RD_NOT_COMPILED;
#else
	Error setup_err = Main::test_setup();
	if (setup_err != OK) {
		print_error(vformat("[GS-GPU] FAIL: Main::test_setup() returned %d", int(setup_err)));
		return GS_GPU_EXIT_FAIL_TEST_SETUP;
	}

	const GsGpuTestOptions opt = _parse_options(argc, argv);

	int bootstrap_rc = _bootstrap_rd(opt);
	if (bootstrap_rc != 0) {
		Main::test_cleanup();
		return bootstrap_rc;
	}

	int rc = _run_doctest(argc, argv, opt);

	_teardown_rd();
	Main::test_cleanup();
	return rc;
#endif
}

#endif // TESTS_ENABLED
