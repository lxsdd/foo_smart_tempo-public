"""Independent immutable SHA and beat-time anchors for already-seen public WAVs."""
from __future__ import annotations
import hashlib
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from generate_synthetic_bpm_reference import CASES, PROSPECTIVE_CASES, render, oracle_beat_grid

FROZEN_SHA = {
    "silence": "fe04e06676e54a18bce6696508d2696394f172b365445e1f2eb969dc0a10f163",
    "pulse_120": "3a294df548daa355e50c642951845f8e443693a576fb202771b1c45572d6ef48",
    "pulse_128": "fbd3e0e362474974e3782c0da5391cbfcd1d43531e7e89c2b7f7a7e48fab9c92",
}
FROZEN_GRID = {
    "prospective_four_floor_97_25": (4000, 310015, 32),
    "prospective_breakbeat_96_70": (4000, 311756, 32),
    "prospective_breakbeat_144_20": (4000, 316899, 48),
    "prospective_breakbeat_189_60": (4000, 317924, 63),
    "prospective_poly_three_143_60": (4000, 318206, 48),
}


class FrozenOracleTests(unittest.TestCase):
    def test_three_fixed_public_pcm_riff_hashes(self):
        known = {row[0]: row for row in CASES}
        with tempfile.TemporaryDirectory() as tmp:
            for name, expected in FROZEN_SHA.items():
                with self.subTest(name=name):
                    file = Path(tmp) / (name + ".wav")
                    manifest_row = render(*known[name], file)
                    self.assertEqual(file.stat().st_size, 640044)
                    actual = hashlib.sha256(file.read_bytes()).hexdigest()
                    self.assertEqual(actual, expected)
                    self.assertEqual(manifest_row["wav_sha256"], expected)
                    tampered = bytearray(file.read_bytes())
                    tampered[-1] ^= 1
                    self.assertNotEqual(hashlib.sha256(tampered).hexdigest(), expected)

    def test_five_fixed_prospective_beat_landmarks(self):
        known = {row[0]: row for row in PROSPECTIVE_CASES}
        self.assertEqual(len(FROZEN_GRID), 5)
        for name, (first, last, count) in FROZEN_GRID.items():
            with self.subTest(name=name):
                bpm = known[name][1]
                positions = oracle_beat_grid(bpm, known[name][2], 20, 16000)
                self.assertEqual((positions[0], positions[-1], len(positions)),
                                 (first, last, count))
                self.assertTrue(all(a < b for a, b in zip(positions, positions[1:])))

    def test_seen_prospective_partition_is_not_a_blind_holdout(self):
        # Names have been analyzed on previous host jobs; never cite as unseen.
        self.assertTrue(all(name.startswith("prospective_") for name in FROZEN_GRID))
        self.assertNotIn("unseen_holdout", {"prospective_holdout", "development"})


if __name__ == "__main__":
    unittest.main()
