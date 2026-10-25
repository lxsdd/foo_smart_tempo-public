"""Synthetic-only WAV oracle regressions; tests generation, not Smart Tempo accuracy."""
from __future__ import annotations

import json
import runpy
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import wave

GENERATOR = Path(__file__).resolve().parents[1] / "tools/generate_synthetic_bpm_reference.py"


class SyntheticBpmReferenceTests(unittest.TestCase):
    def generate(self, folder: Path):
        result = subprocess.run(
            [sys.executable, str(GENERATOR), "--output", str(folder)],
            text=True, capture_output=True, check=True,
        )
        self.assertIn("SYNTHETIC_REFERENCE_GENERATED=80", result.stdout)
        return json.loads((folder / "reference.json").read_text(encoding="utf-8"))

    def test_wave_structure_and_reference_oracle(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp) / "fixtures"
            data = self.generate(root)
            self.assertEqual(data["schema"], "synthetic_bpm_reference_v1")
            self.assertEqual(len(data["cases"]), 80)
            for case in data["cases"]:
                wav = root / (case["id"] + ".wav")
                with wave.open(str(wav), "rb") as reader:
                    self.assertEqual(reader.getnchannels(), case["channels"])
                    self.assertEqual(reader.getsampwidth(), 2)
                    self.assertEqual(reader.getframerate(), case["sample_rate"])
                    total = case["sample_rate"] * case["duration_sec"]
                    self.assertEqual(reader.getnframes(), total)
                events = case["event_sample_positions"]
                self.assertTrue(all(a < b for a, b in zip(events, events[1:])))
                self.assertTrue(all(0 <= pos < total for pos in events))
                if case["style"] == "single_click":
                    self.assertEqual(len(events), 1)
                elif case["style"] == "tempo_switch":
                    # Nonstationary: timing landmarks exist, but no global BPM.
                    self.assertIsNone(case["reference_bpm"])
                    self.assertGreater(len(events), 10)
                elif case["reference_bpm"] is None:
                    self.assertFalse(events)
                elif case["style"].startswith("drum_"):
                    # Declared musical beat grid, plus synthesized drum/hat
                    # timbres and alternating accents; no third-party samples.
                    delta = case["sample_rate"] * 60 / case["reference_bpm"]
                    self.assertGreater(len(events), 5)
                    self.assertTrue(
                        all(abs((b - a) - delta) <= 1 for a, b in zip(events, events[1:]))
                    )
                    self.assertGreater(wav.stat().st_size, 44)
                elif case["style"] == "regular":
                    # Oracle: exact beat spacing within one PCM sample.
                    delta = case["sample_rate"] * 60 / case["reference_bpm"]
                    self.assertGreater(len(events), 5)
                    self.assertTrue(
                        all(abs((b - a) - delta) <= 1 for a, b in zip(events, events[1:]))
                    )
            self.assertEqual(sum(c["style"].startswith("drum_")
                                 for c in data["cases"]), 31)
            self.assertEqual(sum(c["style"] == "silence" for c in data["cases"]), 1)
            self.assertEqual(sum(c["ambiguity"] == "meter_underdetermined"
                                 for c in data["cases"]), 6)
            self.assertEqual(sum(c["reference_bpm"] is None
                                 for c in data["cases"]), 6)

    def test_novel_challenge_family_and_nonstationary_reference(self):
        with tempfile.TemporaryDirectory() as temp:
            data = self.generate(Path(temp) / "newstyles")
            rows = {c["id"]: c for c in data["cases"]}
            self.assertEqual(rows["tempo_90_128_switch"]["ambiguity"],
                             "tempo_nonstationary")
            self.assertIsNone(rows["tempo_90_128_switch"]["reference_bpm"])
            self.assertFalse(rows["nonperiodic_percussion"]["event_sample_positions"])
            for name in ("drum_75_shuffle", "drum_125_triplet",
                         "drum_90_half_time", "drum_142_odd_five",
                         "drum_170_dropouts", "drum_174_dense_hats",
                         "drum_112_poly_three", "drum_137_ghost_notes"):
                case = rows[name]
                self.assertGreater(len(case["event_sample_positions"]), 10)
                self.assertGreater((Path(temp) / "newstyles" /
                                    (name + ".wav")).stat().st_size, 44)

    def test_long_window_sweep_inventory_and_truth(self):
        names = subprocess.check_output(
            [sys.executable, str(GENERATOR), "--list-window-cases"],
            text=True,
        ).splitlines()
        self.assertEqual(len(names), 6)
        self.assertEqual(len(set(names)), 6)
        self.assertTrue(all(name.startswith("window_") for name in names))
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp) / "long"
            subprocess.run(
                [sys.executable, str(GENERATOR), "--output", str(root),
                 "--window-case", "window_128_four_floor"],
                capture_output=True, text=True, check=True,
            )
            data = json.loads((root / "reference.json").read_text(encoding="utf-8"))
            self.assertEqual(len(data["cases"]), 1)
            case = data["cases"][0]
            self.assertEqual(case["duration_sec"], 120)
            self.assertEqual(case["reference_bpm"], 128.0)
            self.assertEqual(case["style"], "drum_four_floor")
            with wave.open(str(root / "window_128_four_floor.wav"), "rb") as wav:
                self.assertEqual(wav.getnframes(), 120 * wav.getframerate())
            self.assertGreater(len(case["event_sample_positions"]), 100)

    def test_ambiguous_subdivisions_share_identical_beat_positions(self):
        # An exact binary-equivalent pulse train cannot reveal whether every
        # second/fourth beat was omitted. These are HOLD/ambiguity fixtures.
        events = runpy.run_path(str(GENERATOR))["event_times"]
        self.assertEqual(events(128.0, "half_pulse", 20),
                         events(64.0, "regular", 20))
        self.assertEqual(events(120.0, "sparse", 20),
                         events(60.0, "half_pulse", 20))

    def test_independent_musical_grid_provenance_and_partitions(self):
        # The beat oracle is declared before synthesis and is not recovered
        # from the analyzer's chosen BPM or the audible onset list.
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp) / "development"
            data = self.generate(root)
            self.assertEqual(data["oracle_metadata_revision"], "musical_grid_v2")
            self.assertEqual({c["corpus_partition"] for c in data["cases"]},
                             {"development"})
            rows = {c["id"]: c for c in data["cases"]}
            for case in data["cases"]:
                beats = case["beat_sample_positions"]
                self.assertEqual(case["generator_seed"] is not None,
                                 case["seed_recipe"] == "fixed_xorshift32_v1")
                if case["reference_bpm"] is None:
                    self.assertEqual(beats, [])
                    self.assertEqual(case["beat_grid_status"],
                                     "not_globally_defined")
                else:
                    bpm = case["reference_bpm"]
                    rate = case["sample_rate"]
                    self.assertGreater(len(beats), 1)
                    self.assertTrue(all(a < b for a, b in zip(beats, beats[1:])))
                    for index, sample in enumerate(beats):
                        expected = round((0.25 + index * 60.0 / bpm) * rate)
                        self.assertEqual(sample, expected)
                    self.assertEqual(case["tempo_segments"], [])
            # Audible pulses do NOT identify all latent musical beats.
            self.assertLess(len(rows["pulse_128_half_pulse"]["event_sample_positions"]),
                            len(rows["pulse_128_half_pulse"]["beat_sample_positions"]))
            self.assertLess(len(rows["pulse_120_sparse"]["event_sample_positions"]),
                            len(rows["pulse_120_sparse"]["beat_sample_positions"]))
            self.assertEqual(rows["drum_142_odd_five"]["meter"], "5/4")
            self.assertEqual(rows["drum_112_poly_three"]["meter"], "4/4")
            self.assertEqual(rows["pulse_128"]["meter"], "unspecified")
            switched = rows["tempo_90_128_switch"]
            self.assertIsNone(switched["reference_bpm"])
            self.assertEqual([part["bpm"] for part in switched["tempo_segments"]],
                             [90.0, 128.0])
            self.assertEqual(switched["beat_sample_positions"], [])
            for part in switched["tempo_segments"]:
                self.assertLess(part["start_sample"], part["end_sample"])
            prospective = Path(temp) / "prospective"
            subprocess.run(
                [sys.executable, str(GENERATOR), "--output", str(prospective),
                 "--prospective-case", "prospective_breakbeat_189_60"],
                check=True, capture_output=True, text=True,
            )
            holdout = json.loads((prospective / "reference.json").read_text(
                encoding="utf-8"))["cases"]
            self.assertEqual(len(holdout), 1)
            self.assertEqual(holdout[0]["corpus_partition"],
                             "prospective_holdout")
            self.assertEqual(holdout[0]["reference_bpm"], 189.6)
            self.assertGreater(len(holdout[0]["beat_sample_positions"]), 20)

    def test_byte_reproducibility_and_no_overwrite(self):
        with tempfile.TemporaryDirectory() as temp:
            left = Path(temp) / "a"
            right = Path(temp) / "b"
            a = self.generate(left)
            b = self.generate(right)
            self.assertEqual(a, b)
            for case in a["cases"]:
                file_name = case["id"] + ".wav"
                self.assertEqual((left / file_name).read_bytes(), (right / file_name).read_bytes())
            blocked = subprocess.run(
                [sys.executable, str(GENERATOR), "--output", str(left)],
                capture_output=True, text=True,
            )
            self.assertNotEqual(blocked.returncode, 0)


if __name__ == "__main__":
    unittest.main()
