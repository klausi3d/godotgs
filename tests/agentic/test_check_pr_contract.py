#!/usr/bin/env python3
"""Unit tests for scripts/agentic/check_pr_contract.py."""

from __future__ import annotations

import copy
import importlib.util
import json
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts" / "agentic" / "check_pr_contract.py"
spec = importlib.util.spec_from_file_location("check_pr_contract", SCRIPT)
assert spec and spec.loader
cpc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cpc)

POLICY = json.loads((ROOT / ".agentic" / "policy.json").read_text(encoding="utf-8"))
TASK_SCHEMA = json.loads((ROOT / ".agentic" / "schemas" / "task.schema.json").read_text(encoding="utf-8"))
TEMPLATE = json.loads((ROOT / ".agentic" / "templates" / "task.json").read_text(encoding="utf-8"))


def _hard(errors):
    return [e for e in errors if not e.startswith("note:")]


class CheckPrContractTest(unittest.TestCase):
    def test_valid_contract_matching_risk(self):
        errors = cpc.check_contract(
            copy.deepcopy(TEMPLATE),
            POLICY,
            TASK_SCHEMA,
            ["modules/gaussian_splatting/logger/logger.cpp"],  # R1, matches declared R1
        )
        self.assertEqual(_hard(errors), [])

    def test_understated_risk_fails(self):
        # Declared R1 but the diff touches the renderer (R2).
        errors = cpc.check_contract(
            copy.deepcopy(TEMPLATE),
            POLICY,
            TASK_SCHEMA,
            ["modules/gaussian_splatting/renderer/gpu_sorter.cpp"],
        )
        self.assertTrue(any("understates" in e for e in _hard(errors)))

    def test_forbidden_path_change_fails(self):
        # Template forbids modules/gaussian_splatting/persistence/**.
        errors = cpc.check_contract(
            copy.deepcopy(TEMPLATE),
            POLICY,
            TASK_SCHEMA,
            ["modules/gaussian_splatting/persistence/incremental_saver.cpp"],
        )
        self.assertTrue(any("forbidden path" in e for e in _hard(errors)))

    def test_out_of_scope_path_fails(self):
        # Same risk class (R1) but outside the declared owned_paths (logger/**).
        errors = cpc.check_contract(
            copy.deepcopy(TEMPLATE),
            POLICY,
            TASK_SCHEMA,
            ["modules/gaussian_splatting/animation/animation_state_machine.cpp"],
        )
        hard = _hard(errors)
        self.assertTrue(any("outside the declared owned_paths" in e for e in hard))
        self.assertFalse(any("understates" in e for e in hard))

    def test_missing_required_field_fails(self):
        contract = copy.deepcopy(TEMPLATE)
        del contract["rollback_plan"]
        errors = cpc.check_contract(contract, POLICY, TASK_SCHEMA, ["modules/gaussian_splatting/logger/x.cpp"])
        self.assertTrue(any("rollback_plan" in e for e in _hard(errors)))

    def test_empty_validation_commands_fails(self):
        contract = copy.deepcopy(TEMPLATE)
        contract["validation_commands"] = []
        errors = cpc.check_contract(contract, POLICY, TASK_SCHEMA, ["modules/gaussian_splatting/logger/x.cpp"])
        self.assertTrue(any("validation_commands" in e for e in _hard(errors)))

    def test_unknown_risk_class_value_fails_schema(self):
        contract = copy.deepcopy(TEMPLATE)
        contract["risk_class"] = "R9"
        errors = cpc.check_contract(contract, POLICY, TASK_SCHEMA, None)
        self.assertTrue(any("risk_class" in e for e in _hard(errors)))

    def test_stacked_pr_requires_base_fields(self):
        contract = copy.deepcopy(TEMPLATE)
        contract["stacked_on"] = {"base_pr": "", "base_sha": ""}
        errors = cpc.check_contract(contract, POLICY, TASK_SCHEMA, ["modules/gaussian_splatting/logger/x.cpp"])
        self.assertTrue(any("stacked_on.base_pr" in e for e in _hard(errors)))
        self.assertTrue(any("stacked_on.base_sha" in e for e in _hard(errors)))

    def test_r3_emits_adr_note_but_no_hard_error(self):
        contract = copy.deepcopy(TEMPLATE)
        contract["risk_class"] = "R3"
        contract["owned_paths"] = ["servers/rendering/foo.cpp"]
        contract["forbidden_paths"] = []
        errors = cpc.check_contract(contract, POLICY, TASK_SCHEMA, ["servers/rendering/foo.cpp"])
        self.assertTrue(any(e.startswith("note:") and "design record" in e for e in errors))
        self.assertEqual(_hard(errors), [])


if __name__ == "__main__":
    unittest.main()
