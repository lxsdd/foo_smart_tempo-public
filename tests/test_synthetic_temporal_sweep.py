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

    def _candidates(self):
        out = []
        for name, (early, late, _, _) in MOD["PHASE_REFERENCES"].items():
            for phase, expected in (("early", early), ("late", late)):
                if expected is not None:
                    out.append({
                        "id": name, "phase": phase, "rank": 1,
                        "bpm": expected, "evidence": 1.75, "support": 1.0,
                        "winner": 0.75, "local_exact": .94,
                    })
                else:
                    # A background nonperiodic proposal is permitted in the
                    # inventory, but it must never gain a reference BPM.
                    out.append({
                        "id": name, "phase": phase, "rank": 1,
                        "bpm": 83.0, "evidence": .4, "support": .3,
                        "winner": .2, "local_exact": .12,
                    })
        return out

    def _run(self, rows, candidates=None):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "temporal.csv"
            with path.open("w", encoding="utf-8", newline="") as f:
                writer = csv.DictWriter(f, fieldnames=MOD["FIELDS"])
                writer.writeheader()
                writer.writerows(rows)
            candidate_path = Path(temp) / "candidates.csv"
            with candidate_path.open("w", encoding="utf-8", newline="") as f:
                writer = csv.DictWriter(f, fieldnames=MOD["CANDIDATE_FIELDS"])
                writer.writeheader()
                writer.writerows(candidates if candidates is not None
                                 else self._candidates())
            return subprocess.run(
                [sys.executable, str(SCORER), "--observed", str(path),
                 "--candidates", str(candidate_path)],
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
        self.assertIn("TEMPORAL_PHASE_CORRECT_CANDIDATE_PRESENT=10/10",
                      completed.stdout)
        self.assertIn("TEMPORAL_CANDIDATE_EVIDENCE_DIAGNOSTIC_ONLY=TRUE",
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

    def test_candidate_inventory_validation_and_missing_reference(self):
        rows = self._rows()
        candidates = self._candidates()
        # A missing exact phase candidate is diagnostic, never an analyzer
        # failure or a reason to use synthetic truth as a selection hint.
        no_exact = [c for c in candidates
                    if not (c["id"] == "window_90_128_switch" and
                            c["phase"] == "late")]
        completed = self._run(rows, no_exact)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertIn("TEMPORAL_PHASE_CORRECT_CANDIDATE_PRESENT=9/10",
                      completed.stdout)

        bad = self._candidates()
        bad[0]["id"] = "owner_private_track"
        self.assertNotEqual(self._run(rows, bad).returncode, 0)
        bad = self._candidates()
        bad[0]["rank"] = 33
        self.assertNotEqual(self._run(rows, bad).returncode, 0)
        bad = self._candidates()
        bad[0]["local_exact"] = float("nan")
        self.assertNotEqual(self._run(rows, bad).returncode, 0)
        seed = self._candidates()[0]
        bad = [{**seed, "rank": i % 32 + 1,
                "bpm": float(seed["bpm"]) + i / 100}
               for i in range(33)]
        self.assertNotEqual(self._run(rows, bad).returncode, 0)

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
