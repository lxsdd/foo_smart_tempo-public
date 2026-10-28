"""Adversarial source-ID and truth-blind ranking tests: Phase-B shadow ONLY."""
import copy
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from audit_smart_tempo_b_candidate_recall import RecallError
from audit_smart_tempo_b_shadow_rank import (
    MODELS, RESULT_KEYS, fixed_rank, fixed_order, shadow_result,
)


def event(stage, bpm, cid=None, score=0., support=0.,
          parents=(), outcome="observed"):
    return dict(stage=stage, candidate_id=cid, parent_candidate_ids=list(parents),
                outcome=outcome, metrics=dict(bpm=bpm, score=score,
                                              support=support))


def fixture(case="pulse_60", selected="c1"):
    expected = {"pulse_60": (60., "regular"),
                "pulse_128": (128., "regular"),
                "pulse_120_sparse": (120., "sparse"),
                "silence": (None, "silence")}
    bpm, style = expected[case]
    manifest = dict(schema="synthetic_bpm_reference_v1", cases=[
        dict(id=case, reference_bpm=bpm, style=style, duration_sec=20,
             sample_rate=16000, event_sample_positions=[], wav_sha256="a"*64)])
    if bpm is None:
        rows = [
            event("primary_select", 0., outcome="no_candidate"),
            event("policy_change", 0.),
            event("final_output", 0., outcome="review_hold")]
        return rows, manifest, [dict(index="0", bpm="0", held="1")]
    wrong = bpm + 24.0
    rows = [
        event("candidate_create", bpm, "raw0"),
        event("candidate_create", wrong, "raw1"),
        event("candidate_transform", bpm, "c0", .2, .9, ["raw0"]),
        event("candidate_transform", wrong, "c1", .8, .3, ["raw1"]),
        event("primary_eligibility", bpm, "c0", .95, .9),
        event("primary_eligibility", wrong, "c1", .2, .3),
        event("policy_eligibility", bpm, "board0", parents=["c0"]),
        event("policy_eligibility", wrong, "board1", parents=["c1"]),
        event("primary_select", bpm if selected == "c0" else wrong,
              parents=[selected], outcome="selected"),
        event("policy_change", bpm if selected == "c0" else wrong),
        event("final_output", bpm if selected == "c0" else wrong, outcome="selected")]
    return rows, manifest, [dict(index="0",
                                 bpm=str(bpm if selected == "c0" else wrong),
                                 held="0")]


class ShadowRankTests(unittest.TestCase):
    def test_missing_reference_is_specifically_primary_selection_failure(self):
        rows, manifest, obs = fixture()
        report = shadow_result(rows, manifest, obs, "pulse_60")
        self.assertEqual((report["CANDIDATES"],report["REFERENCE_PRESENT"],
                          report["REFERENCE_LOST_AT_PRIMARY"]), (2,1,1))
        self.assertEqual((report["NATIVE_PRIMARY_CORRECT"],
                          report["NATIVE_FINAL_CORRECT"],
                          report["NATIVE_PRIMARY_SOURCE_INDEXED"]), (0,0,1))
        self.assertEqual(
            [report["MODEL_"+k+"_CORRECT"] for k in MODELS], [0,1,1,0])
        self.assertEqual(
            [report["MODEL_"+k+"_MATCHES_PRIMARY"] for k in MODELS], [1,0,0,1])
        self.assertEqual(
            [report["MODEL_"+k+"_REFERENCE_RANK"] for k in MODELS],
            [2,1,1,2])
        self.assertEqual(
            [report["MODEL_"+k+"_TOP3_REFERENCE"] for k in MODELS],
            [1,1,1,1])
        self.assertEqual(set(report),set(RESULT_KEYS))

    def test_local_exact_refinement_is_scored_as_native_primary_not_cluster(self):
        rows, manifest, observed = fixture("pulse_60", "c0")
        rows[-3]["metrics"].update(bpm=60.4, cluster_bpm=60.,
                                    local_exact_bpm=60.4, local_exact_delta=.4)
        report = shadow_result(rows, manifest, observed, "pulse_60")
        self.assertEqual(report["NATIVE_PRIMARY_SOURCE_INDEXED"], 1)
        self.assertEqual(report["NATIVE_PRIMARY_CORRECT"], 0)
        self.assertEqual(report["NATIVE_FINAL_CORRECT"], 1)
        self.assertEqual(report["MODEL_COMBINED_CORRECT"], 1)

    def test_correct_primary_not_laundered_as_shadow_improvement(self):
        rows, manifest, observed = fixture("pulse_128", "c0")
        report = shadow_result(rows, manifest, observed, "pulse_128")
        self.assertEqual((report["NATIVE_PRIMARY_CORRECT"],
                          report["REFERENCE_LOST_AT_PRIMARY"]), (1,0))
        self.assertEqual(report["MODEL_SUPPORT_MATCHES_PRIMARY"], 1)
        self.assertEqual(report["MODEL_SCORE_SUM_MATCHES_PRIMARY"], 0)

    def test_native_review_hold_keeps_exact_primary_source_before_policy_release(self):
        rows, manifest, observed = fixture("pulse_60")
        rows[-3]["outcome"] = "review_hold"
        out = shadow_result(rows, manifest, observed, "pulse_60")
        self.assertEqual(out["NATIVE_PRIMARY_SOURCE_INDEXED"], 1)
        self.assertEqual(out["NATIVE_PRIMARY_CORRECT"], 0)
        self.assertEqual(out["NATIVE_FINAL_CORRECT"], 0)
        self.assertEqual(out["REFERENCE_LOST_AT_PRIMARY"], 1)
        # Even a held Primary has a genuinely selected source object. An
        # orphaned alias and a fake automatic selection must still fail.
        bad = copy.deepcopy(rows)
        bad[-3]["parent_candidate_ids"] = ["c77"]
        with self.assertRaises(RecallError):
            shadow_result(bad, manifest, observed, "pulse_60")

    def test_primary_no_candidate_has_no_invented_shadow_identity(self):
        rows, manifest, observed = fixture("pulse_60")
        rows[-3]["outcome"] = "no_candidate"
        rows[-3]["metrics"]["bpm"] = 0.
        rows[-3]["parent_candidate_ids"] = []
        out = shadow_result(rows, manifest, observed, "pulse_60")
        self.assertEqual(out["NATIVE_PRIMARY_SOURCE_INDEXED"], 0)
        self.assertTrue(out["REFERENCE_PRESENT"])

    def test_negative_abstention_no_oracle_generated_candidate(self):
        rows, manifest, observed = fixture("silence")
        report = shadow_result(rows, manifest, observed, "silence")
        self.assertEqual(report["CANDIDATES"], 0)
        self.assertEqual(report["REFERENCE_PRESENT"], 0)
        self.assertTrue(all(report["MODEL_"+m+"_CORRECT"] == 0 for m in MODELS))
        self.assertTrue(all(report["MODEL_"+m+"_MATCHES_PRIMARY"] == 0
                            for m in MODELS))
        self.assertTrue(all(report["MODEL_"+m+"_REFERENCE_RANK"] == 0
                            and report["MODEL_"+m+"_TOP3_REFERENCE"] == 0
                            for m in MODELS))
        altered = copy.deepcopy(rows)
        altered.insert(0,event("candidate_transform",60.,"c0",.2,.9))
        with self.assertRaises(RecallError):
            shadow_result(altered,manifest,observed,"silence")

    def test_predictions_are_blind_to_expected_tempo_and_frozen_order(self):
        rows, manifest, observed = fixture()
        cluster = [x for x in rows if x["stage"]=="candidate_transform"]
        eligible = [x for x in rows if x["stage"]=="primary_eligibility"]
        selected = fixed_rank(cluster,eligible)
        self.assertEqual(selected["SUPPORT"],"c0")
        self.assertEqual(selected["SCORE_SUM"],"c1")
        shift = copy.deepcopy(cluster)
        shift[0]["metrics"]["bpm"] = 159.0
        shift[1]["metrics"]["bpm"] = 175.0
        el = copy.deepcopy(eligible)
        el[0]["metrics"]["bpm"] = 159.0
        el[1]["metrics"]["bpm"] = 175.0
        self.assertEqual(fixed_rank(shift,el),selected)
        # Exactly equal source metric resolves to first native vector item.
        tied = copy.deepcopy(cluster)
        tied[0]["metrics"]["score"] = tied[1]["metrics"]["score"]
        self.assertEqual(fixed_rank(tied,eligible)["SCORE_SUM"],"c0")
        self.assertEqual(fixed_order(tied,eligible)["SCORE_SUM"],["c0","c1"])

    def test_rank_four_reference_is_not_a_top_three_hit(self):
        rows, manifest, observed = fixture("pulse_60")
        # Four source rows; only the first is reference-near and receives
        # the lowest score in every untuned model.
        for row in rows:
            if row["stage"] == "candidate_transform" and row["candidate_id"] == "c0":
                row["metrics"]["score"] = .01
                row["metrics"]["support"] = .01
            if row["stage"] == "primary_eligibility" and row["candidate_id"] == "c0":
                row["metrics"]["score"] = .01
                row["metrics"]["support"] = .01
        for i, score in enumerate((.65, .6), start=2):
            cid = "c"+str(i)
            bpm = 160.+i
            rows.insert(2, event("candidate_transform", bpm, cid, score, score,
                                  ["raw1"]))
            rows.insert(next(i for i,r in enumerate(rows)
                             if r["stage"] == "primary_eligibility"),
                        event("primary_eligibility", bpm, cid, score, score))
            rows.insert(next(i for i,r in enumerate(rows)
                             if r["stage"] == "policy_eligibility"),
                        event("policy_eligibility", bpm, "board"+str(i),
                              parents=[cid]))
        # This light unit fixture verifies shadow ranking; independent
        # production lineage remains checked by validate(require_windows).
        actual = shadow_result(rows, manifest, observed, "pulse_60")
        for name in MODELS:
            self.assertEqual(actual["MODEL_"+name+"_REFERENCE_RANK"], 4)
            self.assertEqual(actual["MODEL_"+name+"_TOP3_REFERENCE"], 0)
            self.assertEqual(actual["MODEL_"+name+"_CORRECT"], 0)

    def test_invalid_identity_source_counts_or_rank_bpm_rejected(self):
        rows, _, _ = fixture()
        c = [x for x in rows if x["stage"]=="candidate_transform"]
        e = [x for x in rows if x["stage"]=="primary_eligibility"]
        for poison in ("missing","double","foreign","wrong_bpm","nonfinite",
                       "non_numeric","duplicate_cluster"):
            with self.subTest(poison=poison):
                cc,ee=copy.deepcopy(c),copy.deepcopy(e)
                if poison=="missing":ee.pop()
                elif poison=="double":ee.append(copy.deepcopy(ee[-1]))
                elif poison=="foreign":ee[-1]["candidate_id"]="c99"
                elif poison=="wrong_bpm":ee[-1]["metrics"]["bpm"]=200.
                elif poison=="nonfinite":cc[0]["metrics"]["support"]=float("nan")
                elif poison=="non_numeric":cc[0]["metrics"]["score"]="bad"
                else:cc[-1]["candidate_id"]="c0"
                with self.assertRaises((RecallError, TypeError)):
                    fixed_rank(cc,ee)

    def test_cannot_change_manifest_or_final_writer_after_the_fact(self):
        rows, manifest, observed = fixture()
        for poison in ("oracle","case","host_bpm","primary_wrong_id",
                       "primary_wrong_bpm","final_forged","silent_selected"):
            with self.subTest(poison=poison):
                r,m,o=copy.deepcopy(rows),copy.deepcopy(manifest),copy.deepcopy(observed)
                case="pulse_60"
                if poison=="oracle":m["cases"][0]["reference_bpm"]=65.
                elif poison=="case":case="pulse_128"
                elif poison=="host_bpm":o[0]["bpm"]="999"
                elif poison=="primary_wrong_id":r[-3]["parent_candidate_ids"]=["c77"]
                elif poison=="primary_wrong_bpm":r[-3]["metrics"]["bpm"]=90.
                elif poison=="final_forged":r[-1]["metrics"]["bpm"]=101.
                else:o[0]["held"]="1"
                with self.assertRaises(RecallError):
                    shadow_result(r,m,o,case)

    def test_shadow_is_post_host_and_never_in_product_or_dsp(self):
        source = (ROOT/"src/foo_smart_tempo/modern_bpm_analyzer.cpp").read_text(encoding="utf-8")
        host = (ROOT/"tests/diagnostic_live_host_smoke.ps1").read_text(encoding="utf-8")
        workflow = (ROOT/".github/workflows/source.yml").read_text(encoding="utf-8")
        tool = (ROOT/"tools/audit_smart_tempo_b_shadow_rank.py").read_text(encoding="utf-8")
        self.assertNotIn("audit_smart_tempo_b_shadow_rank",source)
        self.assertNotIn("reference_bpm",source)
        self.assertIn("audit_smart_tempo_b_shadow_rank.py",host)
        self.assertIn("--manifest $reference --observed $observed",host)
        self.assertIn("--trace $trace --case $label.Substring(10)",host)
        self.assertIn("test_smart_tempo_b_shadow_rank.py",workflow)
        blind = tool.split("def fixed_rank(",1)[1].split("def shadow_result(",1)[0]
        self.assertNotIn("expected_bpm",blind)
        self.assertNotIn("reference_bpm",blind)
        self.assertNotIn("case_id",blind)
        self.assertNotIn("tuned",blind.lower())
        self.assertNotIn("upload-artifact@",workflow)


if __name__=="__main__":
    unittest.main()
