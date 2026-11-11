"""Source-only safeguards for the nonstationary original-host RAW fixture."""
from __future__ import annotations
from pathlib import Path
import tempfile
import unittest
import wave
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from generate_synthetic_raw_route import RATE, SECTION_SECONDS, TEMPOS, NAME, generate


class RawRouteGeneratorTests(unittest.TestCase):
    def test_four_distinct_fixed_regions_and_pcm_contract(self):
        self.assertEqual(TEMPOS, (103.0, 127.0, 154.0, 181.0))
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / NAME
            total, positions = generate(path)
            with wave.open(str(path), "rb") as wav:
                self.assertEqual(wav.getnchannels(), 1)
                self.assertEqual(wav.getsampwidth(), 2)
                self.assertEqual(wav.getframerate(), RATE)
                self.assertEqual(wav.getnframes(), total)
            self.assertEqual(total, RATE * SECTION_SECONDS * len(TEMPOS))
            self.assertTrue(positions)
            self.assertEqual(len(positions), len(set(positions)))
            for section in range(len(TEMPOS)):
                self.assertTrue(any(section * SECTION_SECONDS * RATE <= p <
                                    (section + 1) * SECTION_SECONDS * RATE
                                    for p in positions))
            first = path.read_bytes()
            generate(path)
            self.assertEqual(path.read_bytes(), first)

    def test_no_oracle_in_source_or_public_audio(self):
        source = (Path(__file__).resolve().parents[1] / "tools" /
                  "generate_synthetic_raw_route.py").read_text(encoding="utf-8")
        for forbidden in ("reference_bpm", "owner_path", "artist", "track_title",
                          "user_audio", "requests.get", "http://"):
            self.assertNotIn(forbidden, source)
        self.assertIn("output directory must be empty", source)
        self.assertIn("RAW selector coverage", source)


if __name__ == "__main__":
    unittest.main()
