"""Positive and adversarial synthetic-only MIR supply/selection recall tests."""
import copy
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from audit_smart_tempo_b_candidate_recall import analyze, RecallError


def event(stage, bpm, outcome="observed"):
    return dict(stage=stage, outcome=outcome, metrics={"bpm": bpm})


def reference(case="pulse_128"):
    expected = {"pulse_128": (128.0, "regular"),
                "pulse_60": (60.0, "regular"),
                "pulse_120_sparse": (120.0, "sparse"),
                "silence": (None, "silence")}
    bpm, style = expected[case]
    return {"schema": "synthetic_bpm_reference_v1", "cases": [{
        "id": case, "reference_bpm": bpm, "style": style,
        "duration_sec": 20, "sample_rate": 16000,
        "event_sample_positions": [], "wav_sha256": "a"*64}]}


def pipeline(raw=(128.01, 143.), cluster=(128.01, 143.),
             policy_board=(128.01, 143.), primary=143.,
             policy=143., final=143., held=False):
    result = []
    for stage, items in (("candidate_create", raw),
                         ("candidate_transform", cluster),
                         ("policy_eligibility", policy_board)):
        result.extend(event(stage, value) for value in items)
    result.extend((event("primary_select", primary),
                   event("policy_change", policy),
                   event("final_output", final, "review_hold" if held else "selected")))
    return result


class RecallTests(unittest.TestCase):
    def test_candidate_generated_but_primary_selected_other(self):
        found = analyze(pipeline(), reference(), [{"index": "0", "bpm": "143", "held": "0"}],
                        "pulse_128")
        self.assertEqual(found["ROOT_CAUSE_STAGE"], "primary_selected_other")
        self.assertEqual((found["RAW_NEAR_020"], found["CLUSTER_NEAR_020"],
                          found["POLICY_BOARD_NEAR_020"]), (1, 1, 1))
        self.assertEqual(found["FINAL_ACCEPTED_NEAR_020"], 0)

    def test_stage_partition_and_exact_accept(self):
        cases = (
            (dict(raw=(143.,), cluster=(143.,), policy_board=(143.,)),
             "missing_raw_reference"),
            (dict(cluster=(143.,), policy_board=(143.,)),
             "missing_cluster_reference"),
            (dict(policy_board=(143.,)), "missing_policy_board_reference"),
            (dict(primary=143.), "primary_selected_other"),
            (dict(primary=128., policy=143.), "policy_selected_other"),
            (dict(primary=128., policy=128., final=143.), "final_selected_other"),
            (dict(primary=128., policy=128., final=128.01),
             "reference_in_selected_output"),
        )
        for kw, expected in cases:
            with self.subTest(expected=expected):
                actual = pipeline(**kw)
                final = actual[-1]["metrics"]["bpm"]
                r = analyze(actual, reference(),
                            [{"index": "0", "bpm": str(final), "held": "0"}], "pulse_128")
                self.assertEqual(r["ROOT_CAUSE_STAGE"], expected)

    def test_valid_negative_requires_no_native_candidates(self):
        r = analyze(pipeline(raw=(), cluster=(), policy_board=(),
                             primary=0., policy=0., final=0., held=True),
                    reference("silence"),
                    [{"index": "0", "bpm": "0", "held": "1"}], "silence")
        self.assertEqual(r["ROOT_CAUSE_STAGE"], "negative_abstained")
        self.assertEqual(r["RAW_TOTAL"], 0)
        with self.assertRaises(RecallError):
            analyze(pipeline(raw=(120.,), cluster=(), policy_board=(),
                             primary=0., policy=0., final=0., held=True),
                    reference("silence"),
                    [{"index": "0", "bpm": "0", "held": "1"}], "silence")

    def test_adversarial_manifest_host_identity_and_observations(self):
        good = pipeline(primary=128., policy=128., final=128.)
        base = reference()
        obs = [{"index": "0", "bpm": "128", "held": "0"}]
        for corruption in ("wrong_case", "modified_ref", "foreign_style",
                           "duplicate_terminal", "wrong_final",
                           "held_vs_selected", "malformed_bpm",
                           "negative_bpm", "missing_observation",
                           "bad_hash", "bad_sample_rate", "private_manifest_field"):
            with self.subTest(corruption=corruption):
                rows = copy.deepcopy(good)
                manifest = copy.deepcopy(base)
                observed = copy.deepcopy(obs)
                case_id = "pulse_128"
                if corruption == "wrong_case": case_id = "pulse_60"
                if corruption == "modified_ref":
                    manifest["cases"][0]["reference_bpm"] = 130.
                if corruption == "foreign_style":
                    manifest["cases"][0]["style"] = "private_song"
                if corruption == "duplicate_terminal":
                    rows.append(copy.deepcopy(rows[-1]))
                if corruption == "wrong_final":
                    rows[-1]["metrics"]["bpm"] = 129.
                if corruption == "held_vs_selected": observed[0]["held"] = "1"
                if corruption == "malformed_bpm": observed[0]["bpm"] = "nan"
                if corruption == "negative_bpm": observed[0]["bpm"] = "-128"
                if corruption == "missing_observation": observed.clear()
                if corruption == "bad_hash":
                    manifest["cases"][0]["wav_sha256"] = "invalid"
                if corruption == "bad_sample_rate":
                    manifest["cases"][0]["sample_rate"] = 44100
                if corruption == "private_manifest_field":
                    manifest["cases"][0]["title"] = "private"
                with self.assertRaises(RecallError):
                    analyze(rows, manifest, observed, case_id)

    def test_runner_is_separated_from_native_product(self):
        root = Path(__file__).resolve().parents[1]
        host = (root / "tests/diagnostic_live_host_smoke.ps1").read_text(encoding="utf-8")
        source = (root / "src/foo_smart_tempo/modern_bpm_analyzer.cpp").read_text(encoding="utf-8")
        workflow = (root / ".github/workflows/source.yml").read_text(encoding="utf-8")
        self.assertIn("audit_smart_tempo_b_candidate_recall.py", host)
        self.assertIn("--manifest $reference --observed $observed", host)
        self.assertIn("--trace $trace --case $label.Substring(10)", host)
        self.assertIn("test_smart_tempo_b_candidate_recall.py", workflow)
        self.assertNotIn("audit_smart_tempo_b_candidate_recall", source)
        self.assertNotIn("reference_bpm", source)


if __name__ == "__main__":
    unittest.main()
