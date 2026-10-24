"""Predeclared synthetic parameter holdout: strict offline scoring tests."""
from __future__ import annotations

import csv
import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from generate_synthetic_bpm_reference import CASES, PROSPECTIVE_CASES  # noqa: E402
from score_prospective_bpm_reference import (  # noqa: E402
    CANDIDATE_FIELDS, aggregate, format_case_diagnostics,
    load_candidates, load_manifest,
)
from score_synthetic_bpm_reference import evaluate  # noqa: E402


class ProspectiveScorerTests(unittest.TestCase):
    def fixture(self):
        cases = []
        observed = []
        candidate_groups = {}
        for index, (name, bpm, style) in enumerate(PROSPECTIVE_CASES):
            ambiguous = (style in ("half_pulse", "sparse") or
                         style.startswith("tempo_switch"))
            cases.append({
                "id": name, "reference_bpm": bpm, "style": style,
                "ambiguity": ("meter_underdetermined"
                              if style in ("half_pulse", "sparse") else
                              "tempo_nonstationary"
                              if style.startswith("tempo_switch") else "none"),
            })
            observed.append({"index": str(index),
                             "bpm": str(bpm) if bpm is not None and
                             not ambiguous else "0",
                             "held": "0" if bpm is not None and
                             not ambiguous else "1"})
            candidate_groups[name] = ([{
                "id": name, "rank": "1", "bpm": str(bpm),
                "evidence": "2.0", "support": "1.0",
                "winner": "1.0", "local_exact": "0.98",
            }] if bpm is not None and not ambiguous else [])
        return {"schema": "synthetic_bpm_reference_v1",
                "cases": cases}, observed, candidate_groups

    def test_fixed_prospective_family_and_exact_fake_outcomes(self):
        self.assertEqual(len(PROSPECTIVE_CASES), 20)
        self.assertEqual(len({name for name, _, _ in PROSPECTIVE_CASES}), 20)
        self.assertFalse({name for name, _, _ in CASES} &
                         {name for name, _, _ in PROSPECTIVE_CASES})
        manifest, observed, candidates = self.fixture()
        with tempfile.TemporaryDirectory() as tmp:
            file = Path(tmp) / "manifest.json"
            file.write_text(json.dumps(manifest), encoding="utf-8")
            self.assertEqual(load_manifest(file), manifest)
        production, shadow = aggregate(evaluate(manifest, observed)["results"],
                                       candidates)
        self.assertEqual(production["clear"], 14)
        self.assertEqual(production["accurate"], 14)
        self.assertEqual(shadow["clear"], 14)
        self.assertEqual(shadow["accurate"], 14)
        self.assertEqual(shadow["wrong"], 0)
        self.assertEqual(shadow["ambiguous_accepted"], 0)
        self.assertEqual(shadow["no_tempo_accepted"], 0)

    def test_public_case_diagnostics_are_bounded_and_identity_checked(self):
        manifest, observed, candidates = self.fixture()
        scored = evaluate(manifest, observed)["results"]
        lines = format_case_diagnostics(scored, candidates)
        self.assertEqual(len(lines), 20)
        self.assertIn(
            "id=prospective_four_floor_97_25 cohort=clear "
            "reference=97.250000 production=97.250000 "
            "shadow=97.250000 near_candidate=1", lines[0])
        self.assertTrue(all(line.startswith("PROSPECTIVE_CASE id=prospective_")
                            for line in lines))
        forged = [dict(row) for row in scored]
        forged[0]["id"] = "owner_private_track"
        with self.assertRaises(ValueError):
            format_case_diagnostics(forged, candidates)

    def test_reference_tampering_is_rejected(self):
        manifest, _, _ = self.fixture()
        with tempfile.TemporaryDirectory() as tmp:
            file = Path(tmp) / "manifest.json"
            for forged in ("id", "style", "reference_bpm", "ambiguity"):
                altered = json.loads(json.dumps(manifest))
                altered["cases"][0][forged] = (
                    "owner_private_track" if forged in ("id", "style",
                                                        "ambiguity") else 999)
                file.write_text(json.dumps(altered), encoding="utf-8")
                with self.assertRaises(ValueError):
                    load_manifest(file)

    def test_candidate_cap_and_original_rank_beyond_32(self):
        _, _, grouped = self.fixture()
        good = dict(grouped[PROSPECTIVE_CASES[0][0]][0])
        good["rank"] = "33"  # 32 exported rows need not have ranks <= 32.
        with tempfile.TemporaryDirectory() as tmp:
            file = Path(tmp) / "candidates.csv"

            def write(items):
                with file.open("w", encoding="utf-8", newline="") as f:
                    writer = csv.DictWriter(f, fieldnames=CANDIDATE_FIELDS)
                    writer.writeheader()
                    writer.writerows(items)

            write([good])
            self.assertEqual(len(load_candidates(file)[good["id"]]), 1)
            write([{**good, "rank": "257"}])
            with self.assertRaises(ValueError):
                load_candidates(file)
            write([{**good, "id": "owner_private_track"}])
            with self.assertRaises(ValueError):
                load_candidates(file)
            write([{**good, "rank": str(index + 1)}
                   for index in range(33)])
            with self.assertRaises(ValueError):
                load_candidates(file)
            write([{**good, "local_exact": "nan"}])
            with self.assertRaises(ValueError):
                load_candidates(file)


if __name__ == "__main__":
    unittest.main()
