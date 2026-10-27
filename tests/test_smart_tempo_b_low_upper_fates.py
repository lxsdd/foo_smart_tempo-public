"""Positive and adversarial native low-pulse upper branch proof."""
import copy
from pathlib import Path
import sys
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from audit_smart_tempo_b_low_upper_fates import (
    LowUpperFateError, reconcile_native_low_upper)


def evt(stage, cid, reason, outcome="included", bpm=110.):
    return {"stage": stage, "candidate_id": cid, "reason_code": reason,
            "outcome": outcome,
            "metrics": {"bpm": bpm, "score": 1., "support": 0.,
                        "low_pool_epoch": 1}}


def scenario(upper=True):
    bpms = {"c1": 60., "c2": 110., "c3": 150.}
    rows = [evt("candidate_transform", cid, "cluster_from_raw", "observed",
                bpms[cid]) for cid in bpms]
    for cid in bpms:
        allowed = upper and cid == "c2"
        rows.append(evt("candidate_filter", cid,
                        "low_upper_gate_allowed" if allowed else
                        "low_upper_gate_provisional_exclusion",
                        "included" if allowed else "excluded", bpms[cid]))
    if upper:
        for cid in ("c1", "c3"):
            rows.append(evt("candidate_filter", cid, "low_upper_branch_veto",
                            "excluded", bpms[cid]))
        rows.append(evt("primary_rank", "c2", "low_rank_scored"))
        rows.append(evt("primary_compare", "c2", "low_compare_won"))
        rows.append(evt("primary_terminal", "c2", "terminal_lane_choice",
                        "selected"))
    else:
        for cid in bpms:
            rows.append(evt("candidate_filter", cid,
                            "low_threshold_allowed" if cid == "c1" else
                            "low_threshold_provisional_exclusion",
                            "included" if cid == "c1" else "excluded", bpms[cid]))
    return rows


class LowUpperFateTests(unittest.TestCase):
    def test_positive_branch_and_continuation_and_unvisited(self):
        upper = reconcile_native_low_upper(scenario())
        self.assertEqual(tuple(upper.values()), (1, 2, 0, 1))
        continuation = reconcile_native_low_upper(scenario(False))
        self.assertEqual(tuple(continuation.values()), (1, 0, 1, 0))
        self.assertEqual(reconcile_native_low_upper([])[
            "native_low_upper_complete_invocations"], 0)

    def test_branch_local_veto_is_not_a_final_veto(self):
        rows = scenario()
        # A later continuous-center rescue may choose an upper-vetoed ID.
        rows.insert(-1, evt("primary_compare", "c1",
                            "low_continuous_compare_won"))
        rows[-1]["candidate_id"] = "c1"
        self.assertEqual(reconcile_native_low_upper(rows)[
            "native_low_upper_branch_veto_sources"], 2)

    def test_adversarial_branch_mutations_rejected(self):
        for corruption in ("missing_gate", "swap_gate", "wrong_bpm",
                           "wrong_polarity", "missing_veto", "duplicate_veto",
                           "early_veto", "rank_vetoed", "missing_rank",
                           "missing_compare", "wrong_terminal", "threshold_after",
                           "false_allowed", "veto_readmitted"):
            with self.subTest(corruption=corruption):
                rows = copy.deepcopy(scenario())
                if corruption == "missing_gate": rows.pop(3)
                elif corruption == "swap_gate": rows[3], rows[4] = rows[4], rows[3]
                elif corruption == "wrong_bpm": rows[3]["metrics"]["bpm"] = 90.
                elif corruption == "wrong_polarity": rows[4]["outcome"] = "excluded"
                elif corruption == "missing_veto": rows.pop(6)
                elif corruption == "duplicate_veto": rows.insert(7, copy.deepcopy(rows[7]))
                elif corruption == "early_veto": rows[5], rows[6] = rows[6], rows[5]
                elif corruption == "rank_vetoed": rows[-3]["candidate_id"] = "c1"
                elif corruption == "missing_rank": rows.pop(-3)
                elif corruption == "missing_compare": rows.pop(-2)
                elif corruption == "wrong_terminal": rows[-1]["candidate_id"] = "c3"
                elif corruption == "threshold_after":
                    rows.insert(-1, evt("candidate_filter", "c1",
                                        "low_threshold_allowed"))
                elif corruption == "false_allowed":
                    rows[4]["reason_code"] = "low_upper_gate_allowed"
                else:
                    rows.insert(8, evt("candidate_filter", "c1",
                                       "low_upper_branch_veto", "excluded", 60.))
                with self.assertRaises(LowUpperFateError):
                    reconcile_native_low_upper(rows)

    def test_empty_upper_adversarial(self):
        for corruption in ("missing_threshold", "wrong_threshold_order",
                           "invented_veto", "wrong_threshold_bpm"):
            with self.subTest(corruption=corruption):
                rows = copy.deepcopy(scenario(False))
                if corruption == "missing_threshold": rows.pop()
                elif corruption == "wrong_threshold_order":
                    rows[-1], rows[-2] = rows[-2], rows[-1]
                elif corruption == "invented_veto":
                    rows.insert(6, evt("candidate_filter", "c1",
                                       "low_upper_branch_veto", "excluded", 60.))
                else: rows[6]["metrics"]["bpm"] = 99.
                with self.assertRaises(LowUpperFateError):
                    reconcile_native_low_upper(rows)

    def test_original_host_and_parser_are_wired(self):
        root = Path(__file__).resolve().parents[1]
        source = (root/"src/foo_smart_tempo/mir_candidate_pipeline.cpp").read_text()
        probe = (root/"tests/smart_tempo_ci_probe.cpp").read_text()
        smoke = (root/"tests/diagnostic_live_host_smoke.ps1").read_text()
        parser = (root/"tools/audit_smart_tempo_ci_pipeline_trace.py").read_text()
        workflow = (root/".github/workflows/source.yml").read_text()
        self.assertIn('"low_upper_branch_veto"', source)
        self.assertIn("native_low_upper_contract(true)", probe)
        self.assertIn("native_low_upper_contract(false)", probe)
        self.assertIn("SMART_TEMPO_CI_NATIVE_LOW_UPPER=PASS", probe)
        self.assertIn("SMART_TEMPO_NATIVE_LOW_UPPER_{0}_{1}=PASS", smoke)
        self.assertIn("reconcile_native_low_upper(rows)", parser)
        self.assertIn("test_smart_tempo_b_low_upper_fates.py", workflow)
        self.assertNotIn("reference_bpm", source[source.index("if (!upper.empty())"):source.index("std::vector<const MirCandidate*> thresholdPool;")])


if __name__ == "__main__":
    unittest.main()
