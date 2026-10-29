"""Independent original-cluster rail chronology positive/negative contracts."""
import copy
from pathlib import Path
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"tools"))
from audit_smart_tempo_b_cluster_rail_fates import (
    RailFateError, reconcile_native_rail)


def evt(stage, cid, reason, included=True, bpm=128.):
    return dict(stage=stage,candidate_id=cid,reason_code=reason,
                outcome="included" if included else "excluded",
                metrics=dict(bpm=bpm,score=0.,support=0.))


def scenario(mode):
    general = mode != "harmonic"
    source = ["c1","c2","c3"] if mode != "fallback" else ["c1","c3"]
    bpm = {"c1":30., "c2":128., "c3":230.}
    rows=[evt("candidate_transform",cid,"cluster_from_raw",True,bpm[cid])
          for cid in source]
    for cid in source:
        included = cid == "c2"
        prefix = "general" if general else "harmonic"
        reason=(prefix+"_in_rail" if included else
                "general_out_of_rail_provisional" if general else
                "harmonic_out_of_rail")
        rows.append(evt("candidate_filter",cid,reason,included,bpm[cid]))
    if mode == "fallback":
        for cid in source:
            rows.append(evt("candidate_filter",cid,
                            "general_rail_fallback_readmitted",True,bpm[cid]))
    else:
        for cid in ("c1","c3"):
            rows.append(evt("candidate_filter",cid,
                            ("general" if general else "harmonic")+
                            "_rail_decisive_veto",False,bpm[cid]))
    finalists = source if mode=="fallback" else ["c2"]
    for cid in finalists:
        prefix="general_baseline" if general else "harmonic"
        rows.append(evt("primary_rank",cid,prefix+"_rank_scored",True,bpm[cid]))
        rows.append(evt("primary_compare",cid,prefix+"_compare_won",True,bpm[cid]))
    rows.append(evt("primary_terminal",finalists[-1],"terminal_lane_choice",True,
                    bpm[finalists[-1]]))
    rows[-1]["outcome"]="selected"
    return rows


class ClusterRailFatesTests(unittest.TestCase):
    def test_three_native_rail_positive_paths(self):
        for mode, expected in (
            ("general",(2,0,0,1)),("harmonic",(0,2,0,1)),
            ("fallback",(0,0,2,2))):
            with self.subTest(mode=mode):
                got=reconcile_native_rail(scenario(mode))
                self.assertEqual(tuple(got[k] for k in (
                    "native_general_first_decisive_rail",
                    "native_harmonic_first_decisive_rail",
                    "native_general_rail_fallback_sources",
                    "native_rail_compared_sources")),expected)
                self.assertEqual(got["native_rail_complete_invocations"],1)

    def test_abstain_when_native_rail_never_executed(self):
        self.assertEqual(reconcile_native_rail([])["native_rail_complete_invocations"],0)

    def test_original_rail_adversarial_event_tampering_fails(self):
        for mode in ("general","harmonic","fallback"):
            for corruption in ("missing_initial","wrong_source","wrong_bpm",
                               "duplicate_closure","early_veto","false_inclusion",
                               "missing_rank","wrong_terminal","wrong_family"):
                with self.subTest(mode=mode,corruption=corruption):
                    bad=copy.deepcopy(scenario(mode))
                    checks=[i for i,r in enumerate(bad) if r["stage"]=="candidate_filter"]
                    if corruption=="missing_initial":bad.pop(checks[0])
                    elif corruption=="wrong_source":bad[checks[0]]["candidate_id"]="c3"
                    elif corruption=="wrong_bpm":bad[checks[0]]["metrics"]["bpm"]=77.
                    elif corruption=="duplicate_closure":
                        bad.insert(checks[-1],copy.deepcopy(bad[checks[-1]]))
                    elif corruption=="early_veto":
                        bad[checks[0]],bad[checks[-1]]=bad[checks[-1]],bad[checks[0]]
                    elif corruption=="false_inclusion":bad[checks[0]]["outcome"]="included"
                    elif corruption=="missing_rank":
                        bad=[r for r in bad if r["stage"]!="primary_rank"]
                    elif corruption=="wrong_terminal":bad[-1]["candidate_id"]="c1"
                    else:bad[checks[0]]["reason_code"]="harmonic_in_rail" if mode!="harmonic" else "general_in_rail"
                    with self.assertRaises(RailFateError):
                        reconcile_native_rail(bad)

    def test_real_native_host_and_ci_do_not_use_public_oracle(self):
        root=Path(__file__).resolve().parents[1]
        cpp=(root/"tests/smart_tempo_ci_probe.cpp").read_text(encoding="utf-8")
        ps=(root/"tests/diagnostic_live_host_smoke.ps1").read_text(encoding="utf-8")
        wf=(root/".github/workflows/source.yml").read_text(encoding="utf-8")
        audit=(root/"tools/audit_smart_tempo_ci_pipeline_trace.py").read_text(encoding="utf-8")
        for i in range(3):
            self.assertIn(f"native_rail_contract({i})",cpp)
        self.assertIn("SMART_TEMPO_CI_NATIVE_RAIL=PASS",cpp)
        self.assertIn("SMART_TEMPO_NATIVE_RAIL_{0}_{1}=PASS",ps)
        self.assertIn("test_smart_tempo_b_cluster_rail_fates.py",wf)
        self.assertIn("reconcile_native_rail(rows)",audit)
        self.assertNotIn("reference_bpm",cpp)


if __name__ == "__main__":
    unittest.main()
