"""Frozen synthetic 3/4 versus 6/8 beat-unit controls; no real host claims."""
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest
import wave

TOOL_PATH = Path(__file__).resolve().parents[1] / "tools"
sys.path.insert(0, str(TOOL_PATH))
from generate_synthetic_meter_challenge import CASES, RATE, synth
from score_synthetic_meter_challenge import score

# Literal versioned SHA values independently reproduced by a separate RIFF builder.
EXPECTED = {
    "meter_3_4_120_quarter": "ae25cd1d96941061aba667353a30357002742c02a2c8e78e5b9fd7736fd2620f",
    "meter_6_8_80_dotted": "7a09111a667e307f5a31b81be873af7f0791c9b4efb32f508df6a30c35078fd0",
    "meter_3_4_150_quarter": "3101fd765e20c3f034c6c417897afa0841d90bf3a1458d21bcba781c910e4b42",
    "meter_6_8_100_dotted": "6af55990769044a40396a28343fcc3130eaefd3f0bdda0970f2322395b7b36a0",
    "meter_ambiguous_120q_80dq": "2a092747fa9073d089e66238403ed1389b0257d58a3d161a92f98544c5934bfc",
}


def observe(values):
    return [dict(id=c[0], bpm=(str(v) if v is not None else "0"), held=v is None)
            for c, v in zip(CASES, values)]


class MeterChallengeTests(unittest.TestCase):
    def synthesize(self, path):
        cases = [synth(spec, path / (spec[0] + ".wav")) for spec in CASES]
        return dict(schema="synthetic_meter_challenge_v2", cases=cases)

    def test_frozen_wav_sha_and_declared_beat_units(self):
        with tempfile.TemporaryDirectory() as t:
            root = Path(t)
            manifest = self.synthesize(root)
            for spec, record in zip(CASES, manifest["cases"]):
                name, bpm, meter, unit, _ = spec
                self.assertEqual(record["wav_sha256"], EXPECTED[name])
                self.assertEqual(hashlib.sha256((root / (name + ".wav")).read_bytes()).hexdigest(), EXPECTED[name])
                with wave.open(str(root / (name + ".wav")), "rb") as w:
                    self.assertEqual((w.getnframes(), w.getframerate(), w.getnchannels(), w.getsampwidth()),
                                     (20 * RATE, RATE, 1, 2))
                self.assertEqual(record["reference_bpm"], bpm)
                self.assertEqual((record["meter"], record["beat_unit"]), (meter, unit))
                self.assertEqual(len(record["subdivision_sample_positions"]), 99 if bpm in (150, 100) else 79)
            self.assertEqual([len(x["beat_sample_positions"]) for x in manifest["cases"]], [40, 27, 50, 33, 0])
            self.assertEqual([len(x["bar_sample_positions"]) for x in manifest["cases"]], [14, 14, 17, 17, 14])

    def test_same_eighth_grid_different_beat_unit(self):
        with tempfile.TemporaryDirectory() as t:
            root = Path(t)
            rows = self.synthesize(root)["cases"]
            for a, b in ((0, 1), (2, 3)):
                self.assertEqual(rows[a]["subdivision_sample_positions"], rows[b]["subdivision_sample_positions"])
                self.assertEqual(rows[a]["bar_sample_positions"], rows[b]["bar_sample_positions"])
                self.assertNotEqual(rows[a]["beat_sample_positions"], rows[b]["beat_sample_positions"])
                self.assertNotEqual((root / (CASES[a][0] + ".wav")).read_bytes(),
                                    (root / (CASES[b][0] + ".wav")).read_bytes())
            self.assertEqual(rows[4]["beat_sample_positions"], [])

    def test_correct_and_conservative_abstention(self):
        with tempfile.TemporaryDirectory() as t:
            ref = self.synthesize(Path(t))
            m = score(ref, observe([120, 80, 150, 100, None]))
            self.assertEqual((m["counts"]["clear"], m["counts"]["correct_005"],
                              m["counts"]["wrong"], m["counts"]["ambiguous_held"]), (4, 4, 0, 1))
            self.assertEqual(m["verdict"], "RESEARCH_ONLY_NOT_MUSICAL_GO")

    def test_beat_unit_confusions_and_ambiguous_false_accept(self):
        with tempfile.TemporaryDirectory() as t:
            ref = self.synthesize(Path(t))
            m = score(ref, observe([80, 120, 150, 100, 120]))
            self.assertEqual((m["counts"]["wrong"], m["counts"]["beat_unit_confusion"],
                              m["counts"]["ambiguous_accepted"]), (2, 2, 1))
            self.assertEqual(m["verdict"], "NO_GO")

    def test_reject_wrong_identity_nonfinite_malformed_and_hold(self):
        with tempfile.TemporaryDirectory() as t:
            ref = self.synthesize(Path(t))
            for case in ([-1, 80, 150, 100, None], [float("nan"), 80, 150, 100, None],
                         [float("inf"), 80, 150, 100, None]):
                with self.assertRaises(ValueError):
                    score(ref, observe(case))
            o = observe([120, 80, 150, 100, None])
            o[0]["id"] = "private_title"
            with self.assertRaises(ValueError):
                score(ref, o)
            o = observe([120, 80, 150, 100, None])
            o[0]["held"] = "1"  # non-boolean flags are unsafe
            with self.assertRaises(ValueError):
                score(ref, o)
            o = observe([120, 80, 150, 100, None])
            ref["cases"][1]["reference_bpm"] = 120
            with self.assertRaises(ValueError):
                score(ref, o)

    def test_real_host_review_hold_keeps_measurement_but_does_not_accept(self):
        # Probe records numeric measured BPM and independent withheld flag.
        with tempfile.TemporaryDirectory() as tmp:
            ref = self.synthesize(Path(tmp))
            observed = observe([120, 80, 150, 100, 80])
            observed[0]["held"] = True  # measured 120 but intentionally held
            observed[4]["held"] = True  # measured 80 underdetermined
            result = score(ref, observed)
            self.assertEqual((result["counts"]["correct_020"],
                              result["counts"]["held"],
                              result["counts"]["ambiguous_held"]), (3, 1, 1))
            self.assertEqual(result["details"][0][1], "review_hold")
            self.assertEqual(result["verdict"], "RESEARCH_ONLY_NOT_MUSICAL_GO")

    def test_zero_bpm_without_hold_flag_is_nonaccepted_no_candidate(self):
        with tempfile.TemporaryDirectory() as tmp:
            ref = self.synthesize(Path(tmp))
            observed = observe([120, 80, 150, 100, None])
            observed[0] = dict(id=CASES[0][0], bpm="0", held=False)
            observed[4] = dict(id=CASES[4][0], bpm="0", held=False)
            result = score(ref, observed)
            self.assertEqual(result["counts"]["held"], 1)
            self.assertEqual(result["counts"]["ambiguous_held"], 1)
            self.assertEqual(result["details"][0][1], "no_candidate")

    def test_ci_only_meter_allowlist_and_no_write_host_contract(self):
        root = Path(__file__).resolve().parents[1]
        header = (root / "tests" / "ci_bpm_oracle_sink.h").read_text(encoding="utf-8")
        host = (root / "tests" / "diagnostic_live_host_smoke.ps1").read_text(encoding="utf-8")
        workflow = (root / ".github" / "workflows" / "source.yml").read_text(encoding="utf-8")
        for name, _, _, _, _ in CASES:
            self.assertEqual(header.count('L"' + name + '"'), 1)
            self.assertIn(name, host)
        self.assertIn('read_env(L"GITHUB_ACTIONS") != L"true"', header)
        self.assertIn('read_env(L"RUNNER_TEMP")', header)
        self.assertIn("SMART_TEMPO_CI_FIXTURE", host)
        self.assertIn("MEDIA_WRITE_SAFETY_", host)
        self.assertIn("Run-Case $installer $root $meterWave", host)
        self.assertIn("score_synthetic_meter_challenge.py --manifest", host)
        self.assertIn("SYNTHETIC_REAL_METER_HOST_CASE_COUNT_x64=5", host)
        self.assertIn("SYNTHETIC_METER_HOST_RESPONSE_MS_MEDIAN", host)
        self.assertIn("SYNTHETIC_METER_HOST_RESPONSE_MS_P95", host)
        self.assertIn("SYNTHETIC_METER_POLICY id={0}", host)
        self.assertIn("Invalid public meter policy trace", host)
        scorer = (root / "tools" / "score_synthetic_meter_challenge.py").read_text(
            encoding="utf-8")
        self.assertIn("METER_CHALLENGE_CASE id=", scorer)
        self.assertIn("$meterTimes.Count -ne 5", host)
        self.assertIn('[System.Management.Automation.Language.Parser]::ParseFile', workflow)
        self.assertTrue(host.rstrip().endswith(
            "} finally { if (Test-Path $root) { Remove-Item $root -Recurse "
            "-Force -ErrorAction SilentlyContinue } }"))

    def test_frozen_audio_mutation_detectable(self):
        with tempfile.TemporaryDirectory() as t:
            root = Path(t)
            self.synthesize(root)
            f = root / (CASES[0][0] + ".wav")
            raw = bytearray(f.read_bytes())
            raw[-3] ^= 1
            self.assertNotEqual(hashlib.sha256(raw).hexdigest(), EXPECTED[CASES[0][0]])


if __name__ == "__main__":
    unittest.main()
