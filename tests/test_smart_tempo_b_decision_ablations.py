"""Positive/negative, ground-truth-blind post-host Selector/Policy ablations."""
import copy
import sys
from pathlib import Path
import unittest
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/"tools"))
from audit_smart_tempo_b_decision_ablations import freeze,score,AblationError,MODELS,FIELDS


def e(stage,bpm,cid=None,rank=0.,support=.5,parents=(),outcome="observed"):
    return {"stage":stage,"candidate_id":cid,
            "metrics":{"bpm":bpm,"score":rank,"support":support},
            "parent_candidate_ids":list(parents),"outcome":outcome}


def fixture(primary="c1",outcome="selected",final=84.):
    bpm=60. if primary=="c0" else 84.
    return [
        e("candidate_transform",60.,"c0",.2,.6,["raw0"]),
        e("candidate_transform",84.,"c1",.8,.5,["raw1"]),
        e("primary_eligibility",60.,"c0",.95,.6),
        e("primary_eligibility",84.,"c1",.2,.5),
        e("primary_select",bpm,parents=[primary],outcome=outcome),
        e("final_output",final,outcome="review_hold" if final==0 else "selected")]


class DecisionAblations(unittest.TestCase):
    def test_wrong_primary_but_measured_combined_source(self):
        z=freeze(fixture())
        self.assertEqual(z["PRIMARY_ONLY"],84.)
        self.assertEqual(z["COMBINED"],60.)
        self.assertEqual(z["AGREEMENT_ONLY"],0.)
        r=score(z,60.)
        self.assertEqual(r["NATIVE_FINAL"]["WRONG_AUTO"],1)
        self.assertEqual(r["COMBINED"]["CORRECT_005"],1)
        self.assertEqual(r["AGREEMENT_ONLY"]["HELD"],1)

    def test_correct_primary_overwritten_by_policy(self):
        z=freeze(fixture(primary="c0",final=84.))
        r=score(z,60.)
        self.assertEqual(r["PRIMARY_ONLY"]["CORRECT_020"],1)
        self.assertEqual(r["AGREEMENT_ONLY"]["CORRECT_005"],1)
        self.assertEqual(r["NATIVE_FINAL"]["WRONG_AUTO"],1)

    def test_review_hold_remains_fail_closed_under_primary_and_agreement(self):
        z=freeze(fixture(primary="c0",outcome="review_hold"))
        self.assertEqual((z["PRIMARY_ONLY"],z["AGREEMENT_ONLY"]),(0.,0.))

    def test_empty_negative_and_empty_review_hold(self):
        for outcome in ("no_candidate","review_hold"):
            a=[e("primary_select",0.,parents=[],outcome=outcome),
               e("final_output",0.,outcome="review_hold")]
            z=freeze(a)
            self.assertTrue(all(v==0 for v in z.values()))
            self.assertTrue(all(x["FALSE_ACCEPT"]==0 for x in score(z,None).values()))

    def test_oracle_does_not_enter_candidate_selection(self):
        z=freeze(fixture())
        self.assertEqual(freeze(copy.deepcopy(fixture())),z)
        self.assertEqual(score(z,60.)["COMBINED"]["CORRECT_005"],1)
        self.assertEqual(score(z,84.)["COMBINED"]["CORRECT_005"],0)

    def test_invalid_source_identity_or_bpm_fails_closed(self):
        for field in ("duplicate","foreign","bpm"):
            a=fixture()
            if field=="duplicate":a[3]["candidate_id"]="c0"
            if field=="foreign":a[4]["parent_candidate_ids"]=["c999"]
            if field=="bpm":a[2]["metrics"]["bpm"]=120.
            with self.subTest(field=field),self.assertRaises(ValueError):
                freeze(a)

    def test_nonfinite_metric_and_forged_final_rejected(self):
        a=fixture()
        a[2]["metrics"]["score"]=float("nan")
        with self.assertRaises(ValueError):freeze(a)
        a=fixture()
        a[-1]["metrics"]["bpm"]=0
        with self.assertRaises(ValueError):freeze(a)

    def test_first_native_order_wins_on_equal_combined_scores(self):
        a=fixture()
        a[2]["metrics"]["score"]=a[3]["metrics"]["score"]
        self.assertEqual(freeze(a)["COMBINED"],60.)

    def test_report_contract_and_actual_host_integration(self):
        self.assertEqual(len(MODELS),4)
        self.assertEqual(len(FIELDS),6)
        host=(ROOT/"tests/diagnostic_live_host_smoke.ps1").read_text(encoding="utf-8")
        workflow=(ROOT/".github/workflows/source.yml").read_text(encoding="utf-8")
        self.assertIn("audit_smart_tempo_b_decision_ablations.py",host)
        self.assertIn("SMART_TEMPO_B_ABLATIONS_VERIFIED=1",host)
        self.assertIn("test_smart_tempo_b_decision_ablations.py",workflow)
        source=(ROOT/"src/foo_smart_tempo/modern_bpm_analyzer.cpp").read_text(encoding="utf-8")
        self.assertNotIn("audit_smart_tempo_b_decision_ablations",source)


if __name__=="__main__":
    unittest.main()
