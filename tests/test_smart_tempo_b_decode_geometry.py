"""Actual frame/interval geometry: independent positive and negative CI oracles.

All frames and starts are fabricated; no music, reference BPM or track data.
"""
from __future__ import annotations
import copy
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from audit_smart_tempo_b_decode_geometry import DecodeGeometryError, reconcile


def window(index, total, start, *, source=16000, analysis=16000,
           requested=32000, decoded=32000, output=32000):
    return {"stage": "decode_window", "metrics": {
        "pass_index": index, "pass_total": total, "start_sec": start,
        "source_rate_hz": source, "analysis_rate_hz": analysis,
        "requested_frames": requested, "decoded_frames": decoded,
        "analysis_frames": output}}


def example():
    return [window(0, 3, 0), window(1, 3, 1), window(2, 3, 2)]


class NativeDecodeGeometryTests(unittest.TestCase):
    def test_actual_union_overlap_and_analysis_conservation(self):
        m = reconcile(example(), required=True)
        self.assertEqual(m["decode_geometry_verified"], 1)
        self.assertEqual(m["decode_geometry_passes"], 3)
        self.assertEqual(m["decode_geometry_truncated_passes"], 0)
        self.assertAlmostEqual(m["decode_geometry_coverage_seconds"], 4)
        self.assertAlmostEqual(m["decode_geometry_overlap_seconds"], 2)
        self.assertAlmostEqual(m["decode_geometry_analysis_seconds"], 6)
        self.assertAlmostEqual(m["decode_geometry_source_analysis_delta_seconds"], 0)

    def test_disjoint_passes_have_no_false_overlap(self):
        rows = [window(0, 2, 0), window(1, 2, 5)]
        m = reconcile(rows, required=True)
        self.assertEqual(m["decode_geometry_coverage_seconds"], 4)
        self.assertEqual(m["decode_geometry_overlap_seconds"], 0)

    def test_end_of_file_truncation_is_not_invented_coverage(self):
        rows = [window(0, 2, 0), window(1, 2, 1, decoded=16000, output=16000)]
        m = reconcile(rows, required=True)
        self.assertEqual(m["decode_geometry_truncated_passes"], 1)
        self.assertEqual(m["decode_geometry_coverage_seconds"], 2)
        self.assertEqual(m["decode_geometry_overlap_seconds"], 1)

    def test_96khz_resample_and_bounded_flush(self):
        rows = [window(0, 1, 0, source=96000, analysis=48000,
                       requested=192000, decoded=192000, output=95800)]
        m = reconcile(rows, required=True)
        self.assertEqual(m["decode_geometry_passes"], 1)
        self.assertAlmostEqual(m["decode_geometry_source_analysis_delta_seconds"],
                               -200 / 48000, places=5)

    def test_empty_and_partial_are_never_native_pass(self):
        self.assertEqual(reconcile([])["decode_geometry_verified"], 0)
        with self.assertRaises(DecodeGeometryError):
            reconcile([], required=True)
        partial = example()[:-1]
        self.assertEqual(reconcile(partial)["decode_geometry_verified"], 0)
        with self.assertRaises(DecodeGeometryError):
            reconcile(partial, required=True)

    def test_negative_adversarial_frame_rate_and_pass_mutations(self):
        mutations = {
            "same_rate_short": lambda x: x[0]["metrics"].__setitem__("analysis_frames", 1),
            "same_rate_long": lambda x: x[0]["metrics"].__setitem__("analysis_frames", 33000),
            "source_rate_drift": lambda x: x[1]["metrics"].__setitem__("source_rate_hz", 16001),
            "analysis_rate_drift": lambda x: x[1]["metrics"].__setitem__("analysis_rate_hz", 8000),
            "requested_drift": lambda x: x[1]["metrics"].__setitem__("requested_frames", 32001),
            "source_rate_cap": lambda x: x[0]["metrics"].__setitem__("analysis_rate_hz", 8000),
            "pass_gap": lambda x: x[1]["metrics"].__setitem__("pass_index", 2),
            "pass_duplicate": lambda x: x[1]["metrics"].__setitem__("pass_index", 0),
            "pass_total_drift": lambda x: x[1]["metrics"].__setitem__("pass_total", 4),
            "backward_seek": lambda x: x[2]["metrics"].__setitem__("start_sec", .5),
            "overdecode": lambda x: x[0]["metrics"].__setitem__("decoded_frames", 32001),
            "zero_frames": lambda x: x[0]["metrics"].__setitem__("decoded_frames", 0),
            "nonfinite_start": lambda x: x[0]["metrics"].__setitem__("start_sec", float("nan")),
            "fractional_counter": lambda x: x[0]["metrics"].__setitem__("analysis_frames", 1.5),
        }
        for name, mutation in mutations.items():
            with self.subTest(name=name):
                rows = copy.deepcopy(example())
                mutation(rows)
                with self.assertRaises(DecodeGeometryError):
                    reconcile(rows, required=True)

    def test_resampler_multi_second_loss_and_overrun_rejected(self):
        for output in (1, 150000):
            with self.subTest(output=output):
                rows = [window(0, 1, 0, source=96000, analysis=48000,
                               requested=192000, decoded=192000, output=output)]
                with self.assertRaises(DecodeGeometryError):
                    reconcile(rows, required=True)


if __name__ == "__main__":
    unittest.main()
