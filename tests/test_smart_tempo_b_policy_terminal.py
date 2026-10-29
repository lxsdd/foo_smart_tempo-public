"""Adversarial independent Policy terminal source/action oracle (no audio)."""
import copy
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from audit_smart_tempo_b_policy_terminal import (
    PolicyTerminalError, REASONS, reconcile)


def row(stage, reason, outcome, bpm, score=0, support=0, parents=None,
        action_code=None, sequence=0):
    metrics = dict(bpm=bpm, score=score, support=support)
    if action_code is not None:
        metrics["action_code"] = action_code
    return dict(stage=stage, reason_code=reason, outcome=outcome,
                candidate_id=None, parent_candidate_ids=parents or [],
                metrics=metrics, sequence=sequence)


def case(action=3, reason="measured_material_risk_same_pulse_release",
         outcome="selected", change="writer_override", bpm=128.,
         override=1, release=0, parents=None):
    parents = ["c1"] if parents is None else parents
    return [
        row("policy_resolution", "policy_one_same_bpm", "observed", bpm,
            override, release, parents, sequence=1),
        row("policy_pointer", "pointer_native_fullboard_consistent", "observed",
            bpm, 1, 1, parents, sequence=2),
        row("policy_terminal", reason, outcome, bpm, override, release,
            parents, action_code=action, sequence=3),
        row("policy_change", change, outcome, bpm, 0.2, 0.8,
            parents, sequence=4),
    ]


class NativePolicyTerminalTests(unittest.TestCase):
    def test_positive_writer_override(self):
        result = reconcile(case(), required=True)
        self.assertEqual(result["native_policy_terminal_verified"], 1)
        self.assertEqual(result["native_policy_terminal_action"], "writer_override")

    def test_positive_hold_release(self):
        rows = case(action=4, reason="frozen_soft_center_review_promotion_gate",
                    change="hold_release", override=0, release=1)
        self.assertEqual(reconcile(rows, required=True)["native_policy_terminal_verified"], 1)

    def test_positive_primary_retention(self):
        rows = case(action=1, reason="outside_frozen_policy_surfaces",
                    change="keep_primary", override=0)
        self.assertEqual(reconcile(rows)["native_policy_terminal_action"], "keep_current")

    def test_positive_disabled_no_candidate(self):
        rows = case(action=0, reason="disabled", change="no_policy_write",
                    outcome="rejected", bpm=0., override=0, parents=[])
        self.assertEqual(reconcile(rows)["native_policy_terminal_verified"], 1)

    def test_positive_invariant_veto(self):
        rows = case(action=7, reason="unmeasured_policy_output_veto",
                    change="no_policy_write", outcome="review_hold",
                    bpm=0., override=0, parents=[])
        self.assertEqual(reconcile(rows)["native_policy_terminal_action"], "invariant_review_hold")

    def test_optional_legacy_without_terminal(self):
        rows = [r for r in case() if r["stage"] != "policy_terminal"]
        self.assertEqual(reconcile(rows)["native_policy_terminal_verified"], 0)
        with self.assertRaises(PolicyTerminalError):
            reconcile(rows, required=True)

    def test_adversarial_native_mutations(self):
        base = case()
        changes = [
            lambda a: a[2].__setitem__("reason_code", "unregistered"),
            lambda a: a[2].__setitem__("reason_code", "writer_override"),
            lambda a: a[2]["metrics"].__setitem__("action_code", 99),
            lambda a: a[2]["metrics"].__setitem__("action_code", 4),
            lambda a: a[2]["metrics"].__setitem__("score", 0),
            lambda a: a[2]["metrics"].__setitem__("support", 1),
            lambda a: a[2]["metrics"].__setitem__("bpm", 129),
            lambda a: a[2].__setitem__("outcome", "review_hold"),
            lambda a: a[2].__setitem__("parent_candidate_ids", ["c2"]),
            lambda a: a[2].__setitem__("sequence", 5),
            lambda a: a[3].__setitem__("reason_code", "hold_release"),
            lambda a: a.pop(2),
            lambda a: a.insert(2, copy.deepcopy(a[2])),
        ]
        for i, mutation in enumerate(changes):
            with self.subTest(mutation=i):
                altered = copy.deepcopy(base)
                mutation(altered)
                with self.assertRaises(PolicyTerminalError):
                    reconcile(altered, required=True)

    def test_source_emitted_recovery_terminal_reasons(self):
        # These reviewed labels are literally present in the native Policy
        # source; this is not a blanket acceptance of arbitrary C++ output.
        cpp = (Path(__file__).resolve().parents[1] /
               "src/foo_smart_tempo/mir_candidate_pipeline.cpp").read_text(
                   encoding="utf-8")
        recovery = (
            "dominant_measured_family_conflict_release_gate",
            "measured_residual_downward_family_recovery_gate",
            "measured_strict_direct_downward_family_recovery_gate",
            "measured_winner_half_family_recovery_gate",
            "measured_exact_half_family_recovery_gate",
            "measured_broad_route_cross_view_family_recovery_gate",
        )
        for reason in recovery:
            with self.subTest(reason=reason):
                self.assertIn('"' + reason + '"', cpp)
                self.assertIn(reason, REASONS)
                result = reconcile(case(reason=reason), required=True)
                self.assertEqual(result["native_policy_terminal_action"],
                                 "writer_override")
                # A native recovery/release reason must never be accepted
                # as a silent keep-current or review-hold action.
                with self.assertRaises(PolicyTerminalError):
                    reconcile(case(reason=reason, action=1,
                                   change="keep_primary", override=0),
                              required=True)

    def test_source_emitted_high_pulse_retention_reason(self):
        reason = "measured_independent_high_pulse_retention_gate"
        cpp = (Path(__file__).resolve().parents[1] /
               "src/foo_smart_tempo/mir_candidate_pipeline.cpp").read_text(
                   encoding="utf-8")
        self.assertIn('"' + reason + '"', cpp)
        self.assertIn(reason, REASONS)
        retained = case(reason=reason, action=1, change="keep_primary",
                        override=0)
        self.assertEqual(reconcile(retained, required=True)[
            "native_policy_terminal_action"], "keep_current")
        with self.assertRaises(PolicyTerminalError):
            reconcile(case(reason=reason), required=True)

    def test_static_reason_catalog_covers_native_release_families(self):
        self.assertGreaterEqual(len(REASONS), 55)
        for reason in ("measured_partial_bar_cross_view_bridge_release",
                       "measured_partial_bar_topk_exact_center_release",
                       "measured_material_risk_local_exact_release",
                       "unmeasured_policy_output_veto",
                       "protected_review_hold_reason"):
            self.assertIn(reason, REASONS)


if __name__ == "__main__":
    unittest.main()
