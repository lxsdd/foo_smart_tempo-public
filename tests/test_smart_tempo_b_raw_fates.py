"""Independent complete raw-selector state-machine adversarial regressions."""
from __future__ import annotations
import copy
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from audit_smart_tempo_b_raw_fates import reconcile_native_raw, RawFateError


def row(stage, cid, reason, outcome, bpm=128.0):
    return dict(stage=stage, candidate_id=cid, reason_code=reason,
                outcome=outcome, metrics=dict(bpm=bpm, score=0.0, support=0.0))


def sample(scenario):
    """Six independent frozen event sequences from the original C++ contract.

    The public numeric BPM is deliberately IDENTICAL for c0 and c1: never
    infer selector identity from the BPM value alone.
    """
    A = "raw_alias_"
    S = "raw_support_"
    R = "raw_rank_scored"
    W = "raw_compare_won"
    # tuple: native source index, exact event reason, polarity
    cases = {
        0: [
            (0,A+"provisional_exclusion",0),(1,A+"provisional_exclusion",0),
            (0,A+"fallback_readmitted",1),(1,A+"fallback_readmitted",1),
            (0,S+"provisional_exclusion",0),(1,S+"provisional_exclusion",0),
            (0,S+"fallback_readmitted",1),(1,S+"fallback_readmitted",1),
            (0,R,1),(0,W,1),(1,R,1),(1,W,1)],
        1: [
            (0,A+"provisional_exclusion",0),(1,A+"allowed",1),
            (0,A+"decisive_veto",0),(1,S+"allowed",1),(1,R,1),(1,W,1)],
        2: [
            (0,A+"provisional_exclusion",0),(1,A+"provisional_exclusion",0),
            (0,A+"fallback_readmitted",1),(1,A+"fallback_readmitted",1),
            (0,S+"provisional_exclusion",0),(1,S+"allowed",1),
            (0,S+"decisive_veto",0),(1,R,1),(1,W,1)],
        3: [
            (0,A+"provisional_exclusion",0),(1,A+"allowed",1),
            (0,A+"decisive_veto",0),(1,S+"allowed",1),(1,R,1),(1,W,1)],
        4: [
            (0,A+"provisional_exclusion",0),(1,A+"allowed",1),
            (0,A+"decisive_veto",0),(1,S+"provisional_exclusion",0),
            (1,S+"fallback_readmitted",1),(1,R,1),(1,W,1)],
        5: [
            (0,A+"allowed",1),(1,A+"allowed",1),
            (0,S+"provisional_exclusion",0),(1,S+"allowed",1),
            (0,S+"decisive_veto",0),(1,R,1),(1,W,1)],
    }
    rows = [row("candidate_create","c0","raw_topk","observed"),
            row("candidate_create","c1","raw_topk","observed"),
            row("candidate_transform","c2","cluster_from_raw","observed")]
    for index, reason, included in cases[scenario]:
        stage = ("primary_rank" if reason == R else
                 "primary_compare" if reason.startswith("raw_compare_") else
                 "candidate_filter")
        rows.append(row(stage, f"c{index}", reason,
                        "included" if included else "excluded"))
    rows.append(row("primary_terminal", "c2", "terminal_lane_choice", "selected"))
    return rows


class NativeRawFateTests(unittest.TestCase):
    def test_six_distinct_native_qualification_sequences(self):
        expected = {
            0: (0,0,2,2,2),
            1: (1,0,0,0,1),
            2: (0,1,2,0,1),
            3: (1,0,0,0,1),
            4: (1,0,0,1,1),
            5: (0,1,0,0,1),
        }
        for case, values in expected.items():
            with self.subTest(case=case):
                result = reconcile_native_raw(sample(case))
                self.assertEqual(result["native_raw_complete_invocations"], 1)
                self.assertEqual(tuple(result[name] for name in (
                    "native_raw_first_decisive_alias",
                    "native_raw_first_decisive_support",
                    "native_raw_alias_fallback_sources",
                    "native_raw_support_fallback_sources",
                    "native_raw_compared_sources")), values)

    def test_native_nonraw_lane_does_not_claim_raw_coverage(self):
        other = [row("candidate_create","c0","raw_topk","observed")]
        self.assertEqual(reconcile_native_raw(other)["native_raw_complete_invocations"],0)

    def test_adversarial_native_raw_fate_mutations_fail(self):
        for scenario in range(6):
            original = sample(scenario)
            indices = [i for i,r in enumerate(original)
                       if r["reason_code"].startswith("raw_") and r["stage"] in (
                           "candidate_filter","primary_rank","primary_compare")]
            for name in ("truncate","duplicate","swap","wrong_id","wrong_bpm",
                         "wrong_outcome","wrong_stage","orphan_rank","terminal"):
                with self.subTest(scenario=scenario, mutation=name):
                    bad = copy.deepcopy(original)
                    start = indices[0]
                    if name == "truncate":
                        del bad[indices[-1]]
                    elif name == "duplicate":
                        bad.insert(indices[-1], copy.deepcopy(bad[indices[-1]]))
                    elif name == "swap":
                        bad[indices[0]],bad[indices[1]]=bad[indices[1]],bad[indices[0]]
                    elif name == "wrong_id":
                        bad[start]["candidate_id"] = "c1"
                    elif name == "wrong_bpm":
                        bad[start]["metrics"]["bpm"] = 127.5
                    elif name == "wrong_outcome":
                        bad[start]["outcome"] = ("excluded" if bad[start]["outcome"]=="included"
                                                 else "included")
                    elif name == "wrong_stage":
                        bad[start]["stage"] = "primary_rank"
                    elif name == "orphan_rank":
                        bad.insert(indices[-1], row(
                            "primary_rank","c0","raw_rank_scored","included"))
                    else:
                        bad[-1]["candidate_id"] = "c0"
                    with self.assertRaises(RawFateError):
                        reconcile_native_raw(bad)

    def test_provisional_veto_without_native_resolution_is_rejected(self):
        rows = sample(1)
        rows = [r for r in rows if r["reason_code"] != "raw_alias_decisive_veto"]
        with self.assertRaises(RawFateError):
            reconcile_native_raw(rows)

    def test_source_integration_and_privacy_guards(self):
        src=(Path(__file__).resolve().parents[1] / "tools" /
             "audit_smart_tempo_ci_pipeline_trace.py").read_text(encoding="utf-8")
        ci=(Path(__file__).resolve().parents[1] / ".github" /
            "workflows" / "source.yml").read_text(encoding="utf-8")
        self.assertIn("reconcile_native_raw(rows)",src)
        self.assertIn("RawFateError",src)
        self.assertIn("test_smart_tempo_b_raw_fates.py",ci)
        for forbidden in ("reference_bpm","owner_path","artist","filename","genre"):
            self.assertNotIn(forbidden, Path(__file__).resolve().parents[1].joinpath(
                "tools/audit_smart_tempo_b_raw_fates.py").read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
