"""Positive and adversarial native low continuous-center branch closure."""
from __future__ import annotations
import copy
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from audit_smart_tempo_b_low_continuous_fates import (
    LowContinuousFateError, reconcile_native_low_continuous)


def evt(stage, cid, reason, outcome="included", bpm=96., score=2., tie=None):
    metrics = {"bpm": bpm, "score": score, "support": 0.}
    if tie is not None:
        metrics.update({f"tie_{i}": value for i, value in enumerate(tie)})
    return {"stage": stage, "candidate_id": cid, "reason_code": reason,
            "outcome": outcome, "metrics": metrics}


def scenario(center=99., eligible=True):
    bpms = [60., 96., 99.]
    rows = [evt("candidate_transform", f"c{i+1}", "cluster_from_raw",
                "observed", bpm) for i, bpm in enumerate(bpms)]
    best, best_tuple = None, None
    for i, bpm in enumerate(bpms):
        cid = f"c{i+1}"
        allowed = eligible and i != 0
        rows.append(evt("candidate_filter", cid,
                        "low_continuous_allowed" if allowed else
                        "low_continuous_provisional_exclusion",
                        "included" if allowed else "excluded", bpm,
                        2. if allowed else 0.))
        if allowed:
            tie = (2., .90, .80, .50, -abs(bpm-center))
            rows.append(evt("primary_rank", cid, "low_continuous_rank_scored",
                            bpm=bpm))
            won = best_tuple is None or tie > best_tuple
            rows.append(evt("primary_compare", cid,
                            "low_continuous_compare_won" if won else
                            "low_continuous_compare_lost",
                            "included" if won else "excluded", bpm, tie=tie))
            if won:
                best, best_tuple = cid, tie
    for i, bpm in enumerate(bpms):
        cid = f"c{i+1}"
        reason = ("low_continuous_branch_veto" if not eligible or i == 0 else
                  "low_continuous_branch_winner" if cid == best else
                  "low_continuous_branch_ranked_out")
        rows.append(evt("candidate_filter", cid, reason,
                        "included" if cid == best and eligible else "excluded",
                        bpm, 2. if eligible and i else 0.))
    rows.append(evt("primary_terminal", best or "c1", "terminal_lane_choice",
                    "selected", bpms[int(best[1:])-1] if best else 60.))
    return rows


class ContinuousClosureTests(unittest.TestCase):
    def test_opposite_route_centers_and_no_eligible_candidate(self):
        for center, expected in ((99., "c3"), (96., "c2")):
            rows = scenario(center)
            self.assertEqual([r["candidate_id"] for r in rows
                              if r["reason_code"] == "low_continuous_branch_winner"],
                             [expected])
            result = reconcile_native_low_continuous(rows)
            self.assertEqual(result["native_low_continuous_complete_invocations"], 1)
            self.assertEqual(result["native_low_continuous_branch_veto_sources"], 1)
            self.assertEqual(result["native_low_continuous_ranked_out_sources"], 1)
            self.assertEqual(result["native_low_continuous_rescue_winners"], 1)
        result = reconcile_native_low_continuous(scenario(eligible=False))
        self.assertEqual(result["native_low_continuous_branch_veto_sources"], 3)
        self.assertEqual(result["native_low_continuous_rescue_winners"], 0)

    def test_absent_branch_is_not_fabricated(self):
        self.assertEqual(reconcile_native_low_continuous([])[
            "native_low_continuous_complete_invocations"], 0)

    def test_adversarial_mutations_fail_closed(self):
        base = scenario()
        mutations = []
        for reason in ("low_continuous_allowed", "low_continuous_compare_won",
                       "low_continuous_branch_veto",
                       "low_continuous_branch_ranked_out",
                       "low_continuous_branch_winner"):
            rows = copy.deepcopy(base)
            rows.pop(next(i for i, row in enumerate(rows)
                          if row["reason_code"] == reason))
            mutations.append(rows)
        for field, value in (("candidate_id", "c999"), ("outcome", "included"),
                             ("reason_code", "low_continuous_branch_winner")):
            rows = copy.deepcopy(base)
            target = next(row for row in rows if row["reason_code"] ==
                          "low_continuous_branch_veto")
            target[field] = value
            mutations.append(rows)
        rows = copy.deepcopy(base)
        next(row for row in rows if row["reason_code"] ==
             "low_continuous_compare_won")["metrics"]["tie_4"] = 100.
        mutations.append(rows)
        rows = copy.deepcopy(base)
        next(row for row in rows if row["reason_code"] ==
             "terminal_lane_choice")["candidate_id"] = "c2"
        mutations.append(rows)
        rows = copy.deepcopy(base)
        rows.insert(-1, copy.deepcopy(next(row for row in rows
                     if row["reason_code"] == "low_continuous_branch_veto")))
        mutations.append(rows)
        for rows in mutations:
            with self.subTest(rows=len(rows), tail=rows[-2]["reason_code"]):
                with self.assertRaises(LowContinuousFateError):
                    reconcile_native_low_continuous(rows)

    def test_production_host_parser_and_source_wiring(self):
        source = (ROOT/"src/foo_smart_tempo/mir_candidate_pipeline.cpp").read_text()
        host = (ROOT/"tests/smart_tempo_ci_probe.cpp").read_text()
        parser = (ROOT/"tools/audit_smart_tempo_ci_pipeline_trace.py").read_text()
        workflow = (ROOT/".github/workflows/source.yml").read_text()
        for reason in ("low_continuous_branch_veto",
                       "low_continuous_branch_ranked_out",
                       "low_continuous_branch_winner"):
            self.assertIn('"' + reason + '"', source)
            self.assertIn('"' + reason + '"', parser)
            self.assertIn('"' + reason + '"', host)
        self.assertIn("reconcile_native_low_continuous(rows)", parser)
        self.assertIn("test_smart_tempo_b_low_continuous_fates.py", workflow)
        self.assertIn("#if defined(SMART_TEMPO_CI_ORACLE_PROBE)", source)


if __name__ == "__main__":
    unittest.main()
