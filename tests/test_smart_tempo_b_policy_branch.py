"""Positive and adversarial nested Policy source/runtime branch contracts."""
from __future__ import annotations

import copy
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from audit_smart_tempo_b_policy_branch import (
    BRANCHES, PolicyBranchError, reconcile, verify_source_contract)


def row(stage, reason):
    return {
        "stage": stage, "outcome": "observed", "reason_code": reason,
        "candidate_id": None, "parent_candidate_ids": [],
        "metrics": {"bpm": 0.0, "score": 0.0, "support": 0.0},
    }


def trace(stage, token):
    if stage == "qualification_terminal":
        path = ("qualification_terminal",)
    elif stage == "current_release_terminal":
        path = ("qualification_ready", "current_release_terminal")
    elif stage == "hold_resolution_terminal":
        path = ("qualification_ready", "current_release_continue",
                "hold_resolution_terminal")
    else:
        path = ("qualification_ready", "current_release_continue",
                "hold_resolution_continue", "default_resolution_terminal")
    return [*(row("policy_stage_path", reason) for reason in path),
            row("policy_branch", token),
            row("policy_resolution", "policy_no_output")]


class NestedPolicyBranchTest(unittest.TestCase):
    def test_every_declared_native_branch_is_valid(self):
        count = 0
        for stage, tokens in BRANCHES.items():
            for token in tokens:
                result = reconcile(trace(stage, token), required=True)
                self.assertEqual(result["native_policy_branch_complete"], 1)
                self.assertEqual(result["policy_branch_token"], token)
                count += 1
        self.assertEqual(count, 28)

    def test_source_contract_covers_every_return(self):
        source = (Path(__file__).resolve().parents[1] /
                  "src/foo_smart_tempo/mir_candidate_pipeline.cpp").read_text(
                      encoding="utf-8")
        self.assertEqual(verify_source_contract(source),
                         {"current_release": 8, "hold_resolution": 17})
        for mutated in (
            source.replace('ci_policy_branch("current_release_01");',
                           'ci_policy_branch("wrong_01");', 1),
            source.replace('ci_policy_branch("hold_resolution_17");',
                           'ci_policy_branch("wrong_17");', 1),
            source.replace('ci_policy_branch("current_release_01");',
                           '/* removed witness */', 1),
            source.replace('ci_policy_branch("default_resolution");',
                           '/* removed default */', 1),
            source.replace('g_ci_policy_branch_token = nullptr;',
                           '/* no reset */', 1),
        ):
            with self.assertRaises(PolicyBranchError):
                verify_source_contract(mutated)

    def test_missing_branch_fails_closed(self):
        rows = trace("current_release_terminal", "current_release_01")
        with self.assertRaises(PolicyBranchError):
            reconcile([r for r in rows if r["stage"] != "policy_branch"],
                      required=True)
        self.assertEqual(reconcile([], required=False),
                         {"native_policy_branch_complete": 0})

    def test_wrong_stage_token_and_order_fail(self):
        rows = trace("hold_resolution_terminal", "hold_resolution_05")
        for changed in (
            lambda a: a[-2].__setitem__("reason_code", "current_release_01"),
            lambda a: a[-2].__setitem__("reason_code", "unknown_branch"),
            lambda a: a.insert(-1, copy.deepcopy(a[-2])),
            lambda a: a.insert(0, a.pop(-2)),
            lambda a: a[-2]["metrics"].__setitem__("score", 1.0),
            lambda a: a[-2].__setitem__("parent_candidate_ids", ["c0"]),
            lambda a: a[-2].__setitem__("candidate_id", "c0"),
            lambda a: a[-2].__setitem__("outcome", "selected"),
        ):
            altered = copy.deepcopy(rows)
            changed(altered)
            with self.assertRaises(PolicyBranchError):
                reconcile(altered, required=True)


if __name__ == "__main__":
    unittest.main()
