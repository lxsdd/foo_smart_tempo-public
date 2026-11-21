"""Adversarial, label-blind native manual-export merge provenance."""
import copy
import sys
from pathlib import Path
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from audit_smart_tempo_b_manual_export import reconcile, ManualTraceError


def row(stage, cid=None, parents=(), bpm=0., score=0., support=0.,
        winner=0., exact=0., rank=0, mask=0, reason="", outcome="observed"):
    m = {"bpm": bpm, "score": score, "support": support}
    if stage in ("manual_source", "manual_export"):
        m.update(winner_support=winner, local_exact_score=exact,
                 evidence_rank=rank, origin_mask=mask)
    return {"stage": stage, "candidate_id": cid,
            "parent_candidate_ids": list(parents),
            "metrics": m, "reason_code": reason, "outcome": outcome}


def fixture():
    # Two exact FullBoard rows; Primary merges with the first; TopK is
    # supplemental-only. The latter is not eligible for Primary by origin.
    return [
        row("policy_eligibility", "c2", ("c0",), 128.),
        row("policy_eligibility", "c3", ("c1",), 96.),
        row("primary_select", None, ("c0",), 128.),
        row("manual_state", None, (), 0., 4., 2., reason="manual_requested"),
        row("manual_source", "c4", ("c2",), 128., 5., .7, .4, .8, 1, 1,
            "manual_full_board"),
        row("manual_source", "c5", ("c3",), 96., 4., .6, .2, .7, 2, 1,
            "manual_full_board"),
        row("manual_source", "c6", ("c0",), 128., 6., .8, .3, .9, 0, 2,
            "manual_primary_selector"),
        row("manual_source", "c7", (), 72., 3., .5, .1, .5, 0, 16,
            "manual_partial_topk"),
        row("manual_export", "c8", ("c4", "c6"), 128., 6., .8, .4, .9, 1, 3,
            "manual_merged_export"),
        row("manual_export", "c9", ("c5",), 96., 4., .6, .2, .7, 2, 1,
            "manual_merged_export"),
        row("manual_export", "c10", ("c7",), 72., 3., .5, .1, .5, 3, 16,
            "manual_merged_export"),
    ]


class ManualExportTests(unittest.TestCase):
    def test_merge_fullboard_primary_and_supplemental(self):
        rows = fixture()
        next(r for r in rows if r["stage"] == "manual_state")["metrics"]["support"] = 3.
        result = reconcile(rows, required=True)
        self.assertEqual(result["manual_source_count"], 4)
        self.assertEqual(result["manual_export_count"], 3)
        self.assertEqual(result["manual_merged_sources"], 1)
        self.assertEqual(result["manual_supplemental_only"], 1)

    def test_fail_closed_mutations(self):
        base = fixture()
        next(r for r in base if r["stage"] == "manual_state")["metrics"]["support"] = 3.
        self.assertEqual(reconcile(base, required=True)["manual_native_state"],
                         "REQUESTED_VERIFIED")
        def mutate(label, fn):
            altered = copy.deepcopy(base)
            fn(altered)
            with self.subTest(label=label), self.assertRaises(ManualTraceError):
                reconcile(altered, required=True)
        def stage(rows, name):
            return next(r for r in rows if r["stage"] == name)
        mutate("wrong_count", lambda r: stage(r, "manual_state")["metrics"].update(score=3))
        mutate("foreign_primary", lambda r: next(x for x in r if x["reason_code"] ==
               "manual_primary_selector").update(parent_candidate_ids=["c1"]))
        mutate("missing_source", lambda r: r.pop(6))
        mutate("wrong_merge_parent", lambda r: stage(r, "manual_export").update(
            parent_candidate_ids=["c4"]))
        mutate("wrong_rank", lambda r: stage(r, "manual_export")["metrics"].update(
            evidence_rank=2))
        mutate("false_primary_origin", lambda r: r[-1]["metrics"].update(origin_mask=18))
        mutate("wrong_score", lambda r: r[-1]["metrics"].update(score=99))
        mutate("wrong_bpm", lambda r: r[-1]["metrics"].update(bpm=72.1))
        mutate("duplicate_output", lambda r: r.append(copy.deepcopy(r[-1])))
        mutate("source_reorder", lambda r: r.insert(4, r.pop(6)))
        mutate("unknown_source", lambda r: r[-4].update(reason_code="secret_track"))
        mutate("output_reorder", lambda r: r.insert(8, r.pop(9)))
        mutate("supplemental_parent", lambda r: r[-4].update(parent_candidate_ids=["c2"]))
        mutate("no_request", lambda r: stage(r, "manual_state").update(
            reason_code="manual_not_requested"))
        mutate("missing_state", lambda r: r.pop(3))

    def test_original_host_forces_only_allowlisted_ci_manual_capture(self):
        root = Path(__file__).resolve().parents[1]
        source = (root / "src" / "foo_smart_tempo" /
                  "modern_bpm_analyzer.cpp").read_text(encoding="utf-8")
        host = (root / "tests" / "diagnostic_live_host_smoke.ps1").read_text(
            encoding="utf-8")
        self.assertIn("#if defined(SMART_TEMPO_CI_ORACLE_PROBE)", source)
        self.assertIn("const bool ciCaptureManual =", source)
        self.assertIn("ci_pipeline_trace::permitted_fixture().empty()", source)
        self.assertIn("constexpr bool ciCaptureManual = false;", source)
        self.assertIn("m_diag.suppress_write || captureMeasuredCandidates ||", source)
        self.assertIn("SMART_TEMPO_TRACE_MANUAL_NATIVE_STATE=REQUESTED_VERIFIED", host)
        self.assertIn("Native rhythmic manual-export path not exercised", host)

    def test_explicit_not_requested_and_empty_requested(self):
        self.assertEqual(reconcile([row("manual_state", reason="manual_not_requested")],
                                   required=True)["manual_native_state"], "NOT_REQUESTED")
        self.assertEqual(reconcile([row("manual_state", reason="manual_requested")],
                                   required=True)["manual_export_count"], 0)
        with self.assertRaises(ManualTraceError):
            reconcile([], required=True)

if __name__ == "__main__":
    unittest.main()
