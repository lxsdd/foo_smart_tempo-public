"""Boundary tests for a real-host synthetic window/pass comparison scorer."""
from __future__ import annotations

import csv
from pathlib import Path
import runpy
import subprocess
import sys
import tempfile
import unittest

SCORER = Path(__file__).resolve().parents[1] / "tools/score_synthetic_window_sweep.py"
MOD = runpy.run_path(str(SCORER))
REFERENCE = MOD["REFERENCE"]
AXES = MOD["AXES"]
COLUMNS = MOD["REQUIRED_COLUMNS"]


class WindowSweepScorerTests(unittest.TestCase):
    def test_statuses_respect_holds_aliases_and_abstention(self):
        classify = MOD["classify"]
        self.assertEqual(classify(128.02, 0, 128.0), "exact")
        self.assertEqual(classify(64.0, 0, 128.0), "tempo_alias")
        self.assertEqual(classify(119.0, 0, 128.0), "wrong")
        self.assertEqual(classify(128.0, 1, 128.0), "held")
        self.assertEqual(classify(0.0, 1, None), "safe_hold")
        self.assertEqual(classify(0.0, 0, None), "safe_hold")
        self.assertEqual(classify(90.0, 0, None), "false_accept")

    def _rows(self):
        rows = []
        for name, (truth, style, ambiguity) in REFERENCE.items():
            for seconds, passes in AXES:
                rows.append({
                    "id": name, "style": style,
                    "reference_bpm": "" if truth is None else str(truth),
                    "ambiguity": ambiguity,
                    "seconds": seconds, "passes": passes,
                    "effective_seconds": float(seconds),
                    "effective_passes": passes,
                    "bpm": "0" if truth is None else str(truth),
                    "held": "1" if truth is None else "0",
                    "analysis_ms": "1234",
                })
        return rows

    def _invoke(self, rows):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "measurements.csv"
            with path.open("w", newline="", encoding="utf-8") as stream:
                writer = csv.DictWriter(stream, fieldnames=COLUMNS)
                writer.writeheader()
                writer.writerows(rows)
            return subprocess.run(
                [sys.executable, str(SCORER), "--observed", str(path)],
                capture_output=True, text=True,
            )

    def test_complete_paired_grid_and_summary(self):
        rows = self._rows()
        self.assertEqual(len(rows), 78)
        completed = self._invoke(rows)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertIn("SYNTHETIC_WINDOW_REAL_HOST_OBSERVATIONS=78",
                      completed.stdout)
        self.assertIn("WINDOW_SWEEP_DEFAULT_20S_50P exact=4/4",
                      completed.stdout)
        self.assertIn("safe_ambiguous=2/2 false_accept=0",
                      completed.stdout)
        self.assertIn("WINDOW_SWEEP_EXPLORATORY_ONLY=TRUE",
                      completed.stdout)

    def test_incomplete_duplicate_and_falsified_provenance_rejected(self):
        original = self._rows()
        self.assertNotEqual(self._invoke(original[:-1]).returncode, 0)
        self.assertNotEqual(self._invoke(original + original[:1]).returncode, 0)
        invalid = self._rows()
        invalid[0]["effective_seconds"] = "900"
        self.assertNotEqual(self._invoke(invalid).returncode, 0)
        mislabelled = self._rows()
        mislabelled[0]["reference_bpm"] = "142"
        self.assertNotEqual(self._invoke(mislabelled).returncode, 0)


if __name__ == "__main__":
    unittest.main()
