"""Synthetic BPM scorer regressions; no owner audio."""
import importlib.util
from pathlib import Path
import tempfile
import unittest
import csv

script = Path(__file__).resolve().parents[1] / "tools/score_synthetic_bpm_reference.py"
spec = importlib.util.spec_from_file_location("synthetic_score", script)
score = importlib.util.module_from_spec(spec)
spec.loader.exec_module(score)


class ScorerTests(unittest.TestCase):
    def test_exact_alias_hold_and_silence(self):
        refs = {"cases": [
            {"id": "pulse", "style": "regular", "reference_bpm": 120},
            {"id": "alias", "style": "regular", "reference_bpm": 128},
            {"id": "hold", "style": "syncopated", "reference_bpm": 140},
            {"id": "quiet", "style": "silence", "reference_bpm": None},
        ]}
        rows = [
            {"bpm": "119.9", "held": "0"},
            {"bpm": "64", "held": "0"},
            {"bpm": "141", "held": "1"},
            {"bpm": "0", "held": "1"},
        ]
        result = score.evaluate(refs, rows)
        self.assertEqual(result["counts"]["within_two_percent"], 1)
        self.assertEqual(result["counts"]["half_double_alias"], 1)
        self.assertEqual(result["counts"]["abstained"], 1)
        self.assertEqual(result["counts"]["correct_abstention"], 1)

    def test_mismatch_and_invalid_bpm_fail_closed(self):
        reference = {"cases": [{"id": "pulse", "style": "regular", "reference_bpm": 120}]}
        with self.assertRaises(ValueError):
            score.evaluate(reference, [])
        for bad in ("nan", "inf", "-9"):
            with self.subTest(bpm=bad), self.assertRaises(ValueError):
                score.evaluate(reference, [{"bpm": bad, "held": "0"}])

    def test_false_positive_silence_and_held_alias_are_not_accuracy_passes(self):
        reference = {"cases": [
            {"id": "silence", "style": "silence", "reference_bpm": None},
            {"id": "half", "style": "half_pulse", "reference_bpm": 128},
        ]}
        result = score.evaluate(reference, [
            {"bpm": "120", "held": "0"},
            {"bpm": "64", "held": "1"},
        ])
        self.assertEqual(result["counts"]["false_positive_silence"], 1)
        self.assertEqual(result["counts"]["abstained"], 1)
        self.assertEqual(result["counts"]["within_two_percent"], 0)
        self.assertEqual(result["counts"]["half_double_alias"], 0)

    def test_absolute_precision_tiers_and_public_detail_guard(self):
        refs = {"cases": [
            {"id": "pulse_90", "style": "regular", "reference_bpm": 90.0},
            {"id": "pulse_128", "style": "regular", "reference_bpm": 128.0},
            {"id": "pulse_175", "style": "regular", "reference_bpm": 175.0},
        ]}
        observed = [
            {"bpm": "90.049", "held": "0"},
            {"bpm": "128.199", "held": "0"},
            {"bpm": "175.201", "held": "0"},
        ]
        result = score.evaluate(refs, observed)
        self.assertEqual(result["precision"]["accepted_reference_n"], 3)
        self.assertEqual(result["precision"]["within_0_05_bpm"], 1)
        self.assertEqual(result["precision"]["within_0_20_bpm"], 2)
        self.assertAlmostEqual(result["results"][0]["absolute_error_bpm"], 0.049)
        lines = list(score.precision_detail_lines(result))
        self.assertEqual(len(lines), 3)
        self.assertIn("abs_error_bpm=0.049000", lines[0])

        private_label = {"cases": [
            {"id": "private-track", "style": "regular", "reference_bpm": 120},
        ]}
        scored = score.evaluate(private_label, [{"bpm": "120", "held": "0"}])
        with self.assertRaises(ValueError):
            list(score.precision_detail_lines(scored))

    def test_ambiguity_and_negative_false_accept_do_not_hide_in_precision(self):
        refs = {"cases": [
            {"id": "pulse_128_half_pulse", "style": "half_pulse",
             "reference_bpm": 128.0, "ambiguity": "meter_underdetermined"},
            {"id": "pulse_120_sparse", "style": "sparse",
             "reference_bpm": 120.0, "ambiguity": "meter_underdetermined"},
            {"id": "noise", "style": "noise", "reference_bpm": None},
            {"id": "pulse_123_45", "style": "regular", "reference_bpm": 123.45},
        ]}
        measured = [
            {"bpm": "128", "held": "0"},
            {"bpm": "0", "held": "1"},
            {"bpm": "90", "held": "0"},
            {"bpm": "123.6", "held": "0"},
        ]
        result = score.evaluate(refs, measured)
        self.assertEqual(result["counts"]["ambiguous_accept"], 1)
        self.assertEqual(result["counts"]["ambiguous_hold"], 1)
        self.assertEqual(result["counts"]["false_positive_nonrhythmic"], 1)
        self.assertEqual(result["precision"]["clear_reference_n"], 1)
        self.assertEqual(result["precision"]["within_0_20_bpm"], 1)
        self.assertEqual(result["unsafe_count"], 2)
        self.assertEqual(result["quality_verdict"], "NO_GO")

    def test_unambiguous_error_larger_than_point_two_is_quality_no_go(self):
        refs = {"cases": [
            {"id": "pulse_90", "style": "regular", "reference_bpm": 90.0}
        ]}
        result = score.evaluate(refs, [{"bpm": "90.3", "held": "0"}])
        self.assertEqual(result["counts"]["within_two_percent"], 1)
        self.assertEqual(result["precision"]["within_0_20_bpm"], 0)
        self.assertEqual(result["unsafe_count"], 1)
        self.assertEqual(result["quality_verdict"], "NO_GO")

    def test_observation_ordinals_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            observed = Path(temp) / "numbers.csv"
            observed.write_text("index,bpm,held\n1,120,0\n", encoding="utf-8")
            with self.assertRaises(ValueError):
                score.load_observed(observed)


if __name__ == "__main__":
    unittest.main()
