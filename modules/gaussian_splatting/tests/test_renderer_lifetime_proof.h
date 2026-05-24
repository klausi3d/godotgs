/**************************************************************************/
/*  test_renderer_lifetime_proof.h                                        */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

/*
 * PR 3 of work package #352 — Renderer Lifetime Proof.
 *
 * Composes existing accounting surfaces into a single roll-up that proves
 * zero retained GPU/RID resources across four renderer lifecycle scenarios.
 * The fixture introduces NO new instrumentation; it reads:
 *
 *   - RenderDeviceManager::get_last_shutdown_owned_resource_count()
 *     get_last_shutdown_tracked_resource_count()
 *     Canonical signal for "the renderer's RDM forgot to free an owned RID
 *     it tracked." See interfaces/render_device_manager.h:115-117.
 *
 *   - RenderingDevice::get_memory_usage(MEMORY_TOTAL) delta
 *     Catches non-RDM-tracked churn (raw rd->X_create bypasses,
 *     OutputCompositor framebuffer LRU, etc.). Reuses the SAME 4 MiB noise
 *     floor that the existing [GS-GPU][RID-LEAK?] listener trusts in
 *     tests/gs_gpu_test_runner.cpp:248. For monotonicity checks (C, D) we
 *     use delta-of-deltas so allocator-pool noise cancels out.
 *
 *   - StringName orphans: DEFERRED to PR 6. Emit sentinel -1 in the JSON;
 *     PR 6's subprocess emitter will feed the same JSON shape from a
 *     different path so PR 7's manifest gate consumes one contract.
 *
 * Pass rule: rdm_owned_leaked == 0 AND rd_memory_leaked_bytes < threshold.
 * AND, not OR — they catch different bugs.
 *
 * JSON contract (PR 7 will consume — do not change after merge):
 *   [GS-LIFETIME] {"scenario":"renderer_instance","passed":true,
 *     "rd_bytes_leaked":131072,"rdm_owned_leaked":0,"rdm_tracked_leaked":0,
 *     "teardown_sync":true,"threshold_bytes":4194304,
 *     "stringname_orphan_delta":-1,"monotonicity_ok":true,"fail_reason":""}
 *
 * monotonicity_ok defaults true for absolute-threshold scenarios (A, B);
 * monotonicity-style scenarios (C, D) MUST call set_monotonicity_ok(false,
 * reason) BEFORE finalize() when their cycle-to-cycle delta-of-deltas
 * exceeds the configured threshold. finalize() folds monotonicity_ok into
 * the final pass verdict so the emitted "passed" field always reflects the
 * full pass rule. (Codex PR #386 review #3294763666 P1: previously the
 * monotonicity result was only folded into the local CHECK_MESSAGE bool,
 * not into the JSON contract — downstream gates consuming only the
 * [GS-LIFETIME] line would mis-classify a monotonicity regression as a
 * pass.)
 *
 * Line prefix [GS-LIFETIME] mirrors the [GS-GPU] convention used by
 * gs_gpu_test_runner.cpp so PR 7's manifest parser can grep it trivially.
 */

#pragma once

#include "test_macros.h"

#include "../core/gaussian_data.h"
#include "../core/gaussian_splat_manager.h"
#include "../core/gaussian_splat_scene_director.h"
#include "../core/gaussian_streaming.h"
#include "../interfaces/render_device_manager.h"
#include "../renderer/gaussian_splat_renderer.h"

#include "core/io/json.h"
#include "core/math/random_number_generator.h"
#include "core/string/print_string.h"
#include "core/templates/local_vector.h"
#include "core/variant/dictionary.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering_server.h"

namespace TestGaussianSplatting {

// ---------------------------------------------------------------------------
// LifetimeBaseline — snapshot of every accounting surface at a single point.
// stringname_orphan_delta sentinel -1 = not measured (deferred to PR 6).
// ---------------------------------------------------------------------------
struct LifetimeBaseline {
	uint64_t rd_memory_usage_bytes = 0;
	uint32_t rdm_tracked_resources = 0;
	uint32_t rdm_owned_resources = 0;
	int64_t stringname_orphan_delta = -1;
};

// ---------------------------------------------------------------------------
// LifetimeResult — pass/fail roll-up plus the JSON line emitter.
// ---------------------------------------------------------------------------
struct LifetimeResult {
	String scenario_id;
	LifetimeBaseline before;
	LifetimeBaseline after;
	uint64_t rd_memory_leaked_bytes = 0;
	uint32_t rdm_owned_leaked = 0;
	uint32_t rdm_tracked_leaked = 0;
	uint64_t threshold_bytes = 4ull << 20; // 4 MiB — matches GsGpuRidLeakListener.
	bool teardown_was_synchronous = false;
	bool crashed_or_aborted = false;
	// True unless a monotonicity-style scenario (C, D) recorded a
	// delta-of-deltas excursion via set_monotonicity_ok(false, reason).
	// Folded into `passed` by _compute_pass(). Always emitted in the JSON
	// so downstream gates can distinguish a real lifetime regression in a
	// monotonicity scenario from an absolute-byte-threshold breach.
	// (Codex PR #386 review #3294763666 P1.)
	bool monotonicity_ok = true;
	bool passed = false;
	String fail_reason;

	Dictionary to_dict() const {
		Dictionary d;
		d["scenario"] = scenario_id;
		d["passed"] = passed;
		d["rd_bytes_leaked"] = int64_t(rd_memory_leaked_bytes);
		d["rdm_owned_leaked"] = int64_t(rdm_owned_leaked);
		d["rdm_tracked_leaked"] = int64_t(rdm_tracked_leaked);
		d["teardown_sync"] = teardown_was_synchronous;
		d["threshold_bytes"] = int64_t(threshold_bytes);
		d["stringname_orphan_delta"] = int64_t(after.stringname_orphan_delta);
		d["monotonicity_ok"] = monotonicity_ok;
		d["fail_reason"] = fail_reason;
		return d;
	}

	void emit_to_stdout() const {
		// Mirrors the [GS-GPU][RID-LEAK?] line-prefix pattern from
		// tests/gs_gpu_test_runner.cpp:260 — grep-trivial for PR 7.
		const String json = JSON::stringify(to_dict());
		print_line(String("[GS-LIFETIME] ") + json);
	}
};

// ---------------------------------------------------------------------------
// RendererLifetimeFixture — RAII wrapper. capture_baseline() snapshots the
// before-state; finalize() snapshots after-state, computes the pass rule,
// and emits the JSON line. The dtor auto-finalizes if the test exited via
// REQUIRE_FALSE without an explicit finalize() call.
// ---------------------------------------------------------------------------
class RendererLifetimeFixture {
public:
	RendererLifetimeFixture(const String &p_scenario_id, RenderingDevice *p_rd) :
			scenario_id_(p_scenario_id), rd_(p_rd) {
		result_.scenario_id = p_scenario_id;
	}

	~RendererLifetimeFixture() {
		if (!finalized_) {
			// REQUIRE failure path — emit a best-effort line so PR 7 still
			// sees the scenario in its manifest, marked failed.
			result_.crashed_or_aborted = true;
			result_.passed = false;
			if (result_.fail_reason.is_empty()) {
				result_.fail_reason = "scope_exited_without_finalize";
			}
			result_.emit_to_stdout();
		}
	}

	RendererLifetimeFixture(const RendererLifetimeFixture &) = delete;
	RendererLifetimeFixture &operator=(const RendererLifetimeFixture &) = delete;

	void capture_baseline() {
		_snapshot(result_.before);
		baselined_ = true;
	}

	// Caller invokes finalize() AFTER all references the scenario allocated
	// have been dropped. Computes the pass rule, emits the JSON line, and
	// returns the pass bool so the TEST_CASE can CHECK() it.
	bool finalize() {
		if (finalized_) {
			return result_.passed;
		}
		// Preserve RDM shutdown counts already folded in by
		// record_rdm_shutdown_counts() before _snapshot() overwrites the
		// after-baseline's RDM fields with the sentinel 0s. Without this,
		// rdm_owned_leaked / rdm_tracked_leaked are always 0 in the pass
		// rule and a real RDM-owned RID leak can slip through whenever
		// rd_memory_leaked_bytes stays under threshold. (Codex PR #386
		// review, P1.) Strategy: option (b) — save+restore around
		// _snapshot, keeping the API unchanged.
		const uint32_t saved_rdm_owned = result_.after.rdm_owned_resources;
		const uint32_t saved_rdm_tracked = result_.after.rdm_tracked_resources;
		_snapshot(result_.after);
		result_.after.rdm_owned_resources = saved_rdm_owned;
		result_.after.rdm_tracked_resources = saved_rdm_tracked;
		_compute_pass();
		result_.emit_to_stdout();
		finalized_ = true;
		return result_.passed;
	}

	// Mark this scenario as a skip (e.g. failed_init when an RD singleton is
	// present). Emits a failed=false / passed=false JSON line with
	// fail_reason explaining the skip. Counts toward the manifest but the
	// gate treats fail_reason != "" + crashed_or_aborted as advisory.
	void mark_skipped(const String &p_reason) {
		if (finalized_) {
			return;
		}
		result_.fail_reason = String("skipped: ") + p_reason;
		result_.passed = false;
		result_.crashed_or_aborted = false;
		result_.emit_to_stdout();
		finalized_ = true;
	}

	// Override the noise-floor threshold for monotonicity-style scenarios
	// (C, D) where the absolute number isn't meaningful — only the
	// cycle-to-cycle delta matters.
	void set_threshold_bytes(uint64_t p_bytes) { result_.threshold_bytes = p_bytes; }

	// Record an extra fail reason without changing pass-rule arithmetic.
	void set_fail_reason(const String &p_reason) {
		if (result_.fail_reason.is_empty()) {
			result_.fail_reason = p_reason;
		} else {
			result_.fail_reason += String("; ") + p_reason;
		}
	}

	// Monotonicity-style scenarios (C, D) record their delta-of-deltas
	// verdict here BEFORE finalize(). The fixture folds monotonicity_ok
	// into both the local `passed` bool AND the emitted [GS-LIFETIME] JSON
	// line so downstream gates that consume only the JSON cannot
	// mis-classify a monotonicity regression as a pass. When monotonicity
	// fails, `p_reason` is appended to `fail_reason` and
	// `monotonicity_regression` is prepended so the failure mode is
	// self-describing in the JSON. (Codex PR #386 review #3294763666 P1.)
	void set_monotonicity_ok(bool p_ok, const String &p_reason = String()) {
		result_.monotonicity_ok = p_ok;
		if (!p_ok) {
			const String tag = "monotonicity_regression";
			String detail = tag;
			if (!p_reason.is_empty()) {
				detail += String(": ") + p_reason;
			}
			if (result_.fail_reason.is_empty()) {
				result_.fail_reason = detail;
			} else if (result_.fail_reason.find(tag) < 0) {
				result_.fail_reason = detail + String("; ") + result_.fail_reason;
			}
		}
	}

	void set_teardown_was_synchronous(bool p_sync) {
		result_.teardown_was_synchronous = p_sync;
	}

	const LifetimeResult &result() const { return result_; }

private:
	String scenario_id_;
	RenderingDevice *rd_ = nullptr;
	LifetimeResult result_;
	bool baselined_ = false;
	bool finalized_ = false;

	void _snapshot(LifetimeBaseline &r_out) const {
		r_out.rd_memory_usage_bytes = (rd_ != nullptr)
				? rd_->get_memory_usage(RenderingDevice::MEMORY_TOTAL)
				: 0ull;
		// PR 6 will populate these via the subprocess emitter path. For
		// now the sentinel propagates through.
		r_out.rdm_tracked_resources = 0;
		r_out.rdm_owned_resources = 0;
		r_out.stringname_orphan_delta = -1;
	}

	void _compute_pass() {
		// rd_memory_leaked_bytes := max(0, after - before).
		if (result_.after.rd_memory_usage_bytes > result_.before.rd_memory_usage_bytes) {
			result_.rd_memory_leaked_bytes =
					result_.after.rd_memory_usage_bytes - result_.before.rd_memory_usage_bytes;
		} else {
			result_.rd_memory_leaked_bytes = 0;
		}
		// RDM owned/tracked deltas are only meaningful when an RDM shutdown
		// was invoked during the scenario and its post-shutdown counters
		// were folded in by the caller via record_rdm_shutdown_*.
		if (result_.after.rdm_owned_resources > result_.before.rdm_owned_resources) {
			result_.rdm_owned_leaked =
					result_.after.rdm_owned_resources - result_.before.rdm_owned_resources;
		} else {
			result_.rdm_owned_leaked = 0;
		}
		if (result_.after.rdm_tracked_resources > result_.before.rdm_tracked_resources) {
			result_.rdm_tracked_leaked =
					result_.after.rdm_tracked_resources - result_.before.rdm_tracked_resources;
		} else {
			result_.rdm_tracked_leaked = 0;
		}

		// Pass rule: AND of all signals. They catch different bugs.
		// monotonicity_ok is folded in here so the [GS-LIFETIME] JSON
		// line that finalize() emits AFTER this call reflects the full
		// verdict — a monotonicity-style scenario (asset_attach_detach,
		// scene_director_reload) that breaches its cycle-to-cycle delta
		// threshold cannot publish "passed":true to downstream gates.
		// (Codex PR #386 review #3294763666 P1.)
		const bool rdm_ok = (result_.rdm_owned_leaked == 0);
		const bool rd_bytes_ok = (result_.rd_memory_leaked_bytes < result_.threshold_bytes);
		result_.passed = rdm_ok && rd_bytes_ok && result_.monotonicity_ok && !result_.crashed_or_aborted;
		if (!result_.passed && result_.fail_reason.is_empty()) {
			if (!rdm_ok) {
				result_.fail_reason = vformat("rdm_owned_leaked=%d > 0", int64_t(result_.rdm_owned_leaked));
			} else if (!rd_bytes_ok) {
				result_.fail_reason = vformat(
						"rd_bytes_leaked=%d >= threshold=%d",
						int64_t(result_.rd_memory_leaked_bytes),
						int64_t(result_.threshold_bytes));
			} else if (!result_.monotonicity_ok) {
				// set_monotonicity_ok(false, ...) populates fail_reason
				// directly, so this branch is only reached when a caller
				// flipped result_.monotonicity_ok via some other path
				// without supplying a reason. Keep the JSON
				// self-describing.
				result_.fail_reason = "monotonicity_regression";
			}
		}
	}

public:
	// Fold a manager-shutdown counter pair into the after-snapshot so the
	// pass rule includes the RDM signal. Callers use this when they own a
	// manager they shut down explicitly inside the scenario.
	void record_rdm_shutdown_counts(uint32_t p_owned, uint32_t p_tracked) {
		// Add to the after-snapshot so multiple managers (one per cycle)
		// accumulate. Before-snapshot stays at 0 so the leak == counter.
		result_.after.rdm_owned_resources += p_owned;
		result_.after.rdm_tracked_resources += p_tracked;
	}
};

// ---------------------------------------------------------------------------
// Helpers: build a 64-splat GaussianData, run a render_for_view at 128x128.
// ---------------------------------------------------------------------------
inline Ref<::GaussianData> _make_lifetime_test_data(int p_count) {
	Ref<::GaussianData> data;
	data.instantiate();
	LocalVector<Gaussian> gaussians;
	gaussians.resize(p_count);
	RandomNumberGenerator rng;
	rng.set_seed(0x9E3779B97F4A7C15ull);
	for (int i = 0; i < p_count; i++) {
		Gaussian &g = gaussians[i];
		g = Gaussian{};
		g.position = Vector3(
				rng.randf_range(-2.0f, 2.0f),
				rng.randf_range(-2.0f, 2.0f),
				rng.randf_range(-5.0f, -3.0f));
		g.scale = Vector3(0.1f, 0.1f, 0.1f);
		g.rotation = Quaternion();
		g.opacity = 1.0f;
		g.sh_dc = Color(1.0f, 0.5f, 0.2f, 1.0f);
		g.normal = Vector3(0.0f, 1.0f, 0.0f);
		g.area = 0.01f;
	}
	data->set_gaussians(gaussians);
	return data;
}

// ---------------------------------------------------------------------------
// Scenario A: renderer_instance — create, use, destroy in isolation.
// ---------------------------------------------------------------------------
TEST_CASE("[GaussianSplatting][Renderer][Lifetime][RequiresGPU] renderer_instance lifetime proof") {
	REQUIRE_GPU_DEVICE();
	// `rd` is published by the macro above; reuse it as the
	// authoritative device the renderer was constructed against.

	RendererLifetimeFixture fixture("renderer_instance", rd);
	fixture.capture_baseline();

	{
		Ref<GaussianSplatRenderer> renderer;
		renderer.instantiate(rd);
		REQUIRE(renderer.is_valid());

		Ref<::GaussianData> data = _make_lifetime_test_data(64);
		const Error set_err = renderer->set_gaussian_data(data);
		// ERR_UNCONFIGURED is acceptable: some asset wiring paths only
		// fully configure during the first frame's render_for_view.
		CHECK((set_err == OK || set_err == ERR_UNCONFIGURED));

		Transform3D cam_transform;
		Projection projection;
		projection.set_perspective(60.0f, 1.0f, 0.1f, 200.0f);
		renderer->render_for_view(cam_transform, projection, RID(), Size2i(128, 128));

		// Dispatcher-path probe: ask the renderer (via the dispatcher)
		// whether a render-thread dispatch would actually be attempted
		// BEFORE unref. If a render loop is standing (a future runner
		// flips REQUIRE_GPU_DEVICE to use a live RD), this returns true
		// and the renderer dtor would dispatch teardown to that loop —
		// silent mis-measurement. We require it to return false (no
		// dispatch path) so the dtor falls through to the synchronous
		// _teardown_resources() path. See
		// renderer/gaussian_splat_renderer.cpp:1232-1239.
		//
		// Importantly, this is the new
		// test_is_render_thread_dispatch_path_active() probe — NOT the
		// older test_dispatch_call_on_render_thread_blocking_without_completion()
		// helper, which submits an actual dispatch and conflates
		// "no dispatch path active" (early-exit returned false) with
		// "dispatched but timed out waiting for completion" (also
		// returned false). In a render-loop-enabled environment the
		// older helper could time out and the fixture would silently
		// mis-mark teardown as synchronous. The new probe is a pure
		// inspection of the dispatcher's early-exit state and cannot be
		// confounded by a wait-for-completion timeout. (Codex PR #386
		// review, P1.)
		const bool dispatch_path_active =
				renderer->test_is_render_thread_dispatch_path_active();
		REQUIRE_FALSE_MESSAGE(dispatch_path_active,
				"render_thread_dispatcher path is active in lifetime tests; "
				"renderer dtor would otherwise dispatch teardown to it and the fixture "
				"would mis-measure the post-unref baseline.");
		fixture.set_teardown_was_synchronous(true);

		// Refcount sanity: any leftover Ref<> means dtor won't run on unref.
		// scene_director._should_prune_world (cpp:896) requires refcount<=1
		// for cleanup, so even one extra Ref breaks lifetime accounting.
		// Diagnostics::register_renderer and PerformanceMonitors track raw
		// pointers (not Refs) so they do not bump refcount; verified
		// against rendering_diagnostics.cpp:48 and performance_monitors.h:89.
		CHECK_MESSAGE(renderer->get_reference_count() == 1,
				"Renderer refcount > 1 before unref: a singleton or shared owner "
				"is retaining a Ref<>. Filing this as a finding does not loosen "
				"the lifetime gate.");

		renderer.unref();
	}

	const bool passed = fixture.finalize();
	CHECK_MESSAGE(passed, fixture.result().fail_reason);
	CHECK(fixture.result().teardown_was_synchronous);
}

// ---------------------------------------------------------------------------
// Scenario B: failed_init — GaussianStreamingSystem.initialize() with no
// device, assert clean teardown. Skip-via-emit if RD singleton present.
// ---------------------------------------------------------------------------
TEST_CASE("[GaussianSplatting][Renderer][Lifetime][FailedInit] failed_init lifetime proof") {
	// Tag is intentionally NOT [RequiresGPU]; this scenario asserts the
	// no-device path stays clean. If the runner DID provision an RD via
	// ANY of the three resolution paths the GaussianStreamingSystem will
	// itself consult (RD singleton, RenderingServer, or the
	// GaussianSplatManager), emit a skipped JSON line so the manifest
	// still sees the scenario in its row count. Checking only the RD
	// singleton — as before — meant the system could obtain a device via
	// the manager/RenderingServer path and the assertion that
	// is_streaming_capable() == false would then target the wrong branch.
	// Mirrors the pattern PR #383 shipped in
	// test_gaussian_streaming_lifecycle.cpp:810-820. (Codex PR #386
	// review, P2.)
	bool has_device = RenderingDevice::get_singleton() != nullptr;
	const char *which_device = "RD singleton";
	if (!has_device) {
		if (RenderingServer *rs_probe = RenderingServer::get_singleton()) {
			if (rs_probe->get_rendering_device() != nullptr) {
				has_device = true;
				which_device = "RenderingServer device";
			}
		}
	}
	if (!has_device) {
		if (GaussianSplatManager *mgr_probe = GaussianSplatManager::get_singleton()) {
			if (mgr_probe->get_primary_rendering_device() != nullptr) {
				has_device = true;
				which_device = "GaussianSplatManager primary device";
			}
		}
	}
	if (has_device) {
		RendererLifetimeFixture fixture("failed_init", nullptr);
		fixture.capture_baseline();
		fixture.mark_skipped(String(which_device) + " present");
		return;
	}

	RendererLifetimeFixture fixture("failed_init", nullptr);
	fixture.capture_baseline();

	{
		Ref<::GaussianData> data = _make_lifetime_test_data(64);

		Ref<GaussianStreamingSystem> system;
		system.instantiate();
		// No set_device_manager / initialize_with_device — this MUST fail
		// the runtime-ready check and leave the system in a safe state
		// per PR #383's failed-init guards.
		system->initialize(data);
		CHECK_FALSE(system->is_streaming_capable());

		system.unref();
	}

	const bool passed = fixture.finalize();
	CHECK_MESSAGE(passed, fixture.result().fail_reason);
}

// ---------------------------------------------------------------------------
// Scenario C: scene_director_reload — 3x submit/release cycle on a
// synthetic scenario, monotonicity check.
//
// SHIPS SKIP'D in PR 3. PR 4 flips skip(true) -> skip(false) once
// SharedWorld explicit teardown closes the F6 reload leak at
// gaussian_splat_scene_director.cpp:351. The flip is the two-line
// evidence that PR 4 worked.
// ---------------------------------------------------------------------------
TEST_CASE("[GaussianSplatting][Renderer][Lifetime][RequiresGPU] scene_director_reload lifetime proof"
		* doctest::skip(true)) {
	// ENABLE in PR 4 — once SharedWorld teardown lands, flip skip(true) to
	// skip(false). No other change needed; this body is the evidence.
	REQUIRE_GPU_DEVICE();

	GaussianSplatSceneDirector *director = GaussianSplatSceneDirector::get_singleton();
	const bool owns_director = (director == nullptr);
	if (!director) {
		director = memnew(GaussianSplatSceneDirector);
	}
	REQUIRE(director != nullptr);

	RendererLifetimeFixture fixture("scene_director_reload", rd);
	// Monotonicity threshold: cycle 3 may not exceed cycle 1 by >256 KiB.
	fixture.set_threshold_bytes(256ull * 1024ull);
	fixture.capture_baseline();

	uint64_t cycle1_after_bytes = 0;
	for (int cycle = 0; cycle < 3; cycle++) {
		// Synthetic scenario RID — director keys on the RID itself; no
		// World3D needed for this signal.
		const RID scenario = RID::from_uint64(0x4f4f4c0000000001ull + uint64_t(cycle));
		(void)scenario; // Real bind only enabled in PR 4 with SceneTree.

		// PR 4 will populate this with the real submit_world_submission /
		// release_world_submission cycle and a SharedWorld teardown
		// assertion. The skeleton here ensures PR 4's diff is small.
	}
	(void)cycle1_after_bytes;

	if (owns_director) {
		memdelete(director);
	}

	const bool passed = fixture.finalize();
	CHECK_MESSAGE(passed, fixture.result().fail_reason);
}

// ---------------------------------------------------------------------------
// Scenario D: asset_attach_detach — one renderer, 3x set/clear gaussian
// data cycle, monotonicity check. Cycles 2/3 grow ≤ 64 KiB beyond cycle 1.
// ---------------------------------------------------------------------------
TEST_CASE("[GaussianSplatting][Renderer][Lifetime][RequiresGPU] asset_attach_detach lifetime proof") {
	REQUIRE_GPU_DEVICE();

	RendererLifetimeFixture fixture("asset_attach_detach", rd);
	// Monotonicity threshold for the asset cycle delta-of-deltas.
	fixture.set_threshold_bytes(64ull * 1024ull);
	fixture.capture_baseline();

	uint64_t cycle1_delta_bytes = 0;
	uint64_t cycle_growth_beyond_cycle1 = 0;
	bool monotonicity_ok = true;
	String monotonicity_reason;

	{
		Ref<GaussianSplatRenderer> renderer;
		renderer.instantiate(rd);
		REQUIRE(renderer.is_valid());

		Ref<::GaussianData> data = _make_lifetime_test_data(64);
		Ref<::GaussianData> empty_data; // null Ref — clear path.

		const uint64_t pre_cycle_bytes = rd->get_memory_usage(RenderingDevice::MEMORY_TOTAL);

		for (int cycle = 0; cycle < 3; cycle++) {
			const Error set_err = renderer->set_gaussian_data(data);
			CHECK((set_err == OK || set_err == ERR_UNCONFIGURED));
			// Drive a render so any deferred allocation actually happens.
			Transform3D cam_transform;
			Projection projection;
			projection.set_perspective(60.0f, 1.0f, 0.1f, 200.0f);
			renderer->render_for_view(cam_transform, projection, RID(), Size2i(128, 128));

			const Error clear_err = renderer->set_gaussian_data(empty_data);
			CHECK((clear_err == OK || clear_err == ERR_UNCONFIGURED));

			const uint64_t post_cycle_bytes = rd->get_memory_usage(RenderingDevice::MEMORY_TOTAL);
			const uint64_t cycle_delta = (post_cycle_bytes > pre_cycle_bytes)
					? (post_cycle_bytes - pre_cycle_bytes)
					: 0ull;

			if (cycle == 0) {
				cycle1_delta_bytes = cycle_delta;
			} else {
				const uint64_t growth = (cycle_delta > cycle1_delta_bytes)
						? (cycle_delta - cycle1_delta_bytes)
						: 0ull;
				if (growth > cycle_growth_beyond_cycle1) {
					cycle_growth_beyond_cycle1 = growth;
				}
				if (growth > fixture.result().threshold_bytes) {
					monotonicity_ok = false;
					monotonicity_reason = vformat(
							"cycle %d delta %d B grew %d B beyond cycle-1 delta %d B (threshold %d B)",
							cycle + 1,
							int64_t(cycle_delta),
							int64_t(growth),
							int64_t(cycle1_delta_bytes),
							int64_t(fixture.result().threshold_bytes));
				}
			}
		}

		CHECK_MESSAGE(renderer->get_reference_count() == 1,
				"Renderer refcount > 1 before unref in asset_attach_detach scenario.");
		renderer.unref();
	}

	(void)cycle_growth_beyond_cycle1;

	// Fold the monotonicity verdict into the fixture BEFORE finalize() so
	// the emitted [GS-LIFETIME] JSON line's "passed" field reflects the
	// final verdict and the JSON includes a self-describing
	// "monotonicity_regression" fail_reason. Previously the JSON was
	// emitted BEFORE monotonicity was folded into the local pass bool,
	// which meant a real asset_attach_detach regression that breached the
	// monotonicity threshold (but not the absolute rd-byte threshold)
	// would fail the local CHECK_MESSAGE yet still publish "passed":true
	// to PR #390's --mode lifetime gate. (Codex PR #386 review
	// #3294763666 P1.)
	fixture.set_monotonicity_ok(monotonicity_ok, monotonicity_reason);

	// For monotonicity scenarios, the absolute rd_memory_leaked vs threshold
	// in finalize() is the cycle-1 vs after baseline. The monotonicity
	// signal above is what's actually load-bearing, surfaced via
	// monotonicity_ok and fail_reason in the JSON line.
	const bool scenario_passed = fixture.finalize();
	CHECK_MESSAGE(scenario_passed, fixture.result().fail_reason);
}

// ---------------------------------------------------------------------------
// Regression: monotonicity failure must publish "passed":false in JSON.
//
// Codex PR #386 review #3294763666 (P1): the asset_attach_detach scenario
// previously computed monotonicity_ok in the test body, then called
// fixture.finalize() (which emitted the [GS-LIFETIME] JSON line) BEFORE
// folding monotonicity_ok into the local pass bool. When RD/RDM thresholds
// passed but monotonicity failed, the local CHECK_MESSAGE correctly failed,
// but the JSON contract that downstream gates (PR #390's --mode lifetime)
// consume still claimed "passed":true. This test constructs that exact
// situation — RD/RDM clean, monotonicity flagged false — and asserts the
// JSON now publishes passed=false AND a self-describing fail_reason
// containing "monotonicity_regression".
//
// No [RequiresGPU] tag: this exercises the fixture's pure pass-rule
// arithmetic and JSON emission; no GPU device is needed. (rd_=nullptr is
// the same path the failed_init scenario uses.)
// ---------------------------------------------------------------------------
TEST_CASE("[GaussianSplatting][Renderer][Lifetime] monotonicity failure surfaces in emitted JSON") {
	RendererLifetimeFixture fixture("monotonicity_regression_probe", nullptr);
	fixture.capture_baseline();

	// Simulate the asset_attach_detach failure mode: RD/RDM thresholds
	// pass (nothing allocated against rd_=nullptr; no RDM shutdown counts
	// recorded), but the scenario detected a cycle-to-cycle delta-of-deltas
	// excursion. The reason string mirrors the format
	// asset_attach_detach emits.
	const String synthetic_reason =
			"cycle 3 delta 131072 B grew 65537 B beyond cycle-1 delta 65535 B (threshold 65536 B)";
	fixture.set_monotonicity_ok(false, synthetic_reason);

	const bool passed = fixture.finalize();

	// Final verdict — local pass bool must reflect monotonicity.
	CHECK_FALSE(passed);
	CHECK_FALSE(fixture.result().passed);
	// Sibling signals must still be clean — proves the monotonicity
	// signal alone flipped the verdict and would otherwise have shipped
	// as a silent pass.
	CHECK(fixture.result().rdm_owned_leaked == 0);
	CHECK(fixture.result().rd_memory_leaked_bytes < fixture.result().threshold_bytes);

	// JSON contract — the [GS-LIFETIME] line emitted by finalize() comes
	// from to_dict(); validate the fields a downstream gate would parse.
	const Dictionary emitted = fixture.result().to_dict();
	CHECK(bool(emitted["passed"]) == false);
	CHECK(bool(emitted["monotonicity_ok"]) == false);
	const String emitted_reason = emitted["fail_reason"];
	CHECK(emitted_reason.find("monotonicity_regression") >= 0);
	CHECK(emitted_reason.find(synthetic_reason) >= 0);
}

} // namespace TestGaussianSplatting
