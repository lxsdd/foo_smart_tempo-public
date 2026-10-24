"""Fail-closed public 11-by-3 real-host pass-sweep scorer tests."""
from __future__ import annotations

import csv
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from generate_synthetic_bpm_reference import PROSPECTIVE_CASES  # noqa: E402
from score_prospective_pass_sweep import (  # noqa: E402
    CANDIDATE_FIELDS, OBSERVED_FIELDS, PASSES, TARGETS, keys,
    read_candidates, read_observed, report, status,
)


class ProspectivePassSweepTests(unittest.TestCase):
    def test_predeclared_grid_and_safety_classes(self):
        self.assertEqual(len(TARGETS), 11)
        self.assertEqual(len(keys()), 33)
        self.assertEqual(PASSES, (1, 5, 20))
        self.assertTrue(set(TARGETS) <=
                        {name for name, _, _ in PROSPECTIVE_CASES})
        clear = {"ambiguous": False, "reference_bpm": 96.70}
        self.assertEqual(status(clear, 96.80), "accurate")
        self.assertEqual(status(clear, 97.0), "wrong")
        self.assertEqual(status(clear, None), "held")
        self.assertEqual(status({"ambiguous": True, "reference_bpm": None},
                                108), "ambiguous_accept")
        self.assertEqual(status({"ambiguous": False, "reference_bpm": None},
                                90), "no_tempo_accept")

    def write(self, path, fields, rows):
        with path.open("w", encoding="utf-8", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=fields)
            writer.writeheader()
            writer.writerows(rows)

    def fixtures(self):
        observed = [
            {"id": name, "passes": str(passes), "bpm": "0", "held": "1",
             "effective_seconds": "19.5", "effective_passes": str(passes)}
            for name in TARGETS for passes in PASSES
        ]
        candidate = {
            "id": TARGETS[0], "passes": "1", "rank": "33", "bpm": "97.25",
            "evidence": "1.0", "support": "1.0", "winner": "1.0",
            "local_exact": "0.99",
        }
        return observed, candidate

    def test_strict_matrix_and_candidate_bounds(self):
        observations, candidate = self.fixtures()
        with tempfile.TemporaryDirectory() as tmp:
            observed_path = Path(tmp) / "observed.csv"
            candidate_path = Path(tmp) / "candidates.csv"
            self.write(observed_path, OBSERVED_FIELDS, observations)
            self.write(candidate_path, CANDIDATE_FIELDS, [candidate])
            self.assertEqual(len(read_observed(observed_path)), 33)
            self.assertEqual(len(read_candidates(candidate_path)), 33)
            self.write(candidate_path, CANDIDATE_FIELDS,
                       [candidate, {**candidate, "bpm": "98.0"}])
            self.assertEqual(len(read_candidates(candidate_path)[
                (TARGETS[0], 1)]), 2)
            self.write(observed_path, OBSERVED_FIELDS, observations[:-1])
            with self.assertRaises(ValueError):
                read_observed(observed_path)
            self.write(observed_path, OBSERVED_FIELDS,
                       observations[:-1] + [{**observations[-1],
                                              "id": "owner_private_track"}])
            with self.assertRaises(ValueError):
                read_observed(observed_path)
            self.write(observed_path, OBSERVED_FIELDS,
                       observations[:-1] + [{**observations[-1],
                                              "effective_seconds": "nan"}])
            with self.assertRaises(ValueError):
                read_observed(observed_path)
            for bad in ({**candidate, "id": "owner_private_track"},
                        {**candidate, "rank": "257"},
                        {**candidate, "local_exact": "nan"}):
                self.write(candidate_path, CANDIDATE_FIELDS, [bad])
                with self.assertRaises(ValueError):
                    read_candidates(candidate_path)
            self.write(candidate_path, CANDIDATE_FIELDS,
                       [{**candidate, "rank": str(n + 1)} for n in range(33)])
            with self.assertRaises(ValueError):
                read_candidates(candidate_path)

    def test_batched_report_uses_all_axes_and_rejects_foreign_ids(self):
        observations, candidate = self.fixtures()
        manifest = {"cases": [{"id": name} for name, _, _ in
                              PROSPECTIVE_CASES]}
        baseline = []
        baseline_boards = {}
        for name, bpm, style in PROSPECTIVE_CASES:
            ambiguous = (style in ("half_pulse", "sparse") or
                         style.startswith("tempo_switch"))
            baseline.append({"id": name, "reference_bpm": bpm,
                             "ambiguous": ambiguous, "held": True,
                             "reported_bpm": 0.0})
            baseline_boards[name] = []
        measured = {
            (row["id"], int(row["passes"])):
            {"bpm": 0.0, "held": True, "effective_seconds": 19.5,
             "effective_passes": int(row["passes"])}
            for row in observations
        }
        boards = {key: [] for key in keys()}
        boards[(candidate["id"], 1)] = [candidate]
        lines = report(manifest, baseline, baseline_boards, measured, boards)
        self.assertEqual(sum(line.startswith("PASS_SWEEP_CASE ") for line in lines),
                         44)
        self.assertEqual(sum(line.startswith("PASS_SWEEP_SUMMARY ") for line in lines),
                         4)
        self.assertTrue(all("owner_private_track" not in line for line in lines))
        forged = [dict(row) for row in baseline]
        forged[0]["id"] = "owner_private_track"
        with self.assertRaises(ValueError):
            report(manifest, forged, baseline_boards, measured, boards)


if __name__ == "__main__":
    unittest.main()
