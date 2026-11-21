"""Source-ID exact candidate-fate replay: positive and adversarial tests.

All inputs are invented CI candidate objects. These tests do not measure BPM.
"""
from __future__ import annotations
import copy
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from audit_smart_tempo_ci_pipeline_trace import TraceError
from audit_smart_tempo_b_candidate_fates import reconcile, FATES


def row(stage, cid=None, reason="", outcome="observed", score=0.0,
        epoch=None):
    m = {"bpm": 60.0, "score": score, "support": 0.0}
    if epoch is not None:
        m["low_pool_epoch"] = epoch
    return {"stage": stage, "candidate_id": cid, "reason_code": reason,
            "outcome": outcome, "metrics": m}


def rank_pair(cid, family, score, won, epoch=None):
    return [
        row("primary_rank", cid, family + "_rank_scored", "included",
            score, epoch),
        row("primary_compare", cid, family + "_compare_" +
            ("won" if won else "lost"),
            "included" if won else "excluded", score, epoch),
    ]


def fixture():
    rows = [row("candidate_transform", "c" + str(i))
            for i in range(5)]
    rows += rank_pair("c0", "harmonic", 0.5, True)
    rows += rank_pair("c1", "harmonic", 0.7, True)
    rows += rank_pair("c2", "harmonic", 0.1, False)
    rows += [
        row("candidate_filter", "c3", "low_threshold_provisional_exclusion",
            "excluded"),
        row("candidate_filter", "c3", "low_threshold_fallback_readmitted",
            "included"),
        row("primary_terminal", "c1", "terminal_final_choice", "selected"),
    ]
    for cid, reason in (
            ("c0", "ranked_not_final"),
            ("c1", "selected_final_object"),
            ("c2", "ranked_not_final"),
            ("c3", "observed_not_ranked"),
            ("c4", "no_cluster_observation")):
        rows.append(row("primary_disposition", cid, reason))
    return rows


class CandidateFateLedgerTests(unittest.TestCase):
    def test_complete_partition_and_actual_displacement(self):
        actual = reconcile(fixture())
        self.assertEqual(actual["total"], 5)
        self.assertEqual(actual["selected"], 1)
        self.assertEqual(actual["displaced_winner"], 1)
        self.assertEqual(actual["comparator_loser"], 1)
        self.assertEqual(actual["gate_only"], 1)
        self.assertEqual(actual["unvisited"], 1)
        self.assertEqual(actual["unselected_pool_winner"], 0)
        self.assertEqual(actual["comparator_rows"], 3)
        self.assertEqual(actual["gate_excluded_sources"], 1)
        self.assertEqual(actual["readmitted_sources"], 1)
        self.assertEqual(actual["first_decisive_veto_unproven"], 5)
        self.assertEqual(sum(actual[key] for key in FATES), 5)

    def test_winning_an_unselected_branch_is_not_a_final_selection(self):
        rows = fixture()
        at = next(i for i, r in enumerate(rows)
                  if r["stage"] == "primary_terminal")
        rows[at:at] = rank_pair("c2", "general_alt", 0.9, True)
        actual = reconcile(rows)
        self.assertEqual(actual["unselected_pool_winner"], 1)
        self.assertEqual(actual["comparator_loser"], 0)
        self.assertEqual(actual["selected"], 1)

    def test_low_episodes_reset_incumbent_without_cross_pool_laundering(self):
        rows = fixture()
        at = next(i for i, r in enumerate(rows)
                  if r["stage"] == "primary_terminal")
        rows[at:at] = (rank_pair("c0", "low", 0.1, True, 1) +
                       rank_pair("c2", "low", 0.05, True, 2))
        actual = reconcile(rows)
        self.assertEqual(actual["unselected_pool_winner"], 2)
        self.assertEqual(actual["selected"], 1)

    def test_negative_mutations_fail_closed(self):
        def broken(change):
            rows = fixture()
            change(rows)
            with self.assertRaises(TraceError):
                reconcile(rows)

        def corrupt_score(rows):
            next(r for r in rows if r["stage"] == "primary_compare")[
                "metrics"]["score"] = 999.0

        def false_winner(rows):
            r = next(r for r in rows if r["candidate_id"] == "c2" and
                     r["stage"] == "primary_compare")
            r["reason_code"] = "harmonic_compare_won"
            r["outcome"] = "included"

        def foreign_source(rows):
            next(r for r in rows if r["stage"] == "primary_rank")[
                "candidate_id"] = "c99"

        def forged_native_disposition(rows):
            next(r for r in rows if r["stage"] == "primary_disposition")[
                "reason_code"] = "selected_final_object"

        def forged_readmission(rows):
            next(r for r in rows if r["reason_code"] ==
                 "low_threshold_provisional_exclusion")[
                "reason_code"] = "harmonic_out_of_rail"

        def duplicate_final(rows):
            rows.append(row("primary_terminal", "c0",
                            "terminal_final_choice", "selected"))

        def orphan_comparison(rows):
            at = next(i for i, r in enumerate(rows)
                      if r["stage"] == "primary_rank")
            del rows[at]

        def duplicate_source(rows):
            rows.insert(0, row("candidate_transform", "c0"))

        for mutate in (corrupt_score, false_winner, foreign_source,
                       forged_native_disposition, forged_readmission,
                       duplicate_final, orphan_comparison, duplicate_source):
            with self.subTest(mutation=mutate.__name__):
                broken(mutate)

    def test_empty_negative_source_board_remains_unclaimed(self):
        actual = reconcile([])
        self.assertEqual(actual["total"], 0)
        self.assertEqual(actual["first_decisive_veto_unproven"], 0)
        self.assertEqual(sum(actual[k] for k in FATES), 0)


if __name__ == "__main__":
    unittest.main()
