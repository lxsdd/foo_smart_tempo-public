"""Public synthetic-only final-path attribution regression; no host writes."""
import csv
import math
import sys
import tempfile
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1] / "tools"
sys.path.insert(0, str(TOOLS))
from audit_synthetic_policy_stage import (  # noqa: E402
    load_policy, audit_policy_stages, source_measured_reason_allowlist,
    POLICY_FIELDS,
)
from generate_synthetic_complex_challenge import CASES  # noqa: E402

ALLOWED = frozenset(("none", "measured_half_pulse_recovery", "frozen_writer_override_gate"))


def fixtures():
    scored = {"results": []}
    signals, policies = {}, {}
    for name, bpm, style in CASES:
        risk = style in ("halftrap", "doubletrap", "switch", "nonperiodic", "tone")
        scored["results"].append({"id": name, "held": risk,
                                  "reported_bpm": 0.0 if risk else bpm})
        signals[name] = [{"bpm": str(bpm)}] if bpm is not None else []
        policies[name] = {"id": name, "policy_reason": "mir_review_hold" if risk else "mir_primary",
                          "decision_class": "mir-review-hold" if risk else "mir-primary",
                          "measured_policy_reason": "none"}
    return scored, signals, policies


class PolicyStageTests(unittest.TestCase):
    def write_csv(self, path, rows, fields=POLICY_FIELDS):
        with path.open("w", encoding="utf-8", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=fields)
            writer.writeheader()
            writer.writerows(rows)

    def test_public_source_allowlist_is_frozen_and_available(self):
        actual = source_measured_reason_allowlist()
        self.assertTrue(ALLOWED <= actual)
        self.assertNotIn("invalid", actual)

    def test_primary_and_review_hold_denominators(self):
        scored, signals, policies = fixtures()
        audit = audit_policy_stages(scored, signals, policies, CASES)
        self.assertEqual(audit["totals"]["clear_020"], 11)
        self.assertEqual(audit["totals"]["risk_held"], 5)
        self.assertEqual(audit["stages"]["primary"]["total"], 11)
        self.assertEqual(audit["stages"]["review_hold"]["total"], 5)

    def test_wrong_precise_candidate_and_half_double_alias(self):
        scored, signals, policies = fixtures()
        name, bpm, _ = CASES[0]
        scored["results"][0]["reported_bpm"] = bpm * 2
        signals[name].append({"bpm": str(bpm * 2)})
        policies[name].update(policy_reason="mir_writer_override", decision_class="mir-writer-override",
                              measured_policy_reason="measured_half_pulse_recovery")
        audit = audit_policy_stages(scored, signals, policies, CASES)
        self.assertEqual(audit["totals"]["clear_wrong"], 1)
        self.assertEqual(audit["totals"]["clear_alias"], 1)
        self.assertEqual(audit["stages"]["writer_override"]["wrong_precise_export"], 1)
        self.assertEqual(audit["stages"]["writer_override"]["wrong_output_matches_export"], 1)

    def test_wrong_without_precise_candidate_or_output_match(self):
        scored, signals, policies = fixtures()
        name, bpm, _ = CASES[0]
        scored["results"][0]["reported_bpm"] = bpm * 1.7
        signals[name] = [{"bpm": str(bpm * 1.3)}]
        audit = audit_policy_stages(scored, signals, policies, CASES)
        self.assertEqual(audit["totals"]["wrong_missing_precise_export"], 1)
        self.assertEqual(audit["totals"]["wrong_output_missing_export"], 1)

    def test_release_is_final_path_not_unique_internal_root_cause(self):
        scored, signals, policies = fixtures()
        name = CASES[4][0]  # public halftrap risk control
        scored["results"][4].update(held=False, reported_bpm=94.8)
        policies[name].update(policy_reason="mir_review_hold_release",
                              decision_class="mir-review-hold-release",
                              measured_policy_reason="frozen_writer_override_gate")
        audit = audit_policy_stages(scored, signals, policies, CASES)
        self.assertEqual(audit["stages"]["review_hold_release"]["risk_accepted"], 1)

    def test_unknown_path_is_not_guessed(self):
        scored, signals, policies = fixtures()
        policies[CASES[0][0]].update(policy_reason="none", decision_class="none")
        audit = audit_policy_stages(scored, signals, policies, CASES)
        self.assertEqual(audit["stages"]["unknown"]["clear_020"], 1)

    def test_accepted_hold_or_held_writer_rejected(self):
        scored, signals, policies = fixtures()
        scored["results"][4].update(held=False, reported_bpm=94.8)
        with self.assertRaises(ValueError):
            audit_policy_stages(scored, signals, policies, CASES)
        scored, signals, policies = fixtures()
        policies[CASES[4][0]].update(policy_reason="mir_writer_override",
                                    decision_class="mir-writer-override")
        with self.assertRaises(ValueError):
            audit_policy_stages(scored, signals, policies, CASES)

    def test_identity_and_nan_rejected(self):
        scored, signals, policies = fixtures()
        scored["results"][0]["id"] = "challenge_private"
        with self.assertRaises(ValueError):
            audit_policy_stages(scored, signals, policies, CASES)
        scored, signals, policies = fixtures()
        scored["results"][0]["reported_bpm"] = math.nan
        with self.assertRaises(ValueError):
            audit_policy_stages(scored, signals, policies, CASES)

    def test_csv_complete_ordered_and_exact_header(self):
        _, _, policies = fixtures()
        ids = [name for name, _, _ in CASES]
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "complex-policy.csv"
            self.write_csv(path, policies.values())
            self.assertEqual(len(load_policy(path, ids, ALLOWED)), 16)
            self.write_csv(path, list(policies.values())[:-1])
            with self.assertRaises(ValueError):
                load_policy(path, ids, ALLOWED)
            self.write_csv(path, policies.values(),
                           fields=("id", "decision_class", "policy_reason", "measured_policy_reason"))
            with self.assertRaises(ValueError):
                load_policy(path, ids, ALLOWED)

    def test_private_path_injection_and_unknown_reason_rejected(self):
        _, _, policies = fixtures()
        ids = [name for name, _, _ in CASES]
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "complex-policy.csv"
            for key, value in (("id", "challenge_private"),
                               ("policy_reason", "C:\\Users\\private"),
                               ("decision_class", "=HYPERLINK"),
                               ("measured_policy_reason", "unknown_private_token"),
                               ("measured_policy_reason", "invalid")):
                rows = [dict(row) for row in policies.values()]
                rows[0][key] = value
                self.write_csv(path, rows)
                with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                    load_policy(path, ids, ALLOWED)

    def test_mismatched_final_class_rejected(self):
        _, _, policies = fixtures()
        ids = [name for name, _, _ in CASES]
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "complex-policy.csv"
            rows = [dict(row) for row in policies.values()]
            rows[0]["policy_reason"] = "mir_review_hold_release"
            self.write_csv(path, rows)
            with self.assertRaises(ValueError):
                load_policy(path, ids, ALLOWED)

    def test_witness_hold_not_conflated_with_missing_candidate(self):
        scored, signals, policies = fixtures()
        policies[CASES[4][0]].update(policy_reason="mir_writer_invariant_hold",
                                    decision_class="mir-review-hold")
        audit = audit_policy_stages(scored, signals, policies, CASES)
        self.assertEqual(audit["stages"]["witness_hold"]["risk_held"], 1)
        self.assertEqual(audit["stages"]["no_candidate"]["total"], 0)


if __name__ == "__main__":
    unittest.main()
