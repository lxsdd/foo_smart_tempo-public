"""Complex public synthetic rhythm fixture generation and scoring contracts."""
from __future__ import annotations

import csv
import json
import math
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import wave

TOOLS = Path(__file__).resolve().parents[1] / "tools"
sys.path.insert(0, str(TOOLS))
from generate_synthetic_complex_challenge import CASES, render  # noqa: E402
from score_synthetic_complex_challenge import load_candidates, score  # noqa: E402


class ComplexSyntheticChallengeTests(unittest.TestCase):
    def test_fixed_cases_and_no_private_payload(self):
        result = subprocess.run(
            [sys.executable, str(TOOLS / "generate_synthetic_complex_challenge.py"),
             "--list-cases"], text=True, capture_output=True, check=True)
        ids = result.stdout.splitlines()
        self.assertEqual(len(ids), 16)
        self.assertEqual(ids, [c[0] for c in CASES])
        self.assertEqual(len(set(ids)), 16)
        self.assertTrue(all(s.startswith("challenge_") for s in ids))
        self.assertEqual(sum(bpm is not None for _, bpm, _ in CASES), 13)

    def test_exact_beat_grid_for_rhythm_complexity(self):
        with tempfile.TemporaryDirectory() as temp:
            for name, bpm, style in CASES:
                if bpm is None:
                    continue
                output = Path(temp) / (name + ".wav")
                row = render(name, bpm, style, output)
                positions = row["event_sample_positions"]
                self.assertGreater(len(positions), 5, name)
                delta = row["sample_rate"] * 60 / bpm
                self.assertTrue(
                    all(abs((b - a) - delta) <= 1
                        for a, b in zip(positions, positions[1:])), name)
                self.assertGreater(output.stat().st_size, 44)
                with wave.open(str(output), "rb") as wav:
                    self.assertEqual(wav.getnframes(),
                                     row["sample_rate"] * row["duration_sec"])
                if style in ("halftrap", "doubletrap"):
                    self.assertEqual(row["ambiguity"], "meter_underdetermined")
                else:
                    self.assertEqual(row["ambiguity"], "none")

    def test_deterministic_wave_hash_and_nonstationary_truth(self):
        with tempfile.TemporaryDirectory() as temp:
            left = Path(temp) / "one.wav"
            right = Path(temp) / "two.wav"
            row1 = render(CASES[2][0], CASES[2][1], CASES[2][2], left)
            row2 = render(CASES[2][0], CASES[2][1], CASES[2][2], right)
            self.assertEqual(row1, row2)
            self.assertEqual(left.read_bytes(), right.read_bytes())
            switch = CASES[-3]
            row = render(*switch, Path(temp) / "switch.wav")
            self.assertIsNone(row["reference_bpm"])
            self.assertEqual(row["ambiguity"], "tempo_nonstationary")
            self.assertEqual([s["bpm"] for s in row["tempo_segments"]],
                             [108.0, 142.0])
            self.assertGreater(len(row["event_sample_positions"]), 10)
            for name, bpm, style in CASES[-2:]:
                neg = render(name, bpm, style, Path(temp) / (name + ".wav"))
                self.assertIsNone(neg["reference_bpm"])
                self.assertEqual(neg["event_sample_positions"], [])

    def test_reject_existing_directory_and_unknown_id(self):
        with tempfile.TemporaryDirectory() as temp:
            target = Path(temp) / "fixtures"
            command = [sys.executable,
                       str(TOOLS / "generate_synthetic_complex_challenge.py"),
                       "--output", str(target), "--case", CASES[-1][0]]
            first = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(first.returncode, 0, first.stderr)
            self.assertTrue((target / "reference.json").is_file())
            second = subprocess.run(command, capture_output=True, text=True)
            self.assertNotEqual(second.returncode, 0)
            bad = subprocess.run(command[:-1] + ["unknown"],
                                 capture_output=True, text=True)
            self.assertNotEqual(bad.returncode, 0)

    def test_posthoc_score_and_fail_closed(self):
        refs = []
        obs = []
        timing = []
        for i, (name, bpm, style) in enumerate(CASES):
            amb = "meter_underdetermined" if style in (
                "halftrap", "doubletrap") else (
                "tempo_nonstationary" if style == "switch" else "none")
            refs.append({"id": name, "reference_bpm": bpm,
                         "style": "stress_" + style, "ambiguity": amb})
            # Correct on clear references, held on ambiguous/nonrhythmic.
            obs.append({"index": str(i), "bpm": str(bpm or 0.0),
                        "held": "1" if amb != "none" or bpm is None else "0"})
            timing.append({"index": str(i), "id": name,
                           "analysis_ms": str(300 + i * 10)})
        manifest = {"schema": "synthetic_bpm_reference_v1", "cases": refs}
        good = score(manifest, obs, timing)
        self.assertEqual(good["precision"]["clear_reference_n"], 11)
        self.assertEqual(good["precision"]["within_0_05_bpm"], 11)
        self.assertEqual(good["unsafe_count"], 0)
        obs[0]["bpm"] = str(CASES[0][1] * 2)
        wrong = score(manifest, obs, timing)
        self.assertEqual(wrong["unsafe_count"], 1)
        self.assertEqual(wrong["quality_verdict"], "NO_GO")
        timing[0]["analysis_ms"] = "nan"
        with self.assertRaises(ValueError):
            score(manifest, obs, timing)
        timing[0]["analysis_ms"] = "300"
        manifest["cases"][0]["reference_bpm"] = math.inf
        with self.assertRaises(ValueError):
            score(manifest, obs, timing)


    def test_complex_classification_and_candidate_gap_audit(self):
        refs, obs, timing = [], [], []
        for i, (name, bpm, style) in enumerate(CASES):
            amb = ("meter_underdetermined" if style in
                   ("halftrap", "doubletrap") else
                   "tempo_nonstationary" if style == "switch" else "none")
            refs.append({"id": name, "reference_bpm": bpm,
                         "style": "stress_" + style, "ambiguity": amb})
            obs.append({"index": str(i), "bpm": str(bpm or 0),
                        "held": "1" if amb != "none" or bpm is None else "0"})
            timing.append({"index": str(i), "id": name, "analysis_ms": "350"})
        manifest = {"schema": "synthetic_bpm_reference_v1", "cases": refs}
        obs[0]["bpm"] = str(CASES[0][1] * 2)
        obs[4].update(bpm="94.8", held="0")  # metrical ambiguity
        obs[13].update(bpm="108", held="0")  # tempo switch, not noise
        obs[14].update(bpm="123", held="0")  # nonrhythmic false accept
        candidates = {name: [] for name, _, _ in CASES}
        candidates[CASES[0][0]] = [CASES[0][1], CASES[0][1] * 2]
        scored = score(manifest, obs, timing, candidates)
        classes = scored["challenge_classes"]
        self.assertEqual((classes["clear_correct"], classes["clear_wrong"],
                          classes["clear_held"]), (10, 1, 0))
        self.assertEqual(classes["clear_half_double"], 1)
        self.assertEqual(classes["metrical_accepted"], 1)
        self.assertEqual(classes["nonstationary_accepted"], 1)
        self.assertEqual(classes["nonrhythmic_accepted"], 1)
        self.assertEqual(scored["candidate_audit"]["wrong_with_precise"], 1)
        self.assertEqual(scored["candidate_audit"]["wrong_without_precise"], 0)
        candidates[CASES[0][0]] = [CASES[0][1] * 2]
        gap = score(manifest, obs, timing, candidates)["candidate_audit"]
        self.assertEqual(gap["wrong_without_precise"], 1)
        self.assertEqual(gap["wrong_with_precise"], 0)
        candidates["not_a_public_case"] = [120.0]
        with self.assertRaises(ValueError):
            score(manifest, obs, timing, candidates)

    def test_challenge_candidate_csv_rejects_untrusted_or_unbounded(self):
        fields = ["id", "rank", "bpm", "evidence",
                  "support", "winner", "local_exact"]
        good = [CASES[0][0], "1", "73.25", "1.5", "1", "0", ".95"]
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "candidates.csv"
            def write(rows):
                with path.open("w", encoding="utf-8", newline="") as f:
                    writer = csv.writer(f)
                    writer.writerow(fields)
                    writer.writerows(rows)
            write([good, good])  # Duplicate rank is valid evidence.
            self.assertEqual(len(load_candidates(path)[CASES[0][0]]), 2)
            write([["unlisted_private_identity", *good[1:]]])
            with self.assertRaises(ValueError):
                load_candidates(path)
            write([[good[0], good[1], "nan", *good[3:]]])
            with self.assertRaises(ValueError):
                load_candidates(path)
            write([good] * 33)
            with self.assertRaises(ValueError):
                load_candidates(path)


if __name__ == "__main__":
    unittest.main()
