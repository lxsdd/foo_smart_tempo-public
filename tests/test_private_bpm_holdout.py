"""No-private-data regressions for the offline Smart Tempo BPM holdout scorer."""
from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / "tools/score-private-bpm-holdout.py"
SPEC = importlib.util.spec_from_file_location("private_holdout_scoring", SCRIPT)
SCORE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SCORE)


def fake_track(label, state, bpm, candidates=()):
    return SimpleNamespace(label=label, final={"state": state, "bpm": bpm},
                           candidates=list(candidates))


def fake_candidate(bpm, local_score):
    # MW1 policy candidate board: indices 2 and 3 are local_exact values.
    return ("fullboard", str(bpm), str(bpm), str(local_score))


class PrivateBpmHoldoutTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)

    def population(self):
        return {
            ("11", 0): fake_track("Private Synthetic A", "WRITE", 120.02,
                                  [fake_candidate(120, 0.9)]),
            ("22", 0): fake_track("Private Synthetic B", "WRITE", 90,
                                  [fake_candidate(45, 0.95)]),
            ("33", 1): fake_track("Private Synthetic C", "HOLD", None),
            ("44", 0): fake_track("Private Synthetic D", "WRITE", 133.5,
                                  [fake_candidate(130, 0.1)]),
        }

    def fill(self, template):
        by_key = {c["track_key"]: c for c in template["cases"]}
        by_key["11"].update(reference_class="clear", reference_bpm=120.0)
        by_key["22"].update(reference_class="ambiguous", reference_bpm=None)
        by_key["33"].update(reference_class="no_tempo", reference_bpm=None)
        by_key["44"].update(reference_class="no_tempo", reference_bpm=None)
        return template

    def test_aggregate_and_shadow_never_expose_identifiers_or_labels(self):
        tracks = self.population()
        template = self.fill(SCORE.make_template(tracks))
        path = self.root / "reference.json"
        path.write_text(json.dumps(template), encoding="utf-8")
        references = SCORE.read_references(path, tracks)
        result = SCORE.evaluate_holdout(tracks, references)
        self.assertEqual(result["total"], 4)
        self.assertEqual(result["clear_within_0_05_n"], 1)
        self.assertEqual(result["clear_within_0_20_n"], 1)
        self.assertEqual(result["shadow_clear_within_0_20_n"], 1)
        self.assertEqual(result["ambiguous_accepted_n"], 1)
        self.assertEqual(result["no_tempo_false_accept_n"], 1)
        self.assertEqual(result["unsafe_accepted_n"], 2)
        self.assertEqual(result["quality_verdict"], "NO_GO")
        out = json.dumps(result)
        for forbidden in ("Private Synthetic", '"11"', '"22"', '"33"', '"44"',
                          "120.02", "133.5"):
            self.assertNotIn(forbidden, out)

    def test_partial_duplicate_and_inconsistent_references_fail(self):
        tracks = self.population()
        template = self.fill(SCORE.make_template(tracks))
        reference = self.root / "reference.json"
        for change in ("missing", "duplicate", "label", "nan", "unknown"):
            trial = json.loads(json.dumps(template))
            if change == "missing":
                trial["cases"].pop()
            elif change == "duplicate":
                trial["cases"].append(dict(trial["cases"][0]))
            elif change == "label":
                trial["cases"][0]["label"] = "DIFFERENT"
            elif change == "nan":
                trial["cases"][0]["reference_bpm"] = float("nan")
            elif change == "unknown":
                trial["cases"][0]["reference_class"] = "inferred"
            reference.write_text(json.dumps(trial), encoding="utf-8")
            with self.subTest(change=change), self.assertRaises(
                    SCORE.HoldoutError):
                SCORE.read_references(reference, tracks)

    def test_invalid_candidates_and_holds_fail_closed(self):
        tracks = {("11", 0): fake_track("Secret A", "HOLD", None,
                                      [fake_candidate(120, float("inf"))])}
        refs = {("11", 0): {"class": "ambiguous", "bpm": None}}
        with self.assertRaises(SCORE.HoldoutError):
            SCORE.evaluate_holdout(tracks, refs)
        tracks[("11", 0)] = fake_track("Secret A", "HOLD", 120)
        with self.assertRaises(SCORE.HoldoutError):
            SCORE.evaluate_holdout(tracks, refs)

    def test_prepare_rejects_git_repository_and_existing_file(self):
        outside = self.root / "safe.json"
        SCORE.assert_private_destination(outside)
        outside.write_text("existing", encoding="utf-8")
        with self.assertRaises(SCORE.HoldoutError):
            SCORE.assert_private_destination(outside)
        repo = self.root / "repo"
        repo.mkdir()
        (repo / ".git").mkdir()
        with self.assertRaises(SCORE.HoldoutError):
            SCORE.assert_private_destination(repo / "private.json")

    def test_parser_reuses_strict_wm1_complete_telemetry(self):
        # Synthetic-only no-write trace resembling the actual host console.
        pre = "foo_smart_tempo: [Synthetic Offline]"
        lines = [
            f"{pre} MirAnalysisProvenance enabled=1 track_key=91 "
            "subsong=0 build_id=synthetic policy_schema=mir_policy_test_v1",
            f"{pre} MirSamplingPlan schema=mir_sampling_plan_v1 track_key=91 "
            "subsong=0 requested_window_seconds=20 requested_passes=50 "
            "effective_window_seconds=20 effective_passes=50 known_track_length=1 "
            "track_length_seconds=240 offset_min_pct=20 offset_max_pct=80",
            f"{pre} MirPolicyDecision enabled=1 source_candidate=0 "
            "output_would_write=0 output_would_review_hold=1 output_bpm=0",
            f"{pre} Decision: raw_global=n/a, raw_projected=n/a, "
            "final=0.00, decision_class=hold",
            f"{pre} Track Timing: decode_ms=100, onset_ms=200, total_ms=500",
            f"{pre} Final Output: no writable BPM (existing BPM tag left unchanged), "
            "confidence=0, uncertain=1, decision_class=hold",
        ]
        log = self.root / "synthetic.log"
        log.write_text("\n".join(lines) + "\n", encoding="utf-8")
        parsed = SCORE.parse_current_verbose_log(log)
        self.assertEqual(len(parsed), 1)
        self.assertEqual(parsed[("91", 0)].final["state"], "HOLD")
        template = SCORE.make_template(parsed)
        self.assertEqual(template["cases"][0]["label"], "Synthetic Offline")
        template["cases"][0].update(reference_class="no_tempo")
        refs = self.root / "refs.json"
        refs.write_text(json.dumps(template), encoding="utf-8")
        outcome = SCORE.evaluate_holdout(parsed, SCORE.read_references(refs, parsed))
        self.assertEqual(outcome["no_tempo_false_accept_n"], 0)

    def test_cli_fails_without_echoing_path_or_private_labels(self):
        secret_path = self.root / "PRIVATE_PERSONAL_FILE_DO_NOT_PRINT.log"
        secret_path.write_text("invalid trace", encoding="utf-8")
        result = subprocess.run(
            [sys.executable, str(SCRIPT), "--log", str(secret_path),
             "--prepare", str(self.root / "result.json")],
            text=True, capture_output=True,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("PRIVATE_BPM_AUDIT=FAIL", result.stderr)
        self.assertNotIn("PRIVATE_PERSONAL", result.stdout + result.stderr)
        self.assertFalse((self.root / "result.json").exists())


if __name__ == "__main__":
    unittest.main()
