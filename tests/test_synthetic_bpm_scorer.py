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


    def test_measured_candidate_availability_is_reported_not_selected(self):
        refs = {"cases": [
            {"id": "pulse_60", "style": "regular", "reference_bpm": 60},
            {"id": "pulse_90", "style": "regular", "reference_bpm": 90},
            {"id": "noise", "style": "noise", "reference_bpm": None},
        ]}
        observed = [
            {"bpm": "90", "held": "0"},
            {"bpm": "90", "held": "0"},
            {"bpm": "0", "held": "1"},
        ]
        result = score.evaluate(refs, observed)
        evidence = [
            {"id": "pulse_60", "rank": "1", "bpm": "60.01",
             "evidence": "1.0", "support": "1", "winner": "0",
             "local_exact": "0.8"},
            {"id": "pulse_60", "rank": "2", "bpm": "90",
             "evidence": "0.1", "support": "1", "winner": "1",
             "local_exact": "0.5"},
            {"id": "pulse_90", "rank": "1", "bpm": "90",
             "evidence": "1", "support": "1", "winner": "1",
             "local_exact": "1"},
        ]
        audit = score.inspect_candidate_inventory(result, evidence)
        self.assertEqual(audit["incorrect_clear_reference_n"], 1)
        self.assertEqual(audit["correct_measured_candidate_present_n"], 1)
        self.assertEqual(audit["chosen_measured_candidate_present_n"], 1)
        self.assertEqual(audit["candidate_records_n"], 3)
        self.assertEqual(audit["per_error"], [("pulse_60", 1, 1)])
        self.assertEqual(result["results"][0]["reported_bpm"], 90)

        # The result remains a violation despite a valid measured alternate.
        self.assertEqual(result["quality_verdict"], "NO_GO")

    def test_candidate_inventory_fails_closed_on_non_public_id_or_values(self):
        header = ["id", "rank", "bpm", "evidence", "support",
                  "winner", "local_exact"]
        base = {"id": "pulse_60", "rank": "1", "bpm": "60.01",
                "evidence": "1.0", "support": "1", "winner": "1",
                "local_exact": "0.8"}
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "public-candidates.csv"
            for change in ({"id": "private-track"},
                           {"rank": "33"}, {"bpm": "nan"},
                           {"evidence": "inf"}, {"bpm": "-5"}):
                row = {**base, **change}
                with path.open("w", encoding="utf-8", newline="") as handle:
                    writer = csv.DictWriter(handle, fieldnames=header)
                    writer.writeheader()
                    writer.writerow(row)
                with self.subTest(change=change), self.assertRaises(ValueError):
                    score.load_candidate_inventory(path)
            with path.open("w", encoding="utf-8", newline="") as handle:
                writer = csv.DictWriter(handle, fieldnames=header)
                writer.writeheader()
                writer.writerow(base)
            self.assertEqual(len(score.load_candidate_inventory(path)), 1)


    def test_prespecified_drum_pulse_comparison_classifies_fix_and_regression(self):
        refs = {"cases": [
            {"id": "pulse_60", "style": "regular", "reference_bpm": 60},
            {"id": "drum_60_four_floor", "style": "drum_four_floor", "reference_bpm": 60},
            {"id": "pulse_175_syncopated", "style": "syncopated", "reference_bpm": 175},
            {"id": "drum_175_breakbeat", "style": "drum_breakbeat", "reference_bpm": 175},
            {"id": "pulse_128_8s", "style": "regular", "reference_bpm": 128},
            {"id": "drum_128_8s", "style": "drum_four_floor", "reference_bpm": 128},
        ]}
        observed = [
            {"bpm": "60", "held": "0"},
            {"bpm": "120", "held": "0"},
            {"bpm": "87.5", "held": "0"},
            {"bpm": "175", "held": "0"},
            {"bpm": "63.97", "held": "0"},
            {"bpm": "127.99", "held": "0"},
        ]
        report = score.compare_drum_pulse_pairs(score.evaluate(refs, observed))
        self.assertEqual(report["counts"]["n"], 3)
        self.assertEqual(report["counts"]["drum_fixed_n"], 2)
        self.assertEqual(report["counts"]["drum_regressed_n"], 1)
        self.assertEqual(report["counts"]["both_wrong_n"], 0)
        self.assertEqual(len(report["details"]), 3)

    def test_shadow_summary_reports_counterfactuals_and_ambiguity(self):
        refs = {"cases": [
            {"id": "pulse_60", "style": "regular", "reference_bpm": 60},
            {"id": "pulse_128_half_pulse", "style": "half_pulse",
             "reference_bpm": 128, "ambiguity": "meter_underdetermined"},
            {"id": "steady_tone", "style": "tone", "reference_bpm": None},
        ]}
        observed = [
            {"bpm": "90", "held": "0"},
            {"bpm": "64", "held": "0"},
            {"bpm": "133.5", "held": "0"},
        ]
        evidence = [
            {"id": "pulse_60", "rank": "1", "bpm": "90", "evidence": "9",
             "support": "1", "winner": "1", "local_exact": "0.3"},
            {"id": "pulse_60", "rank": "2", "bpm": "60.01", "evidence": "1",
             "support": "1", "winner": "0", "local_exact": "0.98"},
            {"id": "pulse_128_half_pulse", "rank": "1", "bpm": "64",
             "evidence": "4", "support": "1", "winner": "1", "local_exact": "0.9"},
            {"id": "steady_tone", "rank": "1", "bpm": "133.5",
             "evidence": "1", "support": "1", "winner": "1", "local_exact": "0.15"},
        ]
        scored = score.evaluate(refs, observed)
        shadow = score.summarize_shadow_candidates(scored, evidence)
        self.assertEqual(shadow["clear_n"], 1)
        self.assertEqual(shadow["local_exact_within_0_20_n"], 1)
        self.assertEqual(shadow["raw_evidence_within_0_20_n"], 0)
        self.assertEqual(shadow["rank1_within_0_20_n"], 0)
        self.assertEqual(shadow["local_exact_ambiguous_proposals_n"], 1)
        self.assertEqual(shadow["local_exact_nonrhythmic_proposals_n"], 1)
        self.assertEqual(scored["results"][0]["reported_bpm"], 90)
        with self.assertRaises(ValueError):
            score.summarize_shadow_candidates(scored,
                [{**evidence[0], "id": "private-owner-audio"}])


    def test_predeclared_shadow_profiles_are_reference_blind_and_split(self):
        refs = {"cases": [
            {"id": "pulse_60", "reference_bpm": 60.0,
             "style": "regular"},
            {"id": "drum_75_shuffle", "reference_bpm": 75.0,
             "style": "drum_shuffle"},
            {"id": "tempo_90_128_switch", "reference_bpm": None,
             "ambiguity": "tempo_nonstationary", "style": "tempo_switch"},
            {"id": "nonperiodic_percussion", "reference_bpm": None,
             "style": "nonperiodic_percussion"},
        ]}
        observed = [
            {"bpm": "120", "held": "0"},
            {"bpm": "150", "held": "0"},
            {"bpm": "90", "held": "0"},
            {"bpm": "100", "held": "0"},
        ]
        def record(name, rank, bpm, exact):
            return {"id": name, "rank": str(rank), "bpm": str(bpm),
                    "local_exact": str(exact)}
        entries = [
            record("pulse_60", 1, 120, .5),
            record("pulse_60", 2, 60, .95),
            record("drum_75_shuffle", 1, 150, .2),
            record("drum_75_shuffle", 2, 75, .81),
            record("tempo_90_128_switch", 1, 90, .91),
            record("tempo_90_128_switch", 2, 180, .89),
            record("nonperiodic_percussion", 1, 100, .1),
        ]
        report = score.evaluate_shadow_profiles(
            score.evaluate(refs, observed), entries)
        self.assertEqual(report[("calibration", "production")]["clear_wrong_n"], 1)
        self.assertEqual(report[("calibration", "exact_top")]["clear_accurate_n"], 1)
        self.assertEqual(report[("challenge", "production")]["ambiguous_accepted_n"], 1)
        self.assertEqual(report[("challenge", "production")]["no_tempo_accepted_n"], 1)
        self.assertEqual(report[("challenge", "exact_top")]["clear_accurate_n"], 1)
        self.assertEqual(report[("challenge", "exact_alias_margin_005")]["ambiguous_accepted_n"], 0)
        self.assertEqual(report[("challenge", "exact_floor_075")]["no_tempo_accepted_n"], 0)
        frozen = report[("challenge", "frozen_prospective_095_010")]
        self.assertEqual(frozen["clear_held_n"], 1)
        self.assertEqual(frozen["ambiguous_accepted_n"], 0)
        self.assertEqual(frozen["no_tempo_accepted_n"], 0)
        self.assertEqual(report[("calibration", "frozen_prospective_095_010")]["clear_accurate_n"], 1)
        self.assertEqual(score.shadow_candidate_choice(entries[4:6], .5, .05), None)
        self.assertEqual(score.shadow_candidate_choice(entries[4:6], .5, -.1), 90)

    def test_observation_ordinals_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            observed = Path(temp) / "numbers.csv"
            observed.write_text("index,bpm,held\n1,120,0\n", encoding="utf-8")
            with self.assertRaises(ValueError):
                score.load_observed(observed)


if __name__ == "__main__":
    unittest.main()
