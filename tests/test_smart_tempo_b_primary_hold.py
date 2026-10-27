#!/usr/bin/env python3
"""Public-only positive/negative native Primary hold causal contracts."""
from __future__ import annotations
import copy
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tests"))
from audit_smart_tempo_b_primary_hold import PrimaryHoldError, reconcile
from audit_smart_tempo_ci_pipeline_trace import TraceError, validate
from test_smart_tempo_ci_pipeline_trace import (
    trace, with_resource, empty_board_native_trace)


class NativePrimaryHoldTests(unittest.TestCase):
    def fixture(self, *, flags=(), reason="measured_candidate_primary"):
        rows = with_resource(trace())
        p = next(r for r in rows if r["stage"] == "primary_select")
        h = next(r for r in rows if r["stage"] == "primary_hold")
        held = bool(flags)
        p["outcome"] = h["outcome"] = "review_hold" if held else "selected"
        p["reason_code"] = "primary_review_hold" if held else "primary_auto"
        h["reason_code"] = reason
        h["metrics"]["review_hold"] = int(held)
        h["metrics"]["auto_candidate"] = int(not held)
        for name in ("material_hold", "high_family_hold",
                     "soft_center_hold", "sparse_hold"):
            h["metrics"][name] = int(name in flags)
        return rows

    def test_all_priorities_and_simultaneous_causes(self):
        cases = [
            ((), "measured_candidate_primary"),
            (("sparse_hold",), "sparse_or_acapella_review_hold"),
            (("soft_center_hold", "sparse_hold"), "soft_center_conflict_review_hold"),
            (("high_family_hold", "soft_center_hold", "sparse_hold"),
             "high_family_conflict_review_hold"),
            (("material_hold", "high_family_hold", "soft_center_hold",
              "sparse_hold"), "material_risk_review_hold"),
        ]
        for flags, reason in cases:
            with self.subTest(flags=flags):
                proof = reconcile(self.fixture(flags=flags, reason=reason), required=True)
                self.assertEqual(proof["primary_hold_active"], int(bool(flags)))
                self.assertEqual(proof["primary_hold_cause_count"], len(flags))
                self.assertEqual(proof["primary_hold_multi_cause"], int(len(flags) > 1))

    def test_no_candidate_native_silence(self):
        rows = empty_board_native_trace()
        self.assertEqual(reconcile(rows, required=True)["primary_auto_eligible"], 0)
        self.assertEqual(validate(rows, require_windows=True)["raw"], 0)

    def test_full_host_shape_and_negative_mutations(self):
        rows = with_resource(trace())
        self.assertEqual(validate(rows, require_windows=True)["primary_hold_native_verified"], 1)
        for mutation in ("missing", "duplicate", "source", "outcome", "reason",
                         "flag", "route", "priority", "score", "sequence"):
            with self.subTest(mutation=mutation):
                bad = copy.deepcopy(rows)
                h = next(r for r in bad if r["stage"] == "primary_hold")
                if mutation == "missing":
                    bad.remove(h)
                elif mutation == "duplicate":
                    bad.insert(bad.index(h), copy.deepcopy(h))
                elif mutation == "source":
                    h["parent_candidate_ids"] = ["c0"]
                elif mutation == "outcome":
                    h["outcome"] = "review_hold"
                elif mutation == "reason":
                    h["reason_code"] = "sparse_or_acapella_review_hold"
                elif mutation == "flag":
                    h["metrics"]["sparse_hold"] = 1
                elif mutation == "route":
                    h["metrics"]["route_valid"] = 0
                elif mutation == "priority":
                    h["metrics"]["material_hold"] = 1
                    h["metrics"]["review_hold"] = 1
                    h["metrics"]["auto_candidate"] = 0
                    h["outcome"] = "review_hold"
                    next(r for r in bad if r["stage"] == "primary_select")["outcome"] = "review_hold"
                elif mutation == "score":
                    h["metrics"]["score"] = 0.11
                elif mutation == "sequence":
                    h["sequence"] = 999
                if mutation != "sequence":
                    for i, row in enumerate(bad):
                        row["sequence"] = i
                with self.assertRaises((TraceError, PrimaryHoldError)):
                    validate(bad, require_windows=True)

    def test_held_primary_cannot_be_native_primary_writer(self):
        rows = self.fixture(flags=("sparse_hold",),
                            reason="sparse_or_acapella_review_hold")
        rows.append({"stage": "writer_resolution", "reason_code": "mir_primary"})
        with self.assertRaises(PrimaryHoldError):
            reconcile(rows, required=True)

    def test_production_source_is_observer_only(self):
        source = (ROOT / "tests" / "ci_pipeline_trace.h").read_text(encoding="utf-8")
        cpp = (ROOT / "src" / "foo_smart_tempo" / "mir_candidate_pipeline.cpp").read_text(
            encoding="utf-8")
        self.assertIn('out.push("primary_hold"', source)
        self.assertIn("primary.material_risk_review_hold", source)
        self.assertIn("primary.soft_center_conflict_review_hold", source)
        self.assertIn("#ifndef SMART_TEMPO_CI_ORACLE_PROBE", source)
        self.assertNotIn('out.push("primary_hold"', cpp)


if __name__ == "__main__":
    unittest.main()
