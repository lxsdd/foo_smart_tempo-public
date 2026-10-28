"""Original seek-plan independent synthetic positive and negative cases."""
from __future__ import annotations
import copy
from pathlib import Path
import sys
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from audit_smart_tempo_b_seek_plan import SeekPlanError, reconcile

def fixture(total=3, length=100., duration=20., seek=1):
    rows = []
    for i in range(total):
        pct = 0. if not length else min(79.999999, 20 + 60*(.5 if total == 1 else i/(total-1)))
        start = 0. if not length else min(length*pct/100, max(0., length-duration-.5))
        rows.append({"stage":"decode_window","metrics":{
            "pass_index":i,"pass_total":total,"source_rate_hz":16000,
            "analysis_rate_hz":16000,"start_sec":start,
            "requested_frames":int(duration*16000),"decoded_frames":16000,
            "analysis_frames":16000,"known_length_sec":length,
            "planned_window_sec":duration,"offset_pct":pct,
            "seek_target_sec":start if seek else 0.,
            "can_seek":seek,"seek_invoked":seek,"seek_returned":seek}})
    return rows

class SeekPlanTests(unittest.TestCase):
    def test_original_multi_and_single(self):
        self.assertEqual(reconcile(fixture(),required=True)["seek_calls_returned"],3)
        self.assertEqual(reconcile(fixture(1,0.,2.,0),required=True)["seek_calls_returned"],0)
        self.assertEqual(reconcile(fixture(1),required=True)["seek_decoder_position_verified"],0)

    def test_legacy_never_counts_as_native(self):
        self.assertEqual(reconcile([])["seek_plan_verified"],0)
        old=fixture()
        for row in old:
            row["metrics"].pop("seek_target_sec")
            row["metrics"].pop("seek_returned")
        self.assertEqual(reconcile(old)["seek_plan_verified"],0)
        with self.assertRaises(SeekPlanError):
            reconcile(old,required=True)

    def test_mutations_fail_closed(self):
        mutations={
            "start":lambda m:m[1].__setitem__("start_sec",49.),
            "target":lambda m:m[1].__setitem__("seek_target_sec",49.),
            "return":lambda m:m[0].__setitem__("seek_returned",0),
            "invoked":lambda m:m[0].__setitem__("seek_invoked",0),
            "offset":lambda m:m[2].__setitem__("offset_pct",80.),
            "request":lambda m:m[0].__setitem__("requested_frames",319900),
            "duration":lambda m:m[1].__setitem__("planned_window_sec",19.),
            "length":lambda m:m[1].__setitem__("known_length_sec",99.),
            "capability":lambda m:m[0].__setitem__("can_seek",0),
            "missing":lambda m:m[1].pop("seek_target_sec"),
            "nan":lambda m:m[0].__setitem__("seek_target_sec",float("nan")),
            "fraction":lambda m:m[0].__setitem__("seek_invoked",.5)}
        for name,fn in mutations.items():
            with self.subTest(name=name):
                rows=copy.deepcopy(fixture())
                fn([r["metrics"] for r in rows])
                with self.assertRaises(SeekPlanError):
                    reconcile(rows,required=True)

if __name__=="__main__":
    unittest.main()
