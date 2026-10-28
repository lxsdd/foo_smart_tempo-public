"""Prospective six-case post-host Selector/Policy accounting: adversarial tests."""
import copy
import sys
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from audit_smart_tempo_b_prospective_ablations import (
    CASES, MODELS, FIELDS, ProspectiveError, validate,
)


def fixture(arch):
    rows = []
    for i, case in enumerate(CASES):
        ablation = {}
        for model in MODELS:
            accepted = int(model != "AGREEMENT_ONLY" or i % 2 == 0)
            correct = int(accepted and i % 3 == 0)
            for key, value in {
                "ACCEPTED": accepted, "HELD": 1 - accepted,
                "CORRECT_005": correct, "CORRECT_020": correct,
                "WRONG_AUTO": accepted - correct, "FALSE_ACCEPT": 0,
            }.items():
                ablation[model + "_" + key] = value
        rows.append({"case": case, "ablation": ablation, "media_no_write": True})
    return {"schema": "smart_tempo_b_prospective_ablation_v1",
            "architecture": arch, "source_sha": "a" * 40, "cases": rows}


class ProspectiveAblationTests(unittest.TestCase):
    def test_dual_host_exact_sha_and_denominators(self):
        out = validate([fixture("Win32"), fixture("x64")], "a" * 40)
        self.assertEqual(set(out["architectures"]), {"Win32", "x64"})
        for arch in out["architectures"]:
            for model in MODELS:
                row = out["architectures"][arch][model]
                self.assertEqual(row["ACCEPTED"] + row["HELD"], len(CASES))
                self.assertEqual(row["CORRECT_020"] + row["WRONG_AUTO"], row["ACCEPTED"])
        self.assertEqual(len(MODELS), 4)
        self.assertEqual(len(FIELDS), 6)

    def test_single_architecture_is_explicit_only(self):
        self.assertEqual(len(validate([fixture("Win32")], require_both=False)["architectures"]), 1)
        with self.assertRaises(ProspectiveError):
            validate([fixture("Win32")])

    def test_bad_accuracy_is_reported_not_accepted_as_good(self):
        row = fixture("Win32")
        for case in row["cases"]:
            for model in MODELS:
                for key, value in (("ACCEPTED", 1), ("HELD", 0), ("CORRECT_005", 0),
                                   ("CORRECT_020", 0), ("WRONG_AUTO", 1)):
                    case["ablation"][model + "_" + key] = value
        out = validate([row], require_both=False)
        self.assertEqual(out["architectures"]["Win32"]["COMBINED"]["WRONG_AUTO"], len(CASES))
        self.assertEqual(out["architectures"]["Win32"]["COMBINED"]["CORRECT_020"], 0)

    def test_fail_closed_corruptions(self):
        baseline = [fixture("Win32"), fixture("x64")]
        mutations = (
            lambda x: x[0].update(schema="wrong"),
            lambda x: x[0].update(extra="private"),
            lambda x: x[0].update(architecture="x64"),
            lambda x: x[0].update(source_sha="b" * 40),
            lambda x: x[0]["cases"].pop(),
            lambda x: x[0]["cases"][0].update(case=CASES[1]),
            lambda x: x[0]["cases"][0].update(media_no_write=False),
            lambda x: x[0]["cases"][0]["ablation"].update(COMBINED_ACCEPTED=2),
            lambda x: x[0]["cases"][0]["ablation"].update(COMBINED_HELD=1),
            lambda x: x[0]["cases"][1]["ablation"].update(COMBINED_CORRECT_005=1),
            lambda x: x[0]["cases"][1]["ablation"].update(COMBINED_WRONG_AUTO=0),
            lambda x: x[0]["cases"][0]["ablation"].update(PRIVATE_BPM=123),
            lambda x: x[0]["cases"][0]["ablation"].pop("COMBINED_HELD"),
        )
        for i, mutate in enumerate(mutations):
            rows = copy.deepcopy(baseline)
            mutate(rows)
            with self.subTest(mutation=i), self.assertRaises((ProspectiveError, ValueError, TypeError)):
                validate(rows, "a" * 40)

    def test_real_host_and_ci_wiring(self):
        host = (ROOT / "tests/diagnostic_live_host_smoke.ps1").read_text(encoding="utf-8")
        workflow = (ROOT / ".github/workflows/source.yml").read_text(encoding="utf-8")
        generator = (ROOT / "tools/generate_synthetic_bpm_reference.py").read_text(encoding="utf-8")
        for case in CASES:
            self.assertIn(case, host)
            self.assertIn(case, generator)
        self.assertIn("smart-tempo-b-prospective-Win32.json", workflow)
        self.assertIn("smart-tempo-b-prospective-x64.json", workflow)
        self.assertIn("test_smart_tempo_b_prospective_ablations.py", workflow)
        self.assertIn("audit_smart_tempo_b_prospective_ablations.py", host)
        self.assertNotIn("audit_smart_tempo_b_prospective_ablations",
                         (ROOT / "src/foo_smart_tempo/modern_bpm_analyzer.cpp").read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
