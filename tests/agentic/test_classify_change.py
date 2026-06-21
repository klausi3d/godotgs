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

    def test_local_module_is_r1(self):
        self.assertEqual(self._cls(["modules/gaussian_splatting/logger/logger.cpp"]), "R1")

    def test_tests_path_is_r1(self):
        self.assertEqual(self._cls(["tests/ci/run_module_tests.py"]), "R1")

    def test_docs_is_r0(self):
        self.assertEqual(self._cls(["docs/governance/review-policy.md"]), "R0")

    def test_unknown_sensitive_path_fails_closed_to_r3(self):
        self.assertEqual(self._cls(["some/unmapped/path.bin"]), "R3")

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
