"""Strict synthetic-only temporal-phase observation scorer regression tests."""
from __future__ import annotations

import csv
from pathlib import Path
import runpy
import subprocess
import sys
import tempfile
import unittest

SCORER = Path(__file__).resolve().parents[1] / "tools/score_synthetic_temporal_sweep.py"
MOD = runpy.run_path(str(SCORER))


class TemporalSweepScorerTests(unittest.TestCase):
    def _rows(self):
        out = []
        for name, (early, late, style, ambiguity) in MOD["PHASE_REFERENCES"].items():
            global_ref = early if early == late else None
            for phase, bpm, offset in (("early", early, 20), ("late", late, 80)):
                out.append({
                    "id": name, "style": style,
                    "reference_bpm": "" if global_ref is None else str(global_ref),
                    "ambiguity": ambiguity,
                    "phase": phase, "offset_pct": offset,
                    "seconds": 20, "passes": 1,
                    "effective_seconds": 20, "effective_passes": 1,
                    "bpm": 0 if bpm is None else bpm,
                    "held": 1 if bpm is None else 0, "analysis_ms": 712,
                })
        return out

    def _run(self, rows):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "temporal.csv"
            with path.open("w", encoding="utf-8", newline="") as f:
                writer = csv.DictWriter(f, fieldnames=MOD["FIELDS"])
                writer.writeheader()
                writer.writerows(rows)
            return subprocess.run(
                [sys.executable, str(SCORER), "--observed", str(path)],
                capture_output=True, text=True,
            )

    def test_complete_early_late_public_matrix(self):
        rows = self._rows()
        self.assertEqual(len(rows), 12)
        completed = self._run(rows)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertIn("TEMPORAL_REAL_HOST_OBSERVATIONS=12", completed.stdout)
        self.assertIn("id=window_90_128_switch phase=early expected=90.000",
                      completed.stdout)
        self.assertIn("id=window_90_128_switch phase=late expected=128.000",
                      completed.stdout)
        self.assertIn("TEMPORAL_STATIONARITY_DIAGNOSTIC_ONLY=TRUE",
                      completed.stdout)

    def test_wrong_phase_duplicate_and_forged_reference_fail_closed(self):
        rows = self._rows()
        self.assertNotEqual(self._run(rows[:-1]).returncode, 0)
        self.assertNotEqual(self._run(rows + rows[:1]).returncode, 0)
        rows = self._rows()
        rows[0]["offset_pct"] = 80
        self.assertNotEqual(self._run(rows).returncode, 0)
        rows = self._rows()
        rows[0]["reference_bpm"] = 137
        self.assertNotEqual(self._run(rows).returncode, 0)

    def test_status_abstention_and_wrong_alias(self):
        status = MOD["status"]
        self.assertEqual(status({"held": 0, "bpm": 128.05, "expected": 128.0}),
                         "exact")
        self.assertEqual(status({"held": 0, "bpm": 64, "expected": 128}),
                         "wrong_or_alias")
        self.assertEqual(status({"held": 1, "bpm": 0, "expected": 128}),
                         "held")
        self.assertEqual(status({"held": 1, "bpm": 0, "expected": None}),
                         "safe_hold")
        self.assertEqual(status({"held": 0, "bpm": 66, "expected": None}),
                         "unsafe_accept")


if __name__ == "__main__":
    unittest.main()
