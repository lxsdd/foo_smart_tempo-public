"""Independent public-only positive and negative final-stage/shadow join tests."""
import math
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from audit_synthetic_stage_shadow import audit_stage_shadow, RISK

CASES = (
    ("challenge_73_25_cross32", 73.25, "cross32"),
    ("challenge_97_25_cross54", 97.25, "cross54"),
    ("challenge_129_37_clave", 129.37, "clave"),
    ("challenge_161_80_odd7", 161.80, "odd7"),
    ("challenge_189_60_halftrap", 189.60, "halftrap"),
    ("challenge_67_40_doubletrap", 67.40, "doubletrap"),
    ("challenge_143_60_ghostswing", 143.60, "ghostswing"),
    ("challenge_118_75_phasehats", 118.75, "phasehats"),
    ("challenge_103_30_sparsekick", 103.30, "sparsekick"),
    ("challenge_151_25_humanized", 151.25, "humanized"),
    ("challenge_84_60_masked", 84.60, "masked"),
    ("challenge_212_50_fast", 212.50, "fast"),
    ("challenge_57_35_slow", 57.35, "slow"),
    ("challenge_108_142_switch", None, "switch"),
    ("challenge_nonperiodic", None, "nonperiodic"),
    ("challenge_steady_tone", None, "tone"),
)
PATHS = ("primary", "writer_override", "writer_override", "primary",
         "primary", "primary", "primary", "primary", "primary", "primary",
         "primary", "primary", "writer_override", "writer_override",
         "review_hold_release", "review_hold")
WRONG = frozenset((0, 1, 2, 6, 7, 8, 11, 12))
SHADOW_HOLDS = frozenset((0, 4, 5, 7, 10, 12, 13, 14, 15))


def fixture():
    results, details, frozen, signals = [], [], [], {}
    stages = {p: dict.fromkeys(("total", "clear", "risk", "clear_020",
                                "clear_wrong", "clear_held", "risk_accepted",
                                "risk_held"), 0) for p in set(PATHS)}
    for i, (name, bpm, style) in enumerate(CASES):
        risk = style in RISK
        held = i == 15
        observed = (bpm + 10 if i in WRONG else bpm) if not risk else (0 if held else 100)
        proposal = None if i in SHADOW_HOLDS else bpm
        results.append({"id": name, "held": held, "reported_bpm": observed})
        outcome = ("risk_hold" if held else "risk_accept") if risk else (
            "wrong" if i in WRONG else "correct")
        details.append((name, PATHS[i], outcome, "na", "na"))
        frozen.append((name, proposal))
        signals[name] = [] if proposal is None else [{"bpm": str(proposal)}]
        stage = stages[PATHS[i]]
        stage["total"] += 1
        stage["risk" if risk else "clear"] += 1
        stage[("risk_held" if held else "risk_accepted") if risk else
              ("clear_wrong" if i in WRONG else "clear_020")] += 1
    return ({"results": results}, {"details": details, "stages": stages},
            {"cases": frozen, "counts": {"clear_correct": 7,
             "clear_wrong": 0, "clear_held": 4, "metrical_accepted": 0,
             "nonstationary_accepted": 0, "nonrhythmic_accepted": 0}}, signals)


class StageShadowTests(unittest.TestCase):
    def test_complete_16_case_cross_tab_and_stage_transitions(self):
        scored, stages, frozen, signals = fixture()
        audit = audit_stage_shadow(scored, stages, frozen, CASES, signals)
        total = audit["totals"]
        self.assertEqual((total["clear"], total["risk"]), (11, 5))
        self.assertEqual((total["prod_correct"], total["prod_wrong"], total["prod_held"]),
                         (3, 8, 0))
        self.assertEqual((total["shadow_correct"], total["shadow_wrong"], total["shadow_held"]),
                         (7, 0, 4))
        self.assertEqual((total["wrong_to_correct"], total["wrong_to_held"]), (5, 3))
        self.assertEqual((total["correct_to_correct"], total["correct_to_held"]), (2, 1))
        self.assertEqual((total["risk_accept_to_hold"], total["risk_hold_to_hold"]), (4, 1))
        self.assertEqual((audit["stages"]["primary"]["wrong_to_correct"],
                          audit["stages"]["writer_override"]["wrong_to_correct"]), (3, 2))
        self.assertEqual(audit["stages"]["review_hold_release"]["risk_accept_to_hold"], 1)

    def test_negative_shadow_wrong_is_counted_not_silently_corrected(self):
        scored, stages, frozen, signals = fixture()
        name = CASES[1][0]
        frozen["cases"][1] = (name, 150.0)
        signals[name] = [{"bpm": "150.0"}]
        frozen["counts"]["clear_correct"] -= 1
        frozen["counts"]["clear_wrong"] += 1
        audit = audit_stage_shadow(scored, stages, frozen, CASES, signals)
        self.assertEqual(audit["stages"]["writer_override"]["wrong_to_wrong"], 1)

    def test_shadow_must_be_measured_not_oracle_invented(self):
        scored, stages, frozen, signals = fixture()
        signals[CASES[1][0]] = [{"bpm": "190.0"}]
        with self.assertRaisesRegex(ValueError, "unmeasured"):
            audit_stage_shadow(scored, stages, frozen, CASES, signals)

    def test_reject_private_or_misordered_identity(self):
        scored, stages, frozen, signals = fixture()
        frozen["cases"][0] = ("private_song", 73.25)
        with self.assertRaisesRegex(ValueError, "identity"):
            audit_stage_shadow(scored, stages, frozen, CASES, signals)
        scored, stages, frozen, signals = fixture()
        stages["details"][0] = (CASES[1][0], "primary", "wrong", "na", "na")
        with self.assertRaisesRegex(ValueError, "identity"):
            audit_stage_shadow(scored, stages, frozen, CASES, signals)

    def test_nan_infinite_and_negative_shadow_rejected(self):
        for invalid in (math.nan, math.inf, -1.0, 0.0):
            scored, stages, frozen, signals = fixture()
            frozen["cases"][1] = (CASES[1][0], invalid)
            with self.subTest(invalid=invalid), self.assertRaises(ValueError):
                audit_stage_shadow(scored, stages, frozen, CASES, signals)

    def test_reject_wrong_final_stage_and_host_observation(self):
        scored, stages, frozen, signals = fixture()
        stages["details"][0] = (CASES[0][0], "primary", "correct", "na", "na")
        with self.assertRaisesRegex(ValueError, "outcome"):
            audit_stage_shadow(scored, stages, frozen, CASES, signals)
        scored, stages, frozen, signals = fixture()
        scored["results"][0]["reported_bpm"] = math.nan
        with self.assertRaisesRegex(ValueError, "host BPM"):
            audit_stage_shadow(scored, stages, frozen, CASES, signals)

    def test_drifted_stage_or_shadow_denominator_rejected(self):
        scored, stages, frozen, signals = fixture()
        stages["stages"]["primary"]["clear_wrong"] -= 1
        with self.assertRaisesRegex(ValueError, "denominator"):
            audit_stage_shadow(scored, stages, frozen, CASES, signals)
        scored, stages, frozen, signals = fixture()
        frozen["counts"]["clear_correct"] += 1
        with self.assertRaisesRegex(ValueError, "denominator"):
            audit_stage_shadow(scored, stages, frozen, CASES, signals)

    def test_risk_accept_shadow_accept_explicit_false_accept(self):
        scored, stages, frozen, signals = fixture()
        frozen["cases"][4] = (CASES[4][0], 94.8)
        signals[CASES[4][0]] = [{"bpm": "94.8"}]
        frozen["counts"]["metrical_accepted"] = 1
        audit = audit_stage_shadow(scored, stages, frozen, CASES, signals)
        self.assertEqual(audit["stages"]["primary"]["risk_accept_to_accept"], 1)
        self.assertEqual(audit["totals"]["risk_shadow_accepted"], 1)


if __name__ == "__main__":
    unittest.main()
