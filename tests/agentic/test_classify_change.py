#!/usr/bin/env python3
"""Unit tests for scripts/agentic/classify_change.py."""

from __future__ import annotations

import importlib.util
import json
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts" / "agentic" / "classify_change.py"
spec = importlib.util.spec_from_file_location("classify_change", SCRIPT)
assert spec and spec.loader
classify = importlib.util.module_from_spec(spec)
spec.loader.exec_module(classify)

POLICY = json.loads((ROOT / ".agentic" / "policy.json").read_text(encoding="utf-8"))


class ClassifyChangeTest(unittest.TestCase):
    def _cls(self, paths):
        return classify.classify_paths(paths, POLICY)[0]

    def test_engine_path_is_r3(self):
        self.assertEqual(self._cls(["servers/rendering/foo.cpp"]), "R3")

    def test_persistence_path_is_r3(self):
        self.assertEqual(self._cls(["modules/gaussian_splatting/io/ply_loader.cpp"]), "R3")

    def test_renderer_path_is_r2(self):
        self.assertEqual(self._cls(["modules/gaussian_splatting/renderer/gpu_sorter.cpp"]), "R2")

    def test_shaders_path_is_r2(self):
        self.assertEqual(self._cls(["modules/gaussian_splatting/shaders/raster.glsl"]), "R2")

    def test_core_streaming_is_r2(self):
        # Streaming/VRAM files under core/ are R2, not R1.
        self.assertEqual(self._cls(["modules/gaussian_splatting/core/gaussian_streaming.cpp"]), "R2")
        self.assertEqual(self._cls(["modules/gaussian_splatting/core/streaming_vram_regulator.h"]), "R2")
        self.assertEqual(self._cls(["modules/gaussian_splatting/core/residency_budget_controller.cpp"]), "R2")

    def test_core_nonstreaming_is_r1(self):
        # Ordinary core data files stay R1.
        self.assertEqual(self._cls(["modules/gaussian_splatting/core/gaussian_data.cpp"]), "R1")

    def test_local_module_is_r1(self):
        self.assertEqual(self._cls(["modules/gaussian_splatting/logger/logger.cpp"]), "R1")

    def test_ordinary_test_path_is_r1(self):
        self.assertEqual(self._cls(["tests/ci/test_ply_loader_ci.gd"]), "R1")

    def test_ci_gate_machinery_is_r3(self):
        # The deterministic-check / release-gate runners must not be downgradable at R1.
        self.assertEqual(self._cls(["tests/ci/run_module_tests.py"]), "R3")
        self.assertEqual(self._cls(["tests/runtime/run_runtime_validation.py"]), "R3")
        self.assertEqual(self._cls(["tests/ci/check_renderer_release_gates.py"]), "R3")

    def test_docs_is_r0(self):
        self.assertEqual(self._cls(["docs/governance/review-policy.md"]), "R0")

    def test_root_doc_is_r0(self):
        self.assertEqual(self._cls(["README.md"]), "R0")
        self.assertEqual(self._cls(["CONTRIBUTING.md"]), "R0")

    def test_unknown_sensitive_path_fails_closed_to_r3(self):
        self.assertEqual(self._cls(["some/unmapped/path.bin"]), "R3")

    def test_unmapped_markdown_fails_closed_to_r3(self):
        # A markdown file outside the known doc/root scopes must not slip to R0 via a
        # blanket *.md rule; unrecognized paths fail closed to R3.
        self.assertEqual(self._cls(["weird/unmapped/notes.md"]), "R3")

    def test_overall_is_max_across_paths(self):
        self.assertEqual(
            self._cls(["docs/x.md", "modules/gaussian_splatting/renderer/a.cpp"]),
            "R2",
        )
        self.assertEqual(
            self._cls(["docs/x.md", "servers/y.cpp"]),
            "R3",
        )

    def test_empty_changeset_is_lowest_class(self):
        self.assertEqual(self._cls([]), "R0")

    def test_windows_separators_are_normalized(self):
        self.assertEqual(self._cls([r"modules\gaussian_splatting\renderer\a.cpp"]), "R2")


if __name__ == "__main__":
    unittest.main()
