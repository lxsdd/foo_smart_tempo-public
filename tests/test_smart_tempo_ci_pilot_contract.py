"""Source-only guard for the fixed four-case, dual-architecture B pilot."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
CASES = ("pulse_60", "pulse_128", "pulse_120_sparse", "silence")
EXTENDED = ("pulse_90", "pulse_120", "pulse_140", "pulse_175", "pulse_128_syncopated", "pulse_120_swing", "pulse_128_half_pulse")


class PilotContract(unittest.TestCase):
    def read(self, path):
        return (ROOT / path).read_text(encoding="utf-8")

    def test_shared_compiled_fixture_whitelist(self):
        header = self.read("tests/ci_fixture_allowlist.h")
        for case in CASES:
            self.assertEqual(header.count(case + ".wav"), 1)
        self.assertEqual(header.count(".wav"), len(CASES) + len(EXTENDED))
        for case in EXTENDED:
            self.assertEqual(header.count(case + ".wav"), 1)
        self.assertEqual(header.count("pulse_60_chill.cue"), 1)
        self.assertEqual(header.count(".cue"), 1)

        self.assertIn("runner_temp", header)
        self.assertIn("_wcsnicmp", header)
        for path in ("tests/ci_bpm_oracle_sink.h", "tests/ci_pipeline_trace.h"):
            source = self.read(path)
            self.assertIn('#include "ci_fixture_allowlist.h"', source)
            self.assertIn("ci_fixture::permitted(fixture, root)", source)
            self.assertIn("GITHUB_ACTIONS", source)

    def test_same_four_cases_in_both_original_host_architectures(self):
        script = self.read("tests/diagnostic_live_host_smoke.ps1")
        self.assertIn("$pilotCases=@('pulse_60','pulse_128','pulse_120_sparse','silence')", script)
        self.assertIn("foreach ($caseId in $pilotCases)", script)
        self.assertNotIn("if ($Architecture -eq 'x64')", script)
        self.assertIn("Run-Case $installer $root $wavFile", script)
        self.assertIn("SYNTHETIC_BPM_NEGATIVE_FALSEACCEPTS=0", script)
        self.assertIn("Missing native B-pilot lineage", script)
        auditor = self.read("tools/audit_smart_tempo_ci_pipeline_trace.py")
        self.assertIn("INCOMPLETE_EMPTY_BOARD_CAUSAL", auditor)

    def test_independent_external_cue_low_pulse_host_coverage(self):
        script = self.read("tests/diagnostic_live_host_smoke.ps1")
        config = self.read("src/foo_smart_tempo/preferences.cpp")
        cohort = self.read("tools/audit_smart_tempo_b_pilot_cohort.py")
        self.assertIn("Chill; Chillout; Easy Listening", config)
        self.assertIn("'REM GENRE \"Chill\"'", script)
        self.assertIn("'FILE \"pulse_60.wav\" WAVE'", script)
        self.assertIn("Run-Case $installer $root $lowCue", script)
        self.assertIn("'SYNTHETIC_pulse_60_chill' -RequireLowPulseCoverage", script)
        self.assertIn("@($wavFile,$lowCue,$reference)", script)
        self.assertIn("if ($episodes -lt 1 -or $wins -lt 1", script)
        self.assertIn("SMART_TEMPO_NATIVE_LOW_PULSE_", script)
        self.assertIn("if ($RequireLowPulseCoverage)", script)
        self.assertIn("} else {\n        # Historical accuracy cohort remains fixed", script)
        self.assertIn("SELECTOR_LOW_BESTBY_EPISODES", cohort)
        self.assertLess(script.index("Run-Case $installer $root $lowCue"),
                        script.index("$pilotReport=Join-Path"))

    def test_low_pulse_fixture_cannot_expand_production_paths(self):
        guard = self.read("tests/ci_fixture_allowlist.h")
        script = self.read("tests/diagnostic_live_host_smoke.ps1")
        self.assertIn("synthetic-oracle\\\\pulse_60_chill.cue", guard)
        self.assertIn("runner_temp", guard)
        self.assertNotIn("SMART_TEMPO_CI_ROUTE_OVERRIDE", script)
        self.assertEqual(script.count("-RequireLowPulseCoverage"), 1)
        self.assertIn("if ($caseId -ceq 'pulse_60')", script)

    def test_native_two_center_direct_selector_proof_is_ci_only_and_real_host(self):
        source = self.read("tests/smart_tempo_ci_probe.cpp")
        script = self.read("tests/diagnostic_live_host_smoke.ps1")
        project = self.read("src/foo_smart_tempo/foo_smart_tempo.vcxproj")
        self.assertIn("SMART_TEMPO_CI_ORACLE_PROBE", source)
        self.assertIn("native_continuous_selector_contract(99., 99., 2, 0)", source)
        self.assertIn("native_continuous_selector_contract(96., 96., 1, 1)", source)
        self.assertIn("compute_mir_primary_selection(", source)
        self.assertIn("mir_ci_primary_selector_events()", source)
        self.assertIn('row.tie_metric_4 != -std::abs(row.bpm - center)', source)
        self.assertIn('CREATE_NEW', source)
        self.assertIn('SMART_TEMPO_CI_NATIVE_CONTINUOUS=PASS', source)
        self.assertIn("SMART_TEMPO_CI_NATIVE_CONTINUOUS=PASS", script)
        self.assertIn("Get-Content -LiteralPath $nativeProof -Raw", script)
        self.assertIn("throw 'Original host did not prove native", script)
        self.assertIn("SmartTempoCIRuntimeProbe", project)
        self.assertNotIn("std::getenv(\"USERPROFILE\")", source)
        # Neither generic release component nor owners' music files can run
        # or observe this branch fixture outside ephemeral Actions runner.
        self.assertIn("githubActions != L\"true\"", source)
        self.assertIn("RUNNER_TEMP", source)
        self.assertEqual(script.count('SMART_TEMPO_NATIVE_CONTINUOUS_{0}_{1}=PASS'), 1)

    def test_original_host_script_not_truncated_or_repeated(self):
        script = self.read("tests/diagnostic_live_host_smoke.ps1")
        self.assertEqual(script.count("function Run-Case("), 1)
        self.assertEqual(script.count("$root=Join-Path $env:RUNNER_TEMP ('smart-tempo-'"), 1)
        self.assertEqual(script.count("$trace=$inputFile+'.pipeline.jsonl'"), 1)
        self.assertEqual(script.count("SYNTHETIC_BPM_([A-Z0-9_]+)=([0-9]+)$"), 1)
        self.assertEqual(script.count("SMART_TEMPO_TRACE_([A-Z0-9_]+)=([A-Za-z0-9_.]+)$"), 1)
        self.assertIn("Incomplete native pilot scorer evidence", script)
        self.assertIn("Incomplete native pilot trace evidence", script)
        self.assertIn("$script:pilotEvidence.Add(", script)
        self.assertTrue(script.rstrip().endswith("Remove-Item $root -Recurse -Force -ErrorAction SilentlyContinue } }"))
        workflow = self.read(".github/workflows/source.yml")
        self.assertIn("PowerShell host harness AST parser", workflow)
        self.assertIn("Parser]::ParseFile", workflow)

    def test_no_false_causal_or_release_pass(self):
        workflow = self.read(".github/workflows/source.yml")
        self.assertNotIn("upload-artifact@", workflow)
        for arch in ("Win32", "x64"):
            self.assertIn("HARD GATE - " + arch + " diagnostic menu and media no-write", workflow)
        script = self.read("tests/diagnostic_live_host_smoke.ps1")
        self.assertNotIn("PHASE_B_CAUSAL_COMPLETENESS=PASS", script)
        self.assertIn("audit_smart_tempo_b_pilot_cohort.py", script)
        self.assertIn("smart-tempo-b-pilot-", script)
        self.assertIn("media_no_write=$true", script)
        self.assertIn("[Text.UTF8Encoding]::new($false)", script)
        self.assertIn("--expected-source-sha $sourceSha", script)
        self.assertIn("--expected-source-sha $sourceSha", workflow)
        self.assertIn("object_pairs_hook=unique_json_object", self.read("tools/audit_smart_tempo_b_pilot_cohort.py"))
        workflow = self.read(".github/workflows/source.yml")
        self.assertIn("joined Win32/x64 native B-pilot cohort evidence", workflow)
        self.assertIn("test_smart_tempo_b_pilot_cohort.py", workflow)
        self.assertNotIn("MUSICAL_BPM_QUALITY=PASS", script)


if __name__ == "__main__":
    unittest.main()
