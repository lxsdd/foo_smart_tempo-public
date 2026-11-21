"""Synthetic-only WAV oracle regressions; tests generation, not Smart Tempo accuracy."""
from __future__ import annotations

import json
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
        self.assertIn("SYNTHETIC_REFERENCE_GENERATED=17", result.stdout)
        return json.loads((folder / "reference.json").read_text(encoding="utf-8"))

    def test_wave_structure_and_reference_oracle(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp) / "fixtures"
            data = self.generate(root)
            self.assertEqual(data["schema"], "synthetic_bpm_reference_v1")
            self.assertEqual(len(data["cases"]), 17)
            for case in data["cases"]:
                wav = root / (case["id"] + ".wav")
                with wave.open(str(wav), "rb") as reader:
                    self.assertEqual(reader.getnchannels(), 1)
                    self.assertEqual(reader.getsampwidth(), 2)
                    self.assertEqual(reader.getframerate(), 16000)
                    self.assertEqual(reader.getnframes(), 320000)
                events = case["event_sample_positions"]
                self.assertTrue(all(a < b for a, b in zip(events, events[1:])))
                self.assertTrue(all(0 <= t < 320000 for t in events))
                if case["reference_bpm"] is None:
                    self.assertFalse(events)
                elif case["style"] == "regular":
                    # Oracle: exact beat spacing within one PCM sample.
                    delta = 16000 * 60 / case["reference_bpm"]
                    self.assertGreater(len(events), 5)
                    self.assertTrue(
                        all(abs((b - a) - delta) <= 1 for a, b in zip(events, events[1:]))
                    )
            self.assertEqual(sum(c["style"] == "silence" for c in data["cases"]), 1)

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
