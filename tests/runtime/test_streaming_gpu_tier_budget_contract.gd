extends SceneTree

const FAIL_MARKER := "[RUNTIME_FAIL]"
const METRICS_MARKER := "[RUNTIME_METRICS]"
const StreamingGpuTierBudget = preload("streaming_gpu_tier_budget.gd")

var failures: Array[String] = []


func _init() -> void:
	call_deferred("_run")


func _record_failure(reason: String, context: Dictionary = {}) -> void:
	var message := reason
	if not context.is_empty():
		message = "%s | context=%s" % [reason, str(context)]
	failures.append(message)
	push_error("%s %s" % [FAIL_MARKER, message])


func _base_tier_result() -> Dictionary:
	return {
		"first_visible_ms": 240.0,
		"frame_p95_ms": 292.4,
		"frame_p95_to_avg_ratio": 1.24,
		"source_data_available": true,
		"fallback_rate_available": true,
		"fallback_rate": 0.0,
		"source_frame_count": 120
	}


func _base_residency() -> Dictionary:
	return {
		"residency_ratio": 1.0
	}


func _merge(base: Dictionary, overrides: Dictionary) -> Dictionary:
	var merged := base.duplicate(true)
	for key in overrides.keys():
		merged[key] = overrides[key]
	return merged


func _array_equals(actual: Array, expected: Array) -> bool:
	if actual.size() != expected.size():
		return false
	for index in range(actual.size()):
		if String(actual[index]) != String(expected[index]):
			return false
	return true


func _run_case(test_case: Dictionary) -> void:
	var tier := StreamingGpuTierBudget.tier_1m_budget()
	tier = _merge(tier, test_case.get("tier_overrides", {}))
	var tier_result := _merge(_base_tier_result(), test_case.get("result_overrides", {}))
	var residency := _merge(_base_residency(), test_case.get("residency_overrides", {}))

	var evaluation := StreamingGpuTierBudget.evaluate_tier_budget(tier, tier_result, residency)
	var expected_within := bool(test_case.get("within_budget", false))
	var actual_within := bool(evaluation.get("within_budget", false))
	if actual_within != expected_within:
		_record_failure("Budget contract within_budget mismatch", {
			"case": test_case.get("name", "<unnamed>"),
			"expected": expected_within,
			"actual": actual_within,
			"evaluation": evaluation
		})

	var expected_budget_failures: Array = test_case.get("budget_failures", [])
	var actual_budget_failures: Array = evaluation.get("budget_failures", [])
	if not _array_equals(actual_budget_failures, expected_budget_failures):
		_record_failure("Budget contract budget_failures mismatch", {
			"case": test_case.get("name", "<unnamed>"),
			"expected": expected_budget_failures,
			"actual": actual_budget_failures,
			"evaluation": evaluation
		})

	var expected_telemetry_failures: Array = test_case.get("telemetry_failures", [])
	var actual_telemetry_failures: Array = evaluation.get("telemetry_failures", [])
	if not _array_equals(actual_telemetry_failures, expected_telemetry_failures):
		_record_failure("Budget contract telemetry_failures mismatch", {
			"case": test_case.get("name", "<unnamed>"),
			"expected": expected_telemetry_failures,
			"actual": actual_telemetry_failures,
			"evaluation": evaluation
		})


func _run() -> void:
	var cases := [
		{
			"name": "clean_current_result",
			"within_budget": true,
			"budget_failures": [],
			"telemetry_failures": []
		},
		{
			"name": "frame_p95_exceeded",
			"result_overrides": {"frame_p95_ms": 325.001},
			"within_budget": false,
			"budget_failures": ["frame_p95_exceeded"],
			"telemetry_failures": []
		},
		{
			"name": "frame_p95_to_avg_ratio_high",
			"result_overrides": {"frame_p95_to_avg_ratio": 2.251},
			"within_budget": false,
			"budget_failures": ["frame_p95_to_avg_ratio_high"],
			"telemetry_failures": []
		},
		{
			"name": "first_visible_missing",
			"result_overrides": {"first_visible_ms": -1.0},
			"within_budget": false,
			"budget_failures": ["first_visible_missing"],
			"telemetry_failures": []
		},
		{
			"name": "first_visible_exceeded",
			"result_overrides": {"first_visible_ms": 3500.001},
			"within_budget": false,
			"budget_failures": ["first_visible_exceeded"],
			"telemetry_failures": []
		},
		{
			"name": "fallback_source_data_missing",
			"result_overrides": {"source_data_available": false},
			"within_budget": false,
			"budget_failures": [],
			"telemetry_failures": ["fallback_source_data_missing"]
		},
		{
			"name": "fallback_rate_missing_flag",
			"result_overrides": {"fallback_rate_available": false, "fallback_rate": null},
			"within_budget": false,
			"budget_failures": [],
			"telemetry_failures": ["fallback_rate_missing"]
		},
		{
			"name": "fallback_rate_high",
			"result_overrides": {"fallback_rate": 0.3501},
			"within_budget": false,
			"budget_failures": ["fallback_rate_high"],
			"telemetry_failures": []
		},
		{
			"name": "residency_ratio_low",
			"residency_overrides": {"residency_ratio": 0.749},
			"within_budget": false,
			"budget_failures": ["residency_ratio_low"],
			"telemetry_failures": []
		}
	]

	for test_case in cases:
		_run_case(test_case)

	var summary := {
		"status": "passed" if failures.is_empty() else "failed",
		"cases": cases.size(),
		"failures": failures,
		"tier_1m_max_frame_p95_ms": float(StreamingGpuTierBudget.tier_1m_budget().get("max_frame_p95_ms", 0.0)),
		"sort_evidence_contract": "covered_by_gpu_streaming_stress_validate_sort_metrics"
	}
	print("%s %s" % [METRICS_MARKER, JSON.stringify(summary)])

	if failures.is_empty():
		print("GPU streaming tier budget contract checks passed.")
		quit(0)
	else:
		quit(1)
