#!/usr/bin/env python3
"""Unit tests for runtime renderer-proof report contracts."""

from __future__ import annotations

import importlib.util
import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tests" / "runtime" / "run_runtime_validation.py"
spec = importlib.util.spec_from_file_location("run_runtime_validation", SCRIPT)
assert spec and spec.loader
runtime_validation = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = runtime_validation
spec.loader.exec_module(runtime_validation)


def _result(name: str, metrics: dict[str, object], status: str = "passed"):
    return runtime_validation.TestResult(
        name=name,
        command=["godot", "--script", "test.gd"],
        duration=0.1,
        exit_code=0,
        stdout="",
        stderr="",
        status=status,
        reasons=[],
        metrics=metrics,
    )


class RuntimeRendererProofContractTests(unittest.TestCase):
    def test_required_renderer_proof_passes_with_canonical_pass(self) -> None:
        summary = runtime_validation._build_renderer_proof_summary(
            [
                _result(
                    "Canonical Node Asset Render",
                    {
                        "renderer_proof_kind": "canonical_node_asset",
                        "renderer_proof_status": "passed",
                        "asset_path": "res://tests/fixtures/test_splats.ply",
                        "visible_splats_max": 1024,
                        "visual_luma_variance_max": 0.01,
                    },
                )
            ],
            required=True,
        )

        self.assertEqual(summary["status"], "passed")
        self.assertEqual(summary["passed"], 1)
        self.assertEqual(summary["failure_reasons"], [])

    def test_required_renderer_proof_fails_when_unavailable(self) -> None:
        summary = runtime_validation._build_renderer_proof_summary(
            [
                _result(
                    "Canonical Node Asset Render",
                    {
                        "renderer_proof_kind": "canonical_node_asset",
                        "renderer_proof_status": "skipped_unavailable",
                        "reason": "local RenderingDevice required",
                    },
                    status="skipped",
                )
            ],
            required=True,
        )

        self.assertEqual(summary["status"], "failed")
        self.assertEqual(summary["passed"], 0)
        self.assertEqual(summary["unavailable"], 1)
        self.assertTrue(summary["failure_reasons"])

    def test_required_renderer_proof_fails_without_proof_metrics(self) -> None:
        summary = runtime_validation._build_renderer_proof_summary(
            [_result("Unrelated Runtime Test", {"status": "passed"})],
            required=True,
        )

        self.assertEqual(summary["status"], "failed")
        self.assertEqual(summary["total"], 0)
        self.assertTrue(any("No renderer proof metrics" in reason for reason in summary["failure_reasons"]))

    def test_summary_schema_accepts_renderer_proof_object(self) -> None:
        summary = {
            "total": 1,
            "passed": 1,
            "failed": 0,
            "skipped": 0,
            "duration": 0.1,
            "tests": [
                {
                    "name": "Canonical Node Asset Render",
                    "status": "passed",
                    "reasons": [],
                    "command": ["godot"],
                    "duration": 0.1,
                    "exit_code": 0,
                    "metrics": {},
                }
            ],
            "renderer_proof": runtime_validation._build_renderer_proof_summary([], required=False),
        }

        self.assertEqual(runtime_validation._validate_summary_schema(summary), [])


if __name__ == "__main__":
    unittest.main()
