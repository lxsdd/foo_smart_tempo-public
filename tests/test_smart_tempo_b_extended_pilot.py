"""Positive and adversarial native-only extended BPM cohort contracts."""
import copy
import unittest
from tools.audit_smart_tempo_b_extended_pilot import CASES,SCORE,TRACE,verify,EvidenceError
SHA="a"*40
def fixture(arch="Win32"):
    s=dict.fromkeys(SCORE,0)
    s.update(EVALUATED=1,REFERENCE_RHYTHMIC=1,ACCEPTED_RHYTHMIC=1,
             ACCEPTED_WITHIN_0_05_BPM=1,ACCEPTED_WITHIN_0_20_BPM=1)
    t=dict.fromkeys(TRACE,0)
    t.update(NATIVE_SOURCE_INVENTORY_VERIFIED=1,WRITE_GUARD_BLOCKED=1,
             WINDOWS_COMPLETE=1,DECLARED_WINDOWS=2,DECODED_WINDOWS=2,
             FINAL_ACCEPT=1,PROCESS_PEAK_RSS_BYTES=20000000,
             PROCESS_CPU_SECONDS=.5,PHASE_B_CAUSAL_COMPLETENESS="INCOMPLETE_PRIMARY_VETO")
    return dict(schema="smart_tempo_b_extended_pilot_v1",architecture=arch,
        source_sha=SHA,cases=[dict(case=x,score=copy.deepcopy(s),
              trace=copy.deepcopy(t),media_no_write=True) for x in CASES])
class NativeBExtendedTests(unittest.TestCase):
    def test_both_architectures(self):
        self.assertEqual(verify(fixture(),SHA)[1]["correct_020"],7)
        self.assertEqual(verify(fixture("x64"),SHA)[1]["auto"],7)
    def test_held_and_wrong(self):
        x=fixture()
        x["cases"][0]["score"].update(ACCEPTED_RHYTHMIC=0,HELD_RHYTHMIC=1,
            ACCEPTED_WITHIN_0_05_BPM=0,ACCEPTED_WITHIN_0_20_BPM=0)
        x["cases"][0]["trace"]["FINAL_ACCEPT"]=0
        x["cases"][1]["score"].update(ACCEPTED_WITHIN_0_05_BPM=0,
            ACCEPTED_WITHIN_0_20_BPM=0,ACCEPTED_OUTSIDE_0_20_BPM=1)
        t=verify(x,SHA)[1]
        self.assertEqual((t["held"],t["wrong"],t["correct_020"]),(1,1,5))
    def test_fail_closed_negative_cases(self):
        edits=(lambda x:x["cases"].pop(),
               lambda x:x["cases"][2].update(case=x["cases"][1]["case"]),
               lambda x:x.update(source_sha="b"*40),
               lambda x:x.update(private_path="FORBIDDEN"),
               lambda x:x["cases"][0].update(media_no_write=False),
               lambda x:x["cases"][0]["score"].update(HELD_RHYTHMIC=1),
               lambda x:x["cases"][0]["score"].update(NEGATIVE_TOTAL=1),
               lambda x:x["cases"][0]["score"].update(ACCEPTED_OUTSIDE_0_20_BPM=1),
               lambda x:x["cases"][0]["trace"].update(WRITE_GUARD_BLOCKED=0),
               lambda x:x["cases"][0]["trace"].update(DECODED_WINDOWS=1),
               lambda x:x["cases"][0]["trace"].update(FINAL_ACCEPT=0),
               lambda x:x["cases"][0]["trace"].update(PHASE_B_CAUSAL_COMPLETENESS="PASS"),
               lambda x:x["cases"][0]["trace"].pop("COLLAPSED"))
        for edit in edits:
            with self.subTest(edit=edit):
                x=fixture();edit(x)
                with self.assertRaises(EvidenceError):verify(x,SHA)
if __name__=="__main__":unittest.main()
