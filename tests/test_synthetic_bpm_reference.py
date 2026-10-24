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
        self.assertIn("SYNTHETIC_REFERENCE_GENERATED=60", result.stdout)
        return json.loads((folder / "reference.json").read_text(encoding="utf-8"))

    def test_wave_structure_and_reference_oracle(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp) / "fixtures"
            data = self.generate(root)
            self.assertEqual(data["schema"], "synthetic_bpm_reference_v1")
            self.assertEqual(len(data["cases"]), 60)
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
                elif case["reference_bpm"] is None:
                    self.assertFalse(events)
                elif case["style"] in ("drum_four_floor", "drum_breakbeat"):
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
                                 for c in data["cases"]), 13)
            self.assertEqual(sum(c["style"] == "silence" for c in data["cases"]), 1)
            self.assertEqual(sum(c["ambiguity"] == "meter_underdetermined"
                                 for c in data["cases"]), 6)
            self.assertEqual(sum(c["reference_bpm"] is None
                                 for c in data["cases"]), 4)

    def test_ambiguous_subdivisions_share_identical_beat_positions(self):
        # An exact binary-equivalent pulse train cannot reveal whether every
        # second/fourth beat was omitted. These are HOLD/ambiguity fixtures.
        events = runpy.run_path(str(GENERATOR))["event_times"]
        self.assertEqual(events(128.0, "half_pulse", 20),
                         events(64.0, "regular", 20))
        self.assertEqual(events(120.0, "sparse", 20),
                         events(60.0, "half_pulse", 20))

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
