"""Synthetic cross-stage native source-fate oracle: positive and adversarial."""
from __future__ import annotations
import copy
from pathlib import Path
import sys
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from audit_smart_tempo_b_cross_stage_fates import CrossStageFateError, reconcile

def r(stage, cid=None, parents=None, reason="", outcome="observed", bpm=128.,
      score=0, support=0, **extras):
    return dict(stage=stage, candidate_id=cid, parent_candidate_ids=parents or [],
                reason_code=reason, outcome=outcome,
                metrics=dict(bpm=bpm, score=score, support=support, **extras))

def sample():
    return [
        r("candidate_create", "c0"), r("candidate_create", "c1", bpm=64.),
        r("candidate_transform", "c2", ["c0"]),
        r("candidate_transform", "c3", ["c1"], bpm=64.),
        r("policy_filter", "c2", outcome="included"),
        r("policy_filter", "c3", outcome="excluded", bpm=64.),
        r("policy_eligibility", "c4", ["c2"]),
        r("candidate_filter", "c3", reason="low_upper_branch_veto",
          outcome="excluded", bpm=64.),
        r("primary_rank", "c2", reason="harmonic_rank_scored"),
        r("primary_terminal", "c2", reason="terminal_lane_choice"),
        r("primary_terminal", "c2", reason="terminal_final_choice"),
        r("primary_veto_ledger", "c1", reason="raw_alias_decisive_veto",
          outcome="excluded", bpm=64., support=2),
        r("primary_veto_ledger", "c3", reason="low_upper_branch_veto",
          outcome="excluded", bpm=64., support=4),
        r("policy_pointer", parents=["c4"]),
        r("writer_resolution", parents=["c2"], reason="mir_primary"),
        r("candidate_fate", "c2", reason="cross_stage_observation_not_global_eligibility",
          score=1+2+4+8+16+64+128, raw_veto_count=0,
          cluster_veto_count=0, first_veto_event_index=0,
          last_veto_event_index=0, raw_selector_event_count=0,
          cluster_selector_event_count=3, first_selector_event_index=2,
          last_selector_event_index=4),
        r("candidate_fate", "c3", reason="cross_stage_observation_not_global_eligibility",
          bpm=64., score=128+256+512, support=2, raw_veto_count=1,
          cluster_veto_count=1, first_veto_event_index=2,
          last_veto_event_index=4, raw_selector_event_count=0,
          cluster_selector_event_count=1, first_selector_event_index=1,
          last_selector_event_index=1)
    ]

class CrossStageFatesTests(unittest.TestCase):
    def test_complete_actual_object_source_matrix(self):
        self.assertEqual(reconcile(sample(), required=True), dict(
            cross_stage_fate_rows=2, cross_stage_fate_veto_sources=1,
            cross_stage_fate_writer_sources=1))

    def test_writer_override_object_not_bpm_alias(self):
        rows = sample()
        writer = next(x for x in rows if x["stage"] == "writer_resolution")
        writer["reason_code"] = "mir_writer_override"
        writer["parent_candidate_ids"] = ["c4"]
        primary_fate = next(x for x in rows if x["stage"] == "candidate_fate"
                            and x["candidate_id"] == "c2")
        primary_fate["metrics"]["score"] = 1+2+4+8+32+64+128
        self.assertEqual(reconcile(rows, required=True)["cross_stage_fate_writer_sources"], 1)

    def test_vetoed_source_is_not_globally_ineligible(self):
        rows = sample()
        self.assertEqual(reconcile(rows, required=True)["cross_stage_fate_veto_sources"], 1)

    def test_fail_closed_mutations(self):
        changes = (
            lambda a: a[-1]["metrics"].__setitem__("score", 0),
            lambda a: a[-1]["metrics"].__setitem__("raw_veto_count", 0),
            lambda a: a[-1]["metrics"].__setitem__("cluster_veto_count", 0),
            lambda a: a[-1]["metrics"].__setitem__("first_veto_event_index", 3),
            lambda a: a[-1]["metrics"].__setitem__("last_veto_event_index", 3),
            lambda a: a[-1]["metrics"].__setitem__("raw_selector_event_count", 1),
            lambda a: a[-1]["metrics"].__setitem__("cluster_selector_event_count", 2),
            lambda a: a[-1]["metrics"].__setitem__("first_selector_event_index", 2),
            lambda a: a[-1]["metrics"].__setitem__("last_selector_event_index", 2),
            lambda a: a[-1].__setitem__("candidate_id", "c2"),
            lambda a: a[-1].__setitem__("reason_code", "global_veto"),
            lambda a: a[-1].__setitem__("parent_candidate_ids", ["c1"]),
            lambda a: a.pop(),
            lambda a: a.append(copy.deepcopy(a[-1])),
            lambda a: next(x for x in a if x["stage"] == "policy_pointer").__setitem__(
                "parent_candidate_ids", []),
            lambda a: next(x for x in a if x["stage"] == "writer_resolution").__setitem__(
                "parent_candidate_ids", ["c4"]),
            lambda a: next(x for x in a if x["stage"] == "policy_filter"
                           and x["candidate_id"] == "c3").__setitem__("outcome", "included"),
        )
        for change in changes:
            with self.subTest(change=str(change)):
                rows = sample()
                change(rows)
                with self.assertRaises(CrossStageFateError):
                    reconcile(rows, required=True)

    def test_selector_chronology_is_not_merely_a_membership_bit(self):
        rows = sample()
        rows.insert(8, r("candidate_filter", "c3", reason="low_threshold_allowed",
                         outcome="included", bpm=64.))
        with self.assertRaises(CrossStageFateError):
            reconcile(rows, required=True)

    def test_empty_board_and_optional_legacy(self):
        self.assertEqual(reconcile([], required=True)["cross_stage_fate_rows"], 0)
        self.assertEqual(reconcile([r("candidate_transform", "c0")],
                                   required=False)["cross_stage_fate_rows"], 0)
        with self.assertRaises(CrossStageFateError):
            reconcile([r("candidate_transform", "c0")], required=True)

if __name__ == "__main__":
    unittest.main()
