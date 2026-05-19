extends RefCounted

# Shared runtime budget contract for the streaming GPU stress gate.
# Sort-evidence checks intentionally stay in test_gpu_streaming_stress.gd's
# GPU-backed _validate_sort_metrics() path; this helper only owns the tier
# budget and telemetry invariants evaluated from already-collected metrics.

static func tier_1m_budget() -> Dictionary:
	return {
		"name": "tier_1m",
		"size": 1000000,
		"max_first_visible_ms": 3500.0,
		"min_residency_ratio": 0.75,
		# Runner-specific end-to-end guardrail, not a renderer product target.
		# Current clean Windows Vulkan evidence has tier_1m p95 at 236.772 ms
		# (PR #361 run 26112198038), 240.731 ms (master run 26095036307),
		# 292.4 ms (checked-in report), and 304.903 ms (PR #351 run
		# 26112075257) with residency=1.0, fallback_rate=0.0, and telemetry
		# present. 325 ms stays above that current self-hosted runner envelope
		# while preserving a hard ceiling for gross regressions. Tighten this in
		# a dedicated calibration PR after several clean master runs establish a
		# lower stable envelope.
		"max_frame_p95_ms": 325.0,
		"max_frame_p95_to_avg_ratio": 2.25,
		"max_fallback_rate": 0.35,
		"enforce": true
	}


static func evaluate_tier_budget(tier: Dictionary, tier_result: Dictionary, residency: Dictionary) -> Dictionary:
	var budget_failures: Array[String] = []
	var telemetry_failures: Array[String] = []
	var within_budget := true

	var first_visible_ms := float(tier_result.get("first_visible_ms", -1.0))
	var residency_ratio := float(residency.get("residency_ratio", 0.0))
	var frame_p95_ms := float(tier_result.get("frame_p95_ms", 0.0))
	var frame_p95_to_avg_ratio := float(tier_result.get("frame_p95_to_avg_ratio", 0.0))
	var source_data_available := bool(tier_result.get("source_data_available", false))
	var fallback_rate_available := bool(tier_result.get("fallback_rate_available", false))
	var fallback_rate: Variant = tier_result.get("fallback_rate", null)

	var max_first_visible_ms := float(tier.get("max_first_visible_ms", 0.0))
	if first_visible_ms < 0.0:
		within_budget = false
		budget_failures.append("first_visible_missing")
	elif max_first_visible_ms > 0.0 and first_visible_ms > max_first_visible_ms:
		within_budget = false
		budget_failures.append("first_visible_exceeded")

	var min_residency_ratio := float(tier.get("min_residency_ratio", 0.0))
	if residency_ratio < min_residency_ratio:
		within_budget = false
		budget_failures.append("residency_ratio_low")

	var max_frame_p95_ms := float(tier.get("max_frame_p95_ms", 0.0))
	if max_frame_p95_ms > 0.0 and frame_p95_ms > max_frame_p95_ms:
		within_budget = false
		budget_failures.append("frame_p95_exceeded")
	var max_frame_p95_to_avg_ratio := float(tier.get("max_frame_p95_to_avg_ratio", 0.0))
	if max_frame_p95_to_avg_ratio > 0.0 and frame_p95_to_avg_ratio > max_frame_p95_to_avg_ratio:
		within_budget = false
		budget_failures.append("frame_p95_to_avg_ratio_high")

	var max_fallback_rate := float(tier.get("max_fallback_rate", 1.0))
	if not source_data_available:
		within_budget = false
		telemetry_failures.append("fallback_source_data_missing")
	elif not fallback_rate_available:
		within_budget = false
		telemetry_failures.append("fallback_rate_missing")
	elif fallback_rate == null:
		within_budget = false
		telemetry_failures.append("fallback_rate_missing")
	elif float(fallback_rate) > max_fallback_rate:
		within_budget = false
		budget_failures.append("fallback_rate_high")

	return {
		"within_budget": within_budget,
		"budget_failures": budget_failures,
		"telemetry_failures": telemetry_failures
	}
