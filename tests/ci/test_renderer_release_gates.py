#!/usr/bin/env python3
"""Unit tests for renderer release gate checker."""

from __future__ import annotations

import importlib.util
import json
import tempfile
import unittest
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tests" / "ci" / "check_renderer_release_gates.py"
spec = importlib.util.spec_from_file_location("check_renderer_release_gates", SCRIPT)
assert spec and spec.loader
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)


def _write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def _base_manifest(root: Path) -> dict[str, Any]:
    _write(
        root / "modules/gaussian_splatting/tests/test_gpu.h",
        'TEST_CASE("[GaussianSplatting][RequiresGPU][HazardRepro] Smoke") {}\n',
    )
    tests = checker._extract_requires_gpu_tests(root)
    count, digest = checker._requires_gpu_snapshot(tests)
    for rel in (
        "docs/reference/renderer-release-gates.md",
        "docs/development/known-public-alpha-limitations.md",
        "docs/performance/index.md",
        "docs/assets/data/benchmark_latest.json",
        "tests/visual_baselines/README.md",
        "tests/runtime/runtime_scenarios.json",
        "tests/visual_baselines/composite_hazard_256x256.png",
        ".github/workflows/gaussian_production_gates.yml",
        ".github/workflows/baseline_qa.yml",
        ".github/workflows/release_builds.yml",
    ):
        _write(root / rel, "guards\nmodule-validation\nRun GPU harness\nbuild_linux\nbuild_windows\npublish_release\n")
    _write(
        root / "tests/ci/run_gpu_harness.py",
        "from dataclasses import dataclass\n"
        "@dataclass(frozen=True)\n"
        "class BatchSpec:\n"
        "    name: str\n"
        "    filters: tuple[str, ...]\n"
        "BATCHES = (BatchSpec('CompositorHazard', ('*HazardRepro*',)),)\n",
    )
    return {
        "schema_version": 1,
        "public_alpha_predicate": {
            "disallow_path_filter_downgrade": True,
            "disallow_manual_downgrade": True,
            "disallow_missing_gpu_runner_downgrade": True,
            "disallow_open_world_advisory_only": True,
            "required_issue_query": {
                "blocking_labels_any": ["priority:P0", "release blocker"],
                "alpha_relevant_p1_labels_all": ["priority:P1", "alpha-relevant"],
            },
        },
        "known_limitations_page": "docs/development/known-public-alpha-limitations.md",
        "references": [
            "docs/reference/renderer-release-gates.md",
            "docs/development/known-public-alpha-limitations.md",
            "docs/performance/index.md",
            "docs/assets/data/benchmark_latest.json",
            "tests/visual_baselines/README.md",
            "tests/runtime/runtime_scenarios.json",
        ],
        "requires_gpu_test_snapshot": {
            "test_count": count,
            "sha256": digest,
            "deferred_tags_any": ["SceneTree", "Importer"],
            "deferred_count": 0,
            "closure_policy": "deferred RequiresGPU coverage blocks closure",
        },
        "deferred_requires_gpu_waivers": [],
        "gpu_harness_policy": {
            "script": "tests/ci/run_gpu_harness.py",
            "required_batches": [
                {
                    "name": "CompositorHazard",
                    "minimum_test_cases": 1,
                    "allow_skips": False,
                    "allow_timeout": False,
                    "allow_rid_leaks": False,
                    "reference_artifacts": ["tests/visual_baselines/composite_hazard_256x256.png"],
                }
            ],
        },
        "workflow_policy": {
            "required_workflows": [
                {
                    "file": ".github/workflows/gaussian_production_gates.yml",
                    "required_jobs": ["guards", "module-validation"],
                    "manual_input_may_disable": False,
                    "path_filter_may_disable": False,
                },
                {
                    "file": ".github/workflows/baseline_qa.yml",
                    "required_jobs": ["Run GPU harness"],
                    "manual_input_may_disable": False,
                    "path_filter_may_disable": False,
                },
                {
                    "file": ".github/workflows/release_builds.yml",
                    "required_jobs": ["build_linux", "build_windows", "publish_release"],
                    "manual_input_may_disable": False,
                    "path_filter_may_disable": False,
                },
            ]
        },
        "visual_acceptance": {
            "tracked_actual_png_allowed": False,
            "blocking_references": [
                {
                    "id": "compositor_hazard_256",
                    "path": "tests/visual_baselines/composite_hazard_256x256.png",
                    "required": True,
                }
            ],
            "candidate_rules": {
                "capture_count_min": 1,
                "capture_reference_match_count_must_equal_capture_count": True,
                "capture_ssim_min_must_be_non_null": True,
                "capture_psnr_min_must_be_non_null": True,
            },
        },
        "benchmark_acceptance": {
            "candidate_required_lanes": ["static_baseline"],
            "required_fields_non_null": ["lane_id", "commit_sha", "godot_binary_commit", "godot_binary_mtime_utc"],
        },
        "artifact_requirements": {
            "required_groups": ["linux_release_archive", "known_limitations_page"],
            "each_artifact_requires": ["path", "sha256", "godot_binary_commit", "godot_binary_mtime_utc"],
        },
    }


class RendererReleaseGateTests(unittest.TestCase):
    def test_repository_contract_passes(self) -> None:
        manifest = checker._load_json(ROOT / "docs/reference/renderer_release_gate_manifest.json")
        failures = checker.validate_contract(ROOT, manifest)
        self.assertEqual([], failures)

    def test_new_requires_gpu_test_fails_snapshot_contract(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = _base_manifest(root)
            _write(
                root / "modules/gaussian_splatting/tests/test_new_gpu.h",
                'TEST_CASE("[GaussianSplatting][RequiresGPU] New") {}\n',
            )
            failures = checker.validate_contract(root, manifest)
            self.assertTrue(any("RequiresGPU test snapshot drift" in item for item in failures))

    def test_missing_visual_reference_fails_contract(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = _base_manifest(root)
            (root / "tests/visual_baselines/composite_hazard_256x256.png").unlink()
            failures = checker.validate_contract(root, manifest)
            self.assertTrue(any("reference missing" in item for item in failures))

    def test_missing_known_limitations_page_fails_contract(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = _base_manifest(root)
            (root / "docs/development/known-public-alpha-limitations.md").unlink()
            failures = checker.validate_contract(root, manifest)
            self.assertTrue(any("known limitations page is missing" in item for item in failures))

    def test_required_batch_matching_zero_tests_fails_contract(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = _base_manifest(root)
            _write(
                root / "tests/ci/run_gpu_harness.py",
                "from dataclasses import dataclass\n"
                "@dataclass(frozen=True)\n"
                "class BatchSpec:\n"
                "    name: str\n"
                "    filters: tuple[str, ...]\n"
                "BATCHES = (BatchSpec('CompositorHazard', ('*NoMatch*',)),)\n",
            )
            failures = checker.validate_contract(root, manifest)
            self.assertTrue(any("matches 0 tests" in item for item in failures))

    def test_candidate_fails_timeout_and_null_metrics(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = _base_manifest(root)
            _write(
                root / "gpu_report.json",
                json.dumps(
                    {
                        "batches": [
                            {
                                "name": "CompositorHazard",
                                "timed_out": True,
                                "summary_parse_ok": True,
                                "rc": 0,
                                "test_cases": {"total": 1, "failed": 0, "skipped": 0},
                                "assertions": {"failed": 0},
                                "rid_leak_bytes": 0,
                            }
                        ]
                    }
                ),
            )
            _write(
                root / "bench.json",
                json.dumps([{"lane_id": "static_baseline", "commit_sha": None, "capture_count": 0}]),
            )
            evidence = {
                "commit": "abc",
                "commit_time_utc": "2026-05-19T10:00:00Z",
                "gpu_harness_report": "gpu_report.json",
                "benchmark_report": "bench.json",
                "artifacts": {
                    "linux_release_archive": {
                        "path": "x",
                        "sha256": "y",
                        "godot_binary_commit": "abc",
                        "godot_binary_mtime_utc": "2026-05-19T10:01:00Z",
                    },
                    "known_limitations_page": {
                        "path": "x",
                        "sha256": "y",
                        "godot_binary_commit": "abc",
                        "godot_binary_mtime_utc": "2026-05-19T10:01:00Z",
                    },
                },
            }
            failures = checker.validate_candidate(root, manifest, evidence)
            self.assertTrue(any("timed out" in item for item in failures))
            self.assertTrue(any("null/missing commit_sha" in item for item in failures))
            self.assertTrue(any("has no visual captures" in item for item in failures))

    def test_candidate_fails_stale_and_incomplete_artifacts(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = _base_manifest(root)
            _write(
                root / "gpu_report.json",
                json.dumps(
                    {
                        "batches": [
                            {
                                "name": "CompositorHazard",
                                "timed_out": False,
                                "summary_parse_ok": True,
                                "rc": 0,
                                "test_cases": {"total": 1, "failed": 0, "skipped": 0},
                                "assertions": {"failed": 0},
                                "rid_leak_bytes": 0,
                            }
                        ]
                    }
                ),
            )
            _write(
                root / "bench.json",
                json.dumps(
                    [
                        {
                            "lane_id": "static_baseline",
                            "commit_sha": "abc",
                            "godot_binary_commit": "abc",
                            "godot_binary_mtime_utc": "2026-05-19T10:01:00Z",
                            "capture_count": 1,
                            "capture_reference_match_count": 1,
                            "capture_ssim_min": 0.99,
                            "capture_psnr_min": 40.0,
                            "gpu_timing_available": False,
                            "gpu_timing_source": "unavailable",
                        }
                    ]
                ),
            )
            evidence = {
                "commit": "abc",
                "commit_time_utc": "2026-05-19T10:00:00Z",
                "gpu_harness_report": "gpu_report.json",
                "benchmark_report": "bench.json",
                "artifacts": {
                    "linux_release_archive": {
                        "path": "x",
                        "sha256": "y",
                        "godot_binary_commit": "old",
                        "godot_binary_mtime_utc": "2026-05-19T09:00:00Z",
                    }
                },
            }
            failures = checker.validate_candidate(root, manifest, evidence)
            self.assertTrue(any("built from stale commit" in item for item in failures))
            self.assertTrue(any("binary predates commit" in item for item in failures))
            self.assertTrue(any("artifact group missing: known_limitations_page" in item for item in failures))

    def test_candidate_issue_classification_uses_live_labels(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = _base_manifest(root)
            evidence = {"artifacts": {}, "issue_classifications": {"350": {"status": "blocking"}}}
            issues = [{"number": 350, "state": "OPEN", "labels": [{"name": "release blocker"}]}]
            failures = checker.validate_candidate(root, manifest, evidence, issues)
            self.assertTrue(any("issue #350 is still blocking" in item for item in failures))


if __name__ == "__main__":
    unittest.main()
