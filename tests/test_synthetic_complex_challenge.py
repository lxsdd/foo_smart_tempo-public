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
from score_synthetic_complex_challenge import (  # noqa: E402
    load_candidates, load_candidate_signals, score,
)


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


    def test_frozen_policy_never_uses_reference_to_select(self):
        refs, observed, timing = [], [], []
        signals = {name: [] for name, _, _ in CASES}
        for i, (name, bpm, style) in enumerate(CASES):
            ambiguity = ("meter_underdetermined" if style in
                         ("halftrap", "doubletrap") else
                         "tempo_nonstationary" if style == "switch" else "none")
            refs.append({"id": name, "reference_bpm": bpm,
                         "style": "stress_" + style, "ambiguity": ambiguity})
            observed.append({"index": str(i), "bpm": str(bpm or 0),
                             "held": "1" if ambiguity != "none" or bpm is None else "0"})
            timing.append({"index": str(i), "id": name, "analysis_ms": "350"})
            if ambiguity == "none" and bpm is not None:
                signals[name] = [{"rank": "1", "bpm": str(bpm),
                                  "local_exact": "0.98"}]
        signals[CASES[4][0]] = [
            {"rank": "1", "bpm": "94.8", "local_exact": "0.99"},
            {"rank": "2", "bpm": "189.6", "local_exact": "0.95"}]
        signals[CASES[5][0]] = [
            {"rank": "1", "bpm": "134.8", "local_exact": "0.94"}]
        signals[CASES[13][0]] = [
            {"rank": "1", "bpm": "142", "local_exact": "0.60"}]
        signals[CASES[14][0]] = [
            {"rank": "1", "bpm": "123", "local_exact": "0.40"}]
        signals[CASES[15][0]] = [
            {"rank": "1", "bpm": "123", "local_exact": "0.20"}]
        manifest = {"schema": "synthetic_bpm_reference_v1", "cases": refs}
        good = score(manifest, observed, timing, candidate_signals=signals)
        counts = good["frozen_shadow"]["counts"]
        self.assertEqual((counts["clear_correct"], counts["clear_wrong"],
                          counts["clear_held"]), (11, 0, 0))
        self.assertEqual((counts["metrical_accepted"],
                          counts["nonstationary_accepted"],
                          counts["nonrhythmic_accepted"]), (0, 0, 0))
        # High-scoring but wrong audio candidate stays WRONG, not corrected
        # by the post-hoc reference: this is deliberately NOT an oracle.
        signals[CASES[0][0]] = [
            {"rank": "1", "bpm": str(CASES[0][1] * 2),
             "local_exact": "0.99"}]
        wrong = score(manifest, observed, timing, candidate_signals=signals)
        self.assertEqual(wrong["frozen_shadow"]["counts"]["clear_wrong"], 1)
        # A close octave competitor forces a safe hold using audio alone.
        signals[CASES[0][0]].append(
            {"rank": "2", "bpm": str(CASES[0][1]),
             "local_exact": "0.95"})
        held = score(manifest, observed, timing, candidate_signals=signals)
        self.assertEqual(held["frozen_shadow"]["counts"]["clear_held"], 1)
        self.assertEqual(held["frozen_shadow"]["counts"]["clear_wrong"], 0)
        signals["not_a_public_case"] = []
        with self.assertRaises(ValueError):
            score(manifest, observed, timing, candidate_signals=signals)

    def test_signal_loader_preserves_strict_bounded_validation(self):
        fields = ["id", "rank", "bpm", "evidence",
                  "support", "winner", "local_exact"]
        row = [CASES[0][0], "1", "73.25", "1.5", "1", "0", ".98"]
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "candidates.csv"
            def write():
                with path.open("w", encoding="utf-8", newline="") as f:
                    writer = csv.writer(f)
                    writer.writerow(fields)
                    writer.writerow(row)
            write()
            signals = load_candidate_signals(path)
            self.assertEqual(signals[CASES[0][0]][0]["local_exact"], ".98")
            row[-1] = "nan"
            write()
            with self.assertRaises(ValueError):
                load_candidate_signals(path)


    def test_selection_path_audit_attribution_and_fail_closed(self):
        refs, observed, timing = [], [], []
        signals = {name: [] for name, _, _ in CASES}
        for i, (name, bpm, style) in enumerate(CASES):
            ambiguity = ("meter_underdetermined" if style in
                         ("halftrap", "doubletrap") else
                         "tempo_nonstationary" if style == "switch" else "none")
            refs.append({"id": name, "reference_bpm": bpm,
                         "style": "stress_" + style, "ambiguity": ambiguity})
            observed.append({"index": str(i), "bpm": str(bpm or 0),
                             "held": "1" if ambiguity != "none" or bpm is None else "0"})
            timing.append({"index": str(i), "id": name, "analysis_ms": "350"})
        for i in (0, 1, 2, 3):
            observed[i]["bpm"] = str(CASES[i][1] * (2 if i == 0 else 1.3))
        observed[6]["held"] = "1"
        signals[CASES[0][0]] = [
            {"rank": "1", "bpm": str(CASES[0][1] + .01), "local_exact": ".99"},
            {"rank": "2", "bpm": observed[0]["bpm"], "local_exact": ".90"}]
        signals[CASES[1][0]] = [
            {"rank": "1", "bpm": str(CASES[1][1]), "local_exact": ".98"}]
        signals[CASES[2][0]] = [
            {"rank": "1", "bpm": observed[2]["bpm"], "local_exact": ".80"}]
        signals[CASES[3][0]] = [
            {"rank": "1", "bpm": "170", "local_exact": ".80"}]
        signals[CASES[6][0]] = [
            {"rank": "1", "bpm": str(CASES[6][1]), "local_exact": ".98"}]
        manifest = {"schema": "synthetic_bpm_reference_v1", "cases": refs}
        audit = score(manifest, observed, timing,
                      candidate_signals=signals)["selection_path_audit"]
        c = audit["counts"]
        self.assertEqual((c["clear_total"], c["correct"], c["wrong"], c["held"]),
                         (11, 6, 4, 1))
        self.assertEqual((c["wrong_precise"], c["wrong_no_precise"],
                          c["wrong_precise_top_local"]), (2, 2, 2))
        self.assertEqual((c["precise_output_matches"], c["precise_output_unmatched"],
                          c["no_precise_output_matches"],
                          c["no_precise_output_unmatched"]), (1, 1, 1, 1))
        self.assertEqual(audit["details"][0]["precise_rank"], 1)
        self.assertEqual(audit["details"][0]["output_rank"], 2)
        signals[CASES[0][0]][0]["local_exact"] = "nan"
        with self.assertRaises(ValueError):
            score(manifest, observed, timing, candidate_signals=signals)




    def test_genre_scenarios_match_actual_cpp_default_routes(self):
        """Guard test-only C labels against C++ default-config drift.

        This is a source-snapshot check, NOT a simulated real-host B result.
        No private genre tags, audio or oracle values feed any selector.
        """
        import re
        from score_synthetic_complex_challenge import GENRE_SCENARIOS

        source = (TOOLS.parent / "src" / "foo_smart_tempo" /
                  "preferences.cpp").read_text(encoding="utf-8")
        default_blob = source.split("static const char* kDefaultGenreRules =", 1)[1]
        default_blob = default_blob.split('R"(', 1)[1].split(')"', 1)[0]
        mapping = {"house": "House", "techno": "Techno",
                   "slow": "Downtempo"}
        self.assertEqual(set(GENRE_SCENARIOS),
                         {"none", "house", "techno", "slow", "conflicting"})
        self.assertEqual(GENRE_SCENARIOS["none"], ())
        for scenario, tag in mapping.items():
            matches = [
                line for line in default_blob.splitlines()
                if tag in [piece.strip()
                           for piece in line.partition("=")[0].split(";")]
            ]
            self.assertEqual(len(matches), 1, tag)
            found = re.search(
                r"=\s*center\s+(\d+(?:\.\d+)?),\s*spread\s+(\d+(?:\.\d+)?)\s*$",
                matches[0])
            self.assertIsNotNone(found, tag)
            center, spread = (float(value) for value in found.groups())
            self.assertEqual(GENRE_SCENARIOS[scenario],
                             ((center - spread, center + spread),))
        self.assertEqual(
            GENRE_SCENARIOS["conflicting"],
            GENRE_SCENARIOS["house"] + GENRE_SCENARIOS["slow"])
        # Techno-default 120..148 excludes frozen 151.25-BPM test case,
        # though the earlier exploratory 125..155 range included it.
        self.assertFalse(
            any(lo <= 151.25 <= hi
                for lo, hi in GENRE_SCENARIOS["techno"]))
        self.assertTrue(
            all(lo <= 143.60 <= hi
                for lo, hi in GENRE_SCENARIOS["techno"]))

    def test_genre_counterfactual_uses_actual_board_and_preserves_denominators(self):
        from score_synthetic_complex_challenge import audit_genre_counterfactual
        signals = {name: [] for name, _, _ in CASES}
        rows = [{"id": name} for name, _, _ in CASES]
        for name, bpm, style in CASES:
            if bpm is not None and style not in ("halftrap", "doubletrap"):
                signals[name] = [{"rank": "1", "bpm": str(bpm),
                                  "local_exact": ".99"}]
        signals[CASES[0][0]].append(
            {"rank": "2", "bpm": "125", "local_exact": ".98"})
        signals[CASES[1][0]] = [
            {"rank": "1", "bpm": "125", "local_exact": ".99"},
            {"rank": "2", "bpm": "97.25", "local_exact": ".98"}]
        scored = {"results": rows}
        result = audit_genre_counterfactual(scored, signals)
        self.assertEqual(result["legacy_B"], "UNMEASURED")
        base = result["scenarios"]["none"]
        self.assertEqual((base["clear_correct"], base["clear_wrong"],
                          base["clear_held"]), (10, 1, 0))
        house = result["scenarios"]["house"]
        self.assertEqual((house["clear_correct"], house["clear_wrong"],
                          house["clear_held"]), (9, 1, 1))
        slow = result["scenarios"]["slow"]
        self.assertEqual((slow["clear_correct"], slow["clear_wrong"],
                          slow["clear_held"]), (10, 0, 1))
        # Frozen reference labels must match the source-defined default bounds.
        self.assertEqual(result["scenarios"]["techno"]["reference_in_range"], 2)
        self.assertEqual(result["scenarios"]["techno"]["reference_out_of_range"], 9)
        self.assertEqual(result["scenarios"]["techno"]["out_of_range_wrong"], 1)
        self.assertEqual(result["scenarios"]["conflicting"], base)
        self.assertEqual(house["reference_in_range"] +
                         house["reference_out_of_range"], 11)
        self.assertTrue(all(v["metrical_accepted"] == 0 and
                            v["nonrhythmic_accepted"] == 0
                            for v in result["scenarios"].values()))
        signals[CASES[15][0]] = [
            {"rank": "1", "bpm": "125", "local_exact": ".99"}]
        self.assertEqual(audit_genre_counterfactual(scored, signals)
                         ["scenarios"]["none"]["nonrhythmic_accepted"], 1)
        signals["not_public"] = []
        with self.assertRaises(ValueError):
            audit_genre_counterfactual(scored, signals)


if __name__ == "__main__":
    unittest.main()
