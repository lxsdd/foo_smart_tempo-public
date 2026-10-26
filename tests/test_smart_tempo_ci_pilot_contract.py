"""Source-only guard for the fixed four-case, dual-architecture B pilot."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
CASES = ("pulse_60", "pulse_128", "pulse_120_sparse", "silence")


class PilotContract(unittest.TestCase):
    def read(self, path):
        return (ROOT / path).read_text(encoding="utf-8")

    def test_shared_compiled_fixture_whitelist(self):
        header = self.read("tests/ci_fixture_allowlist.h")
        for case in CASES:
            self.assertEqual(header.count(case + ".wav"), 1)
        self.assertEqual(header.count(".wav"), len(CASES))
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

    def test_no_false_causal_or_release_pass(self):
        workflow = self.read(".github/workflows/source.yml")
        self.assertNotIn("upload-artifact@", workflow)
        for arch in ("Win32", "x64"):
            self.assertIn("HARD GATE - " + arch + " diagnostic menu and media no-write", workflow)
        script = self.read("tests/diagnostic_live_host_smoke.ps1")
        self.assertNotIn("PHASE_B_CAUSAL_COMPLETENESS=PASS", script)
        self.assertNotIn("MUSICAL_BPM_QUALITY=PASS", script)


if __name__ == "__main__":
    unittest.main()
