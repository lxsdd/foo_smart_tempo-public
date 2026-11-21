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

    def test_strict_absolute_bpm_denominators_and_negative_falseaccepts(self):
        refs = {"cases": [
            {"id": "a", "style": "regular", "reference_bpm": 128.0},
            {"id": "b", "style": "regular", "reference_bpm": 60.0},
            {"id": "c", "style": "sparse", "reference_bpm": 120.0},
            {"id": "d", "style": "silence", "reference_bpm": None},
        ]}
        p = score.evaluate(refs, [
            {"bpm": "128.04", "held": "0"},
            {"bpm": "60.19", "held": "0"},
            {"bpm": "90", "held": "1"},
            {"bpm": "123", "held": "0"},
        ])["precision"]
        self.assertEqual((p["reference_rhythmic"], p["accepted_rhythmic"],
                          p["held_rhythmic"]), (3, 2, 1))
        self.assertEqual((p["accepted_within_0_05_bpm"],
                          p["accepted_within_0_20_bpm"]), (1, 2))
        self.assertEqual(p["negative_total"], 1)
        self.assertEqual(p["negative_falseaccepts"], 1)

    def test_held_numeric_is_not_accepted(self):
        refs = {"cases": [
            {"id": "a", "style": "regular", "reference_bpm": 120.0},
            {"id": "b", "style": "silence", "reference_bpm": None},
        ]}
        p = score.evaluate(refs, [
            {"bpm": "120", "held": "1"},
            {"bpm": "0", "held": "1"},
        ])["precision"]
        self.assertEqual(p["accepted_rhythmic"], 0)
        self.assertEqual(p["accepted_within_0_05_bpm"], 0)
        self.assertEqual(p["negative_falseaccepts"], 0)

    def test_mismatch_and_invalid_bpm_fail_closed(self):
        reference = {"cases": [{"id": "pulse", "style": "regular", "reference_bpm": 120}]}
        with self.assertRaises(ValueError):
            score.evaluate(reference, [])
        for bad in ("nan", "inf", "-9"):
            with self.subTest(bpm=bad), self.assertRaises(ValueError):
                score.evaluate(reference, [{"bpm": bad, "held": "0"}])

    def test_observation_ordinals_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            observed = Path(temp) / "numbers.csv"
            observed.write_text("index,bpm,held\n1,120,0\n", encoding="utf-8")
            with self.assertRaises(ValueError):
                score.load_observed(observed)


if __name__ == "__main__":
    unittest.main()
