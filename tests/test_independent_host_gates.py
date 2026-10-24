"""Regression contract for independent, fail-closed Win32/x64 host gates.

Only synthetic workflow text is mutated in negative tests. No foobar runtime,
audio, personal data or executable binaries are used.
"""
from pathlib import Path
import re
import unittest

WORKFLOW = Path(__file__).resolve().parents[1] / ".github/workflows/source.yml"

STEP_PATTERN = re.compile(
    r"(?ms)^      - name: (?P<name>[^\n]+)\n(?P<body>.*?)(?=^      - name: |\Z)"
)

BUILD = "Build Win32 and x64 with public SDK"
WIN32 = "HARD GATE - real foobar2000 Win32 component load"
X64 = "HARD GATE - real foobar2000 x64 component load"
INDEPENDENT_X64 = "if: ${{ !cancelled() && steps.source_build.outcome == 'success' }}"


def validate(source: str) -> None:
    steps = {}
    for match in STEP_PATTERN.finditer(source):
        name = match.group("name")
        if name in steps:
            raise ValueError(f"Duplicate workflow step: {name}")
        steps[name] = match.group("body")
    for name in (BUILD, WIN32, X64):
        if name not in steps:
            raise ValueError(f"Missing required workflow step: {name}")
    build, win32, x64 = (steps[name] for name in (BUILD, WIN32, X64))
    if "id: source_build" not in build:
        raise ValueError("Build result must have an explicit step ID")
    if INDEPENDENT_X64 not in x64:
        raise ValueError("x64 must run after Win32 failure, but not after build failure/cancellation")
    for name, body, dll, arch in (
        (WIN32, win32, "build/foo_smart_tempo.dll", "Win32"),
        (X64, x64, "build/x64/foo_smart_tempo.dll", "x64"),
    ):
        if "continue-on-error:" in body:
            raise ValueError(f"Hard gate must remain fail-closed: {name}")
        if f"architecture: {arch}" not in body or f"component-dll: {dll}" not in body:
            raise ValueError(f"Missing actual {arch} product DLL host probe")
        if "foobar-host-smoke@" not in body:
            raise ValueError(f"Missing shared original foobar host runner: {name}")
    if source.index("      - name: " + WIN32) > source.index("      - name: " + X64):
        raise ValueError("Host gate order changed")


class IndependentHostGates(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = WORKFLOW.read_text(encoding="utf-8")

    def test_actual_workflow_has_independent_fail_closed_gates(self):
        validate(self.source)

    def test_missing_build_result_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "explicit step ID"):
            validate(self.source.replace("id: source_build", "id: unrelated", 1))

    def test_implicit_success_skipping_x64_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "x64 must run"):
            validate(self.source.replace(INDEPENDENT_X64, "if: ${{ success() }}", 1))

    def test_unconditional_x64_after_failed_build_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "x64 must run"):
            validate(self.source.replace(INDEPENDENT_X64, "if: ${{ always() }}", 1))

    def test_suppressed_win32_failure_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "fail-closed"):
            validate(self.source.replace(
                "      - name: " + WIN32 + "\n",
                "      - name: " + WIN32 + "\n        continue-on-error: true\n",
                1,
            ))

    def test_suppressed_x64_failure_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "fail-closed"):
            validate(self.source.replace(
                "      - name: " + X64 + "\n",
                "      - name: " + X64 + "\n        continue-on-error: true\n",
                1,
            ))

    def test_missing_x64_binary_probe_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "Missing actual x64"):
            validate(self.source.replace("build/x64/foo_smart_tempo.dll", "build/foo_smart_tempo.dll", 1))


if __name__ == "__main__":
    unittest.main()
