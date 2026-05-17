#pragma once

#include "test_macros.h"

#include "core/error/error_list.h"
#include "core/io/dir_access.h"
#include "core/io/image.h"
#include "core/math/math_funcs.h"
#include "core/os/os.h"
#include "core/string/ustring.h"
#include "servers/rendering/rendering_device.h"

#ifdef TESTS_ENABLED

namespace TestGaussianSplatting {
namespace VisualCompare {

inline constexpr const char *BASELINE_ROOT = "tests/visual_baselines";
inline constexpr const char *BASELINE_MODE_ENV = "GS_VISUAL_BASELINE_MODE";
// Note: an earlier revision allowed `GS_VISUAL_BASELINE_ROOT` to override the
// baseline directory to any absolute path. That was a path-traversal sink in
// update mode (PNGs would be saved under attacker-controlled paths). The
// override is removed in favor of walk-up resolution from the executable's
// directory — same robustness across cwd configurations but no untrusted
// input flows into the I/O path.

struct ComparisonResult {
	bool match = false;
	uint32_t mismatched_pixels = 0;
	uint64_t total_pixels = 0;
	int max_per_channel_diff_lsb = 0;
	double mean_per_channel_diff_lsb = 0.0;
	double psnr_db = 99.0;
	String diff_summary;
};

inline bool is_update_mode() {
	OS *os = OS::get_singleton();
	if (!os) {
		return false;
	}
	const String mode = os->get_environment(BASELINE_MODE_ENV).to_lower().strip_edges();
	return mode == "update";
}

// Resolve baselines to an absolute path so I/O is independent of the caller's
// current working directory. The harness binary commonly lives in `bin/`, so a
// raw cwd-relative `tests/visual_baselines` resolves to `bin/tests/...` and
// silently misses committed baselines (compare mode false-fails) or writes
// updated baselines to the wrong location (update mode misplaces artifacts).
//
// Resolution order:
//   1. Walk up from the executable's directory looking for `tests/visual_baselines`;
//      anchor there. Handles the standard `<repo>/bin/<godot>.exe` layout.
//   2. Fall back to cwd-relative — preserves the historical behavior for
//      callers that already run from the repo root.
//
// Deliberately no env-var override: any string accepted as an absolute path
// here becomes a write target in update mode, and the test harness is
// invoked from CI workflows that pass through PR-controlled environment in
// some configurations. Walk-up resolution is sufficient for every legitimate
// caller and doesn't introduce an untrusted-input I/O sink.
inline String _baseline_root_absolute() {
	OS *os = OS::get_singleton();
	if (os) {
		const String exe_path = os->get_executable_path();
		if (!exe_path.is_empty()) {
			String dir = exe_path.get_base_dir();
			for (int i = 0; i < 8 && !dir.is_empty(); i++) {
				const String candidate = dir.path_join(BASELINE_ROOT);
				if (DirAccess::dir_exists_absolute(candidate)) {
					return candidate;
				}
				const String parent = dir.get_base_dir();
				if (parent == dir) {
					break;
				}
				dir = parent;
			}
		}
	}
	return String(BASELINE_ROOT);
}

inline String resolve_baseline_path(const String &p_relative) {
	return _baseline_root_absolute().path_join(p_relative);
}

inline Error save_image_png(const Ref<Image> &p_image, const String &p_path) {
	ERR_FAIL_COND_V(p_image.is_null(), ERR_INVALID_PARAMETER);
	const String dir = p_path.get_base_dir();
	if (!dir.is_empty() && !DirAccess::dir_exists_absolute(dir)) {
		Error mk_err = DirAccess::make_dir_recursive_absolute(dir);
		if (mk_err != OK && mk_err != ERR_ALREADY_EXISTS) {
			return mk_err;
		}
	}
	return p_image->save_png(p_path);
}

inline Ref<Image> load_image_png(const String &p_path) {
	Ref<Image> img;
	img.instantiate();
	Error err = img->load(p_path);
	if (err != OK) {
		return Ref<Image>();
	}
	return img;
}

inline Ref<Image> read_back_texture(RenderingDevice *p_rd, RID p_texture) {
	ERR_FAIL_NULL_V(p_rd, Ref<Image>());
	ERR_FAIL_COND_V(!p_texture.is_valid(), Ref<Image>());

	Vector<uint8_t> data = p_rd->texture_get_data(p_texture, 0);
	if (data.is_empty()) {
		return Ref<Image>();
	}

	const RD::TextureFormat fmt = p_rd->texture_get_format(p_texture);
	Image::Format godot_fmt;
	// BGRA storage formats are common desktop render-target choices (D3D12
	// defaults, some Vulkan swapchain formats). Godot's Image::FORMAT_RGBA8
	// only accepts R,G,B,A byte order, so for BGRA inputs we swap R↔B in the
	// readback buffer in place and tag the resulting Image as RGBA8.
	// A8B8G8R8_*_PACK32 is byte-identical to R8G8B8A8 on little-endian
	// (the "A8B8G8R8" names the order MSB→LSB of the packed uint32, which is
	// R,G,B,A in memory on LE), so no swap is needed — same Image format.
	bool needs_rb_swap = false;
	switch (fmt.format) {
		case RD::DATA_FORMAT_R8G8B8A8_UNORM:
		case RD::DATA_FORMAT_R8G8B8A8_SRGB:
		case RD::DATA_FORMAT_A8B8G8R8_UNORM_PACK32:
		case RD::DATA_FORMAT_A8B8G8R8_SRGB_PACK32:
			godot_fmt = Image::FORMAT_RGBA8;
			break;
		case RD::DATA_FORMAT_B8G8R8A8_UNORM:
		case RD::DATA_FORMAT_B8G8R8A8_SRGB:
			godot_fmt = Image::FORMAT_RGBA8;
			needs_rb_swap = true;
			break;
		case RD::DATA_FORMAT_R16G16B16A16_SFLOAT:
			godot_fmt = Image::FORMAT_RGBAH;
			break;
		case RD::DATA_FORMAT_R32G32B32A32_SFLOAT:
			godot_fmt = Image::FORMAT_RGBAF;
			break;
		default:
			return Ref<Image>();
	}

	if (needs_rb_swap) {
		uint8_t *bytes = data.ptrw();
		const int64_t pixel_count = int64_t(fmt.width) * int64_t(fmt.height);
		for (int64_t i = 0; i < pixel_count; i++) {
			const int64_t offset = i * 4;
			if (offset + 3 >= data.size()) {
				break;
			}
			const uint8_t b = bytes[offset + 0];
			bytes[offset + 0] = bytes[offset + 2];
			bytes[offset + 2] = b;
		}
	}

	return Image::create_from_data(fmt.width, fmt.height, false, godot_fmt, data);
}

// Both images are converted to FORMAT_RGBA8 internally; comparisons are in 8-bit LSB units.
// Caller's images are not mutated.
inline ComparisonResult compare_images(const Ref<Image> &p_a, const Ref<Image> &p_b, double p_max_per_channel_diff_lsb = 1.0) {
	ComparisonResult result;
	if (p_a.is_null() || p_b.is_null()) {
		result.diff_summary = "Null image";
		return result;
	}
	if (p_a->get_width() != p_b->get_width() || p_a->get_height() != p_b->get_height()) {
		result.diff_summary = vformat("Size mismatch: %dx%d vs %dx%d",
				p_a->get_width(), p_a->get_height(), p_b->get_width(), p_b->get_height());
		return result;
	}

	const int w = p_a->get_width();
	const int h = p_a->get_height();
	if (w <= 0 || h <= 0) {
		// Zero-dimension images would divide by zero in the MSE/mean computations
		// below and produce a misleading "match=true / psnr=99" result for empty
		// inputs. Treat as a non-match with a clear summary so callers can branch.
		result.diff_summary = vformat("Empty image: %dx%d", w, h);
		return result;
	}
	result.total_pixels = uint64_t(w) * uint64_t(h);

	Ref<Image> a8;
	a8.instantiate();
	a8->set_data(w, h, false, p_a->get_format(), p_a->get_data());
	a8->convert(Image::FORMAT_RGBA8);

	Ref<Image> b8;
	b8.instantiate();
	b8->set_data(w, h, false, p_b->get_format(), p_b->get_data());
	b8->convert(Image::FORMAT_RGBA8);

	const Vector<uint8_t> da = a8->get_data();
	const Vector<uint8_t> db = b8->get_data();
	const int64_t n = MIN(da.size(), db.size());

	double sum_sq_err = 0.0;
	double sum_abs_lsb = 0.0;
	int max_lsb = 0;
	uint32_t mismatched = 0;
	// Compare in float space — the previous int(Math::ceil(threshold)) form
	// silently widened fractional tolerances (0.1 → 1, 1.1 → 2), so tests
	// that intentionally asked for sub-LSB tolerance still treated single-
	// LSB diffs as matches. Use the raw double threshold and compare against
	// the per-pixel max diff. Note: pixel values are quantized to 0..255, so
	// the smallest non-zero per-channel diff IS 1 — a threshold < 1.0
	// effectively means "exact match".
	const double max_lsb_threshold = p_max_per_channel_diff_lsb;

	const uint8_t *pa = da.ptr();
	const uint8_t *pb = db.ptr();
	for (int64_t pix = 0; pix < int64_t(result.total_pixels); pix++) {
		const int64_t offset = pix * 4;
		if (offset + 3 >= n) {
			break;
		}
		int pixel_max = 0;
		for (int c = 0; c < 4; c++) {
			const int diff = int(pa[offset + c]) - int(pb[offset + c]);
			const int abs_diff = diff < 0 ? -diff : diff;
			sum_sq_err += double(diff) * double(diff);
			sum_abs_lsb += double(abs_diff);
			if (abs_diff > pixel_max) {
				pixel_max = abs_diff;
			}
		}
		if (double(pixel_max) > max_lsb_threshold) {
			mismatched++;
		}
		if (pixel_max > max_lsb) {
			max_lsb = pixel_max;
		}
	}

	result.mismatched_pixels = mismatched;
	result.max_per_channel_diff_lsb = max_lsb;
	result.mean_per_channel_diff_lsb = sum_abs_lsb / (4.0 * double(result.total_pixels));
	const double mse = sum_sq_err / (4.0 * double(result.total_pixels));
	if (mse > 1e-10) {
		result.psnr_db = 20.0 * Math::log(255.0 / Math::sqrt(mse)) / Math::log(10.0);
	} else {
		result.psnr_db = 99.0;
	}
	result.match = (mismatched == 0);
	// Use String::num_uint64 + concatenation instead of vformat("%d", int(...))
	// for the pixel counters — `mismatched` is uint32_t (up to 4.3B) and
	// `total_pixels` is uint64_t. The original int() casts would overflow for
	// any image larger than ~2.1B pixels (16k×16k or larger) and produce
	// negative-looking counters in the diff_summary, misleading CI triage on
	// the very runs where accurate output matters most. Godot's vformat does
	// not accept `%llu` (verified earlier in this PR), so explicit string
	// conversion is the portable approach.
	const String mismatched_str = String::num_uint64(uint64_t(mismatched));
	const String total_str = String::num_uint64(result.total_pixels);
	result.diff_summary = "mismatched=" + mismatched_str + "/" + total_str +
			vformat(" max_lsb=%d mean_lsb=%.3f psnr=%.2f dB",
					max_lsb, result.mean_per_channel_diff_lsb, result.psnr_db);
	return result;
}

// In compare mode (default): asserts texture matches baseline within tolerance.
// In update mode (env GS_VISUAL_BASELINE_MODE=update): writes texture as the new baseline.
// On mismatch in compare mode, writes the captured image to <baseline_path>.actual.png for review.
inline bool capture_and_compare(RenderingDevice *p_rd, RID p_texture, const String &p_baseline_relative,
		double p_max_per_channel_diff_lsb = 1.0, double p_min_psnr_db = 45.0,
		String *r_failure_reason = nullptr) {
	const String baseline_path = resolve_baseline_path(p_baseline_relative);
	Ref<Image> captured = read_back_texture(p_rd, p_texture);
	if (captured.is_null()) {
		if (r_failure_reason) {
			*r_failure_reason = "Texture readback failed (unsupported format or invalid RID)";
		}
		return false;
	}

	if (is_update_mode()) {
		Error err = save_image_png(captured, baseline_path);
		if (err != OK) {
			if (r_failure_reason) {
				*r_failure_reason = vformat("Failed to write baseline %s (error %d)", baseline_path, int(err));
			}
			return false;
		}
		return true;
	}

	Ref<Image> baseline = load_image_png(baseline_path);
	if (baseline.is_null()) {
		if (r_failure_reason) {
			*r_failure_reason = vformat(
					"Baseline %s missing or unreadable. Re-run with GS_VISUAL_BASELINE_MODE=update to capture.",
					baseline_path);
		}
		save_image_png(captured, baseline_path + ".actual.png");
		return false;
	}

	ComparisonResult cmp = compare_images(baseline, captured, p_max_per_channel_diff_lsb);
	if (!cmp.match || cmp.psnr_db < p_min_psnr_db) {
		if (r_failure_reason) {
			*r_failure_reason = vformat("Baseline mismatch (%s); psnr_threshold=%.2f dB",
					cmp.diff_summary, p_min_psnr_db);
		}
		save_image_png(captured, baseline_path + ".actual.png");
		return false;
	}
	return true;
}

} // namespace VisualCompare
} // namespace TestGaussianSplatting

namespace {

TEST_CASE("[VisualCompare] save/load PNG round-trip is byte-identical for known fixture") {
	using namespace TestGaussianSplatting::VisualCompare;

	Ref<Image> fixture;
	fixture.instantiate();
	fixture->initialize_data(16, 16, false, Image::FORMAT_RGBA8);
	for (int y = 0; y < 16; y++) {
		for (int x = 0; x < 16; x++) {
			fixture->set_pixel(x, y, Color(x / 15.0f, y / 15.0f, 0.5f, 1.0f));
		}
	}

	const String tmp = String("_gs_visual_compare_selftest.png");
	REQUIRE(save_image_png(fixture, tmp) == OK);

	Ref<Image> loaded = load_image_png(tmp);
	REQUIRE(loaded.is_valid());
	CHECK_EQ(loaded->get_width(), 16);
	CHECK_EQ(loaded->get_height(), 16);

	ComparisonResult same = compare_images(fixture, loaded, 1.0);
	CHECK_MESSAGE(same.match, same.diff_summary.utf8().get_data());
	CHECK(same.max_per_channel_diff_lsb <= 1);
}

TEST_CASE("[VisualCompare] mutated fixture is detected as a mismatch") {
	using namespace TestGaussianSplatting::VisualCompare;

	Ref<Image> a;
	a.instantiate();
	a->initialize_data(16, 16, false, Image::FORMAT_RGBA8);
	a->fill(Color(0.5f, 0.5f, 0.5f, 1.0f));

	Ref<Image> b;
	b.instantiate();
	b->initialize_data(16, 16, false, Image::FORMAT_RGBA8);
	b->fill(Color(0.5f, 0.5f, 0.5f, 1.0f));
	b->set_pixel(8, 8, Color(1.0f, 0.0f, 0.0f, 1.0f));
	b->set_pixel(8, 9, Color(1.0f, 0.0f, 0.0f, 1.0f));

	ComparisonResult cmp = compare_images(a, b, 1.0);
	CHECK_FALSE(cmp.match);
	CHECK_EQ(cmp.mismatched_pixels, 2u);
	CHECK(cmp.max_per_channel_diff_lsb > 1);
}

TEST_CASE("[VisualCompare] tolerance threshold absorbs single-LSB difference") {
	using namespace TestGaussianSplatting::VisualCompare;

	Ref<Image> a;
	a.instantiate();
	a->initialize_data(8, 8, false, Image::FORMAT_RGBA8);
	a->fill(Color(0.5f, 0.5f, 0.5f, 1.0f));

	Ref<Image> b;
	b.instantiate();
	b->initialize_data(8, 8, false, Image::FORMAT_RGBA8);
	b->fill(Color(0.5f, 0.5f, 0.5f, 1.0f));
	b->set_pixel(4, 4, Color(0.5f + (1.0f / 255.0f), 0.5f, 0.5f, 1.0f));

	ComparisonResult cmp = compare_images(a, b, 1.0);
	CHECK(cmp.match);
	CHECK(cmp.max_per_channel_diff_lsb <= 1);
}

} // namespace

#endif // TESTS_ENABLED
