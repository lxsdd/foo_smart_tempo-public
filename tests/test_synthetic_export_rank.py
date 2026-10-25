"""Positive and fail-closed regressions for public exported-rank diagnostics."""
from __future__ import annotations

from pathlib import Path
import sys
import unittest

TOOLS = Path(__file__).resolve().parents[1] / "tools"
sys.path.insert(0, str(TOOLS))
from audit_synthetic_export_rank import audit_export_rank
from generate_synthetic_complex_challenge import CASES


def fixture():
    scored = {"results": []}
    signals = {}
    details = []
    keys = ("total", "clear", "risk", "clear_020", "clear_wrong",
            "clear_held", "risk_accepted", "risk_held")
    stages = {"primary": dict.fromkeys(keys, 0),
              "writer_override": dict.fromkeys(keys, 0)}
    risk_styles = {"halftrap", "doubletrap", "switch", "nonperiodic", "tone"}
    clear = [c for c in CASES if c[2] not in risk_styles]
    assert len(CASES) == 16 and len(clear) == 11
    for name, bpm, style in CASES:
        risk = style in risk_styles
        index = clear.index((name, bpm, style)) if not risk else -1
        wrong = not risk and index in (0, 1, 2)
        held = (not risk and index == 3) or (risk and style == "tone")
        stage = "writer_override" if not risk and index == 1 else "primary"
        actual = 0. if held else (bpm + 9. if wrong else bpm if not risk else 111.)
        scored["results"].append({"id": name, "reported_bpm": actual, "held": held})
        outcome = ("risk_hold" if risk and held else "risk_accept" if risk else
                   "held" if held else "wrong" if wrong else "correct")
        details.append((name, stage, outcome, "na", "na"))
        s = stages[stage]
        s["total"] += 1
        s["risk" if risk else "clear"] += 1
        if risk:
            s["risk_held" if held else "risk_accepted"] += 1
        else:
            s["clear_held" if held else "clear_wrong" if wrong else "clear_020"] += 1
        if wrong and index == 0:
            signals[name] = [{"rank": "1", "bpm": str(bpm), "local_exact": "0.99"},
                             {"rank": "2", "bpm": str(actual), "local_exact": "0.50"}]
        elif wrong and index == 1:
            signals[name] = [{"rank": "1", "bpm": str(actual), "local_exact": "0.45"},
                             {"rank": "2", "bpm": str(bpm), "local_exact": "0.99"}]
        elif wrong and index == 2:
            signals[name] = [{"rank": "1", "bpm": str(actual), "local_exact": "0.99"}]
        else:
            signals[name] = [{"rank": "1", "bpm": str(actual if actual else bpm or 100),
                              "local_exact": "0.75"}]
    return scored, signals, {"details": details, "stages": stages}


class ExportRankTests(unittest.TestCase):
    def test_positive_full_stage_and_rank_denominators(self):
        scored, signals, stages = fixture()
        a = audit_export_rank(scored, signals, stages, CASES)
        t = a["totals"]
        self.assertEqual((t["total"], t["clear"], t["risk"], t["correct"],
                          t["wrong"], t["held"]), (16, 11, 5, 7, 3, 1))
        self.assertEqual((t["wrong_precise_rank1"], t["wrong_precise_nonrank1_only"],
                          t["wrong_no_precise"]), (1, 1, 1))
        self.assertEqual((t["wrong_output_rank1"], t["wrong_output_nonrank1_only"],
                          t["wrong_output_missing"]), (2, 1, 0))
        self.assertEqual((t["wrong_precise_top_local"],
                          t["wrong_precise_not_top_local"]), (2, 0))
        self.assertEqual(a["stages"]["writer_override"]["wrong_precise_nonrank1_only"], 1)
        self.assertEqual(t["risk_accept"], 4)
        self.assertEqual(a["interpretation"],
                         "exported_rank_only_not_primary_selector_provenance")

    def test_missing_and_duplicate_id_fail_closed(self):
        scored, signals, stages = fixture()
        del signals[CASES[0][0]]
        with self.assertRaises(ValueError):
            audit_export_rank(scored, signals, stages, CASES)
        scored, signals, stages = fixture()
        scored["results"][0]["id"] = CASES[1][0]
        with self.assertRaises(ValueError):
            audit_export_rank(scored, signals, stages, CASES)

    def test_negative_nan_and_unbounded_rank_or_local(self):
        for key, value in (("bpm", "nan"), ("bpm", "inf"), ("bpm", "-10"),
                           ("rank", "0"), ("rank", "257"), ("rank", "1.5"),
                           ("local_exact", "nan"), ("local_exact", "1.01"),
                           ("local_exact", "-0.01")):
            with self.subTest(key=key, value=value):
                scored, signals, stages = fixture()
                signals[CASES[0][0]][0][key] = value
                with self.assertRaises(ValueError):
                    audit_export_rank(scored, signals, stages, CASES)

    def test_negative_stage_drift_and_host_hold_contradiction(self):
        scored, signals, stages = fixture()
        stages["stages"]["writer_override"]["clear_wrong"] += 1
        with self.assertRaises(ValueError):
            audit_export_rank(scored, signals, stages, CASES)
        scored, signals, stages = fixture()
        scored["results"][0]["held"] = True
        with self.assertRaises(ValueError):
            audit_export_rank(scored, signals, stages, CASES)

    def test_negative_outcome_and_cap(self):
        scored, signals, stages = fixture()
        details = list(stages["details"])
        row = list(details[0])
        row[2] = "correct"
        details[0] = tuple(row)
        stages["details"] = details
        with self.assertRaises(ValueError):
            audit_export_rank(scored, signals, stages, CASES)
        scored, signals, stages = fixture()
        signals[CASES[0][0]] *= 17
        with self.assertRaises(ValueError):
            audit_export_rank(scored, signals, stages, CASES)

    def test_negative_nonfinite_reference_and_private_extra_case(self):
        scored, signals, stages = fixture()
        changed = list(CASES)
        changed[0] = (changed[0][0], float("nan"), changed[0][2])
        with self.assertRaises(ValueError):
            audit_export_rank(scored, signals, stages, tuple(changed))
        scored, signals, stages = fixture()
        signals["private_track"] = [{"rank": "1", "bpm": "120", "local_exact": "1"}]
        with self.assertRaises(ValueError):
            audit_export_rank(scored, signals, stages, CASES)

    def test_negative_top_local_absence_and_tie(self):
        scored, signals, stages = fixture()
        name = CASES[0][0]
        signals[name][0]["local_exact"] = "0.10"
        a = audit_export_rank(scored, signals, stages, CASES)
        self.assertEqual(a["totals"]["wrong_precise_not_top_local"], 1)
        signals[name][0]["local_exact"] = "0.50"
        a = audit_export_rank(scored, signals, stages, CASES)
        self.assertEqual(a["totals"]["wrong_precise_top_local"], 2)

    def test_cpp_source_exports_rank_not_primary_origin(self):
        source = (TOOLS.parent / "tests" / "ci_bpm_oracle_sink.h").read_text(
            encoding="utf-8")
        self.assertIn("candidate.evidence_rank", source)
        self.assertIn("candidate.local_exact_score", source)
        self.assertIn("rank,bpm,evidence,support,winner,local_exact", source)


if __name__ == "__main__":
    unittest.main()
