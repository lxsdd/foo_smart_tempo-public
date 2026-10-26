"""Independent positive/negative B-pilot synthetic cohort tests."""
import sys
import unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"tools"))
from audit_smart_tempo_b_pilot_cohort import validate,PilotError

def fixture(arch):
    cases=[]
    for name in ("pulse_60","pulse_128","pulse_120_sparse","silence"):
        negative=name=="silence"; correct=name=="pulse_128"
        score=dict(EVALUATED=1,REFERENCE_RHYTHMIC=int(not negative),
            ACCEPTED_RHYTHMIC=int(not negative),HELD_RHYTHMIC=0,
            ACCEPTED_WITHIN_0_05_BPM=int(correct),ACCEPTED_WITHIN_0_20_BPM=int(correct),
            ACCEPTED_OUTSIDE_0_20_BPM=int(not negative and not correct),
            NEGATIVE_TOTAL=int(negative),NEGATIVE_FALSEACCEPTS=0,
            HALF_DOUBLE_ALIAS=0,CORRECT_ABSTENTION=int(negative))
        trace=dict(NATIVE_SOURCE_INVENTORY_VERIFIED=1,RAW=0 if negative else 20,
            COLLAPSED=0 if negative else 18,WRITE_GUARD_BLOCKED=1,
            PHASE_B_CAUSAL_COMPLETENESS="INCOMPLETE_EMPTY_BOARD_CAUSAL" if negative else "INCOMPLETE_PRIMARY_VETO",
            DECODED_WINDOWS=50,DECLARED_WINDOWS=50,WINDOWS_COMPLETE=1,
            FINAL_ACCEPT=int(not negative),PROCESS_CPU_SECONDS=1.2,
            PROCESS_PEAK_RSS_BYTES=50000000)
        if negative: trace["NEGATIVE_NO_CANDIDATE_TRACE"]=1
        else:
            trace["PRIMARY_VETO_REASON_UNKNOWN"]=18
            trace["INDEXED_NATIVE_FINAL_WITNESS"]=1
        shadow=dict(CANDIDATES=0 if negative else 18,
            REFERENCE_PRESENT=int(not negative),
            NATIVE_PRIMARY_CORRECT=int(correct),
            NATIVE_FINAL_CORRECT=int(correct),
            NATIVE_PRIMARY_SOURCE_INDEXED=int(not negative),
            REFERENCE_LOST_AT_PRIMARY=int(not negative and not correct))
        for model in ("SCORE_SUM","SUPPORT","COMBINED","SCORE_TIMES_SUPPORT"):
            shadow["MODEL_"+model+"_CORRECT"]=int(correct)
            shadow["MODEL_"+model+"_MATCHES_PRIMARY"]=int(not negative)
            shadow["MODEL_"+model+"_REFERENCE_RANK"]=(1 if correct else
                                                       2 if not negative else 0)
            shadow["MODEL_"+model+"_TOP3_REFERENCE"]=int(not negative)
        cases.append(dict(case=name,score=score,trace=trace,
                          shadow=shadow,media_no_write=True))
    return dict(schema="smart_tempo_b_pilot_cohort_v1",architecture=arch,
                source_sha="a"*40,cases=cases)

class CohortTests(unittest.TestCase):
    def test_complete_host_evidence_is_not_phase_b_or_musical_pass(self):
        r=validate([fixture("Win32"),fixture("x64")])
        self.assertEqual((r["phase_b"],r["phase_c"],r["musical_release"]),
                         ("INCOMPLETE","CLOSED","NO_GO"))
        self.assertEqual(r["architectures"]["Win32"]["wrong"],2)
        self.assertEqual(r["architectures"]["x64"]["within_005"],1)
        self.assertEqual(r["architectures"]["x64"]["shadow_lost_primary"],2)
        self.assertEqual(r["architectures"]["Win32"]["shadow_models"]["SUPPORT"],1)
        self.assertEqual(r["architectures"]["Win32"]["shadow_top3"]["SUPPORT"],3)
    def test_single_host_is_not_cross_architecture_proof(self):
        with self.assertRaises(PilotError): validate([fixture("Win32")])
        self.assertEqual(len(validate([fixture("Win32")],require_both=False)["architectures"]),1)
    def test_adversarial_matrix(self):
        def mutate(path,value):
            r=[fixture("Win32"),fixture("x64")]; obj=r[0]
            for key in path[:-1]: obj=obj[key]
            obj[path[-1]]=value
            with self.subTest(path=path),self.assertRaises(PilotError): validate(r)
        mutate(["source_sha"],"b"*40)
        mutate(["architecture"],"x64")
        mutate(["cases",0,"case"],"silence")
        mutate(["cases",0,"media_no_write"],False)
        mutate(["cases",0,"score","EVALUATED"],2)
        mutate(["cases",0,"score","ACCEPTED_RHYTHMIC"],0)
        mutate(["cases",0,"score","ACCEPTED_WITHIN_0_05_BPM"],1)
        mutate(["cases",0,"trace","NATIVE_SOURCE_INVENTORY_VERIFIED"],0)
        mutate(["cases",0,"trace","WRITE_GUARD_BLOCKED"],0)
        mutate(["cases",0,"trace","WINDOWS_COMPLETE"],0)
        mutate(["cases",0,"trace","PRIMARY_VETO_REASON_UNKNOWN"],0)
        mutate(["cases",0,"trace","INDEXED_NATIVE_FINAL_WITNESS"],0)
        mutate(["cases",0,"trace","PHASE_B_CAUSAL_COMPLETENESS"],"PASS")
        mutate(["cases",0,"trace","PROCESS_CPU_SECONDS"],float("nan"))
        mutate(["cases",0,"trace","PROCESS_PEAK_RSS_BYTES"],True)
        mutate(["cases",3,"score","NEGATIVE_FALSEACCEPTS"],1)
        mutate(["cases",3,"trace","NEGATIVE_NO_CANDIDATE_TRACE"],0)
        mutate(["cases",3,"trace","RAW"],1)
        mutate(["cases",3,"trace","FINAL_ACCEPT"],1)
        mutate(["cases",0,"score","PRIVATE_TRACK_NAME"],1)
        mutate(["cases",0,"trace","user_path"],1)
        mutate(["cases",0,"shadow","PRIVATE_SONG"],1)
        mutate(["cases",0,"shadow","REFERENCE_PRESENT"],0)
        mutate(["cases",0,"shadow","MODEL_SUPPORT_CORRECT"],2)
        mutate(["cases",0,"shadow","NATIVE_FINAL_CORRECT"],1)
        mutate(["cases",0,"shadow","CANDIDATES"],17)
        mutate(["cases",0,"shadow","NATIVE_PRIMARY_SOURCE_INDEXED"],0)
        mutate(["cases",0,"shadow","NATIVE_PRIMARY_CORRECT"],1)
        mutate(["cases",0,"shadow","MODEL_SUPPORT_MATCHES_PRIMARY"],2)
        mutate(["cases",0,"shadow","MODEL_SUPPORT_REFERENCE_RANK"],0)
        mutate(["cases",0,"shadow","MODEL_SUPPORT_REFERENCE_RANK"],19)
        mutate(["cases",0,"shadow","MODEL_SUPPORT_TOP3_REFERENCE"],0)
        mutate(["cases",0,"shadow","MODEL_SUPPORT_CORRECT"],1)
        mutate(["cases",3,"shadow","REFERENCE_PRESENT"],1)
    def test_cross_architecture_decision_class_drift(self):
        r=[fixture("Win32"),fixture("x64")]
        s=r[1]["cases"][0]["score"]; t=r[1]["cases"][0]["trace"]
        s.update(ACCEPTED_RHYTHMIC=0,HELD_RHYTHMIC=1,ACCEPTED_OUTSIDE_0_20_BPM=0)
        t.update(FINAL_ACCEPT=0,INDEXED_NATIVE_FINAL_WITNESS=0)
        with self.assertRaisesRegex(PilotError,"cross-architecture decision class drift"):
            validate(r)

    def test_shadow_cross_architecture_must_agree_without_reference_tuning(self):
        rows=[fixture("Win32"),fixture("x64")]
        rows[1]["cases"][0]["shadow"]["MODEL_SUPPORT_CORRECT"]=1
        rows[1]["cases"][0]["shadow"]["MODEL_SUPPORT_MATCHES_PRIMARY"]=0
        with self.assertRaisesRegex(PilotError,"cross-architecture shadow ranking drift"):
            validate(rows)

    def test_cli_json_serialization_and_checkout_sha(self):
        """Exercise the real CLI boundary: two hosts, BOM, duplicate keys, SHA."""
        import contextlib
        import io
        import json
        import tempfile
        from unittest.mock import patch
        from audit_smart_tempo_b_pilot_cohort import main

        payloads=[json.dumps(fixture(a),separators=(",",":")).encode("utf-8")
                  for a in ("Win32","x64")]
        with tempfile.TemporaryDirectory() as tmp:
            files=[Path(tmp)/"win32.json",Path(tmp)/"x64.json"]
            def invoke(blobs,sha="a"*40):
                for file,blob in zip(files,blobs):
                    file.write_bytes(blob)
                stdout=io.StringIO()
                stderr=io.StringIO()
                argv=["audit","--expected-source-sha",sha,
                      "--report",str(files[0]),"--report",str(files[1])]
                with patch.object(sys,"argv",argv),contextlib.redirect_stdout(stdout),contextlib.redirect_stderr(stderr):
                    try:
                        main()
                    except SystemExit as exc:
                        return exc.code,stdout.getvalue(),stderr.getvalue()
                return 0,stdout.getvalue(),stderr.getvalue()

            status,out,err=invoke(payloads)
            self.assertEqual((status,err),(0,""))
            self.assertIn("SMART_TEMPO_B_PILOT_AGGREGATE=PASS",out)
            cases=(
                ("bom",[bytes.fromhex("efbbbf")+payloads[0],payloads[1]],"BOM"),
                ("duplicate root key",[payloads[0].replace(
                    b'"schema":',b'"schema":"forged","schema":',1),payloads[1]],"duplicate JSON key"),
                ("duplicate nested key",[payloads[0].replace(
                    b'"EVALUATED":',b'"EVALUATED":0,"EVALUATED":',1),payloads[1]],"duplicate JSON key"),
            )
            for name,blobs,reason in cases:
                with self.subTest(name=name):
                    status,out,err=invoke(blobs)
                    self.assertNotEqual(status,0)
                    self.assertIn(reason,err)
                    self.assertNotIn("SMART_TEMPO_B_PILOT_AGGREGATE=PASS",out)
            status,out,err=invoke(payloads,"b"*40)
            self.assertNotEqual(status,0)
            self.assertIn("does not match checked-out source",err)
            self.assertNotIn("SMART_TEMPO_B_PILOT_AGGREGATE=PASS",out)

if __name__=="__main__": unittest.main()
