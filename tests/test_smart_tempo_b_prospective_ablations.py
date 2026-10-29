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
        native_correct = int(i % 3 == 0)
        recall = {
            "ROOT_CAUSE_STAGE": ("reference_in_selected_output"
                                 if native_correct else "primary_selected_other"),
            "PRIMARY_COMPARATOR_DIAGNOSIS": ("reference_primary_selected"
                                             if native_correct else "reference_ranked_lost"),
            "RAW_NEAR_020": 1, "CLUSTER_NEAR_020": 1,
            "POLICY_BOARD_NEAR_020": 1,
            "PRIMARY_NEAR_020": native_correct,
            "POLICY_NEAR_020": native_correct,
            "FINAL_ACCEPTED_NEAR_020": native_correct,
        }
        rows.append({"case": case, "ablation": ablation, "recall": recall,
                     "media_no_write": True})
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

    def test_paired_counterfactuals_and_research_only_gate(self):
        result = validate([fixture("Win32"), fixture("x64")], "a" * 40)
        self.assertTrue(result["cross_arch_case_parity"])
        self.assertEqual(result["production_admission"], "NO_GO")
        for arch in ("Win32", "x64"):
            pair = result["paired"][arch]["AGREEMENT_ONLY"]
            self.assertEqual(pair["LOSS_CORRECT_020"], 1)
            self.assertEqual(pair["GAIN_HELD"], 3)
            self.assertEqual(pair["LOSS_WRONG_AUTO"], 2)
            self.assertEqual(result["gates"][arch]["AGREEMENT_ONLY"],
                             "NO_SYNTHETIC_ACCURACY_GAIN")
            self.assertEqual(result["gates"][arch]["COMBINED"],
                             "NO_SYNTHETIC_ACCURACY_GAIN")

    def test_regression_gate_and_aggregate_preserving_host_drift(self):
        reports = [fixture("Win32"), fixture("x64")]
        for report in reports:
            row = report["cases"][0]["ablation"]
            row["COMBINED_CORRECT_005"] = 0
            row["COMBINED_CORRECT_020"] = 0
            row["COMBINED_WRONG_AUTO"] = 1
        result = validate(reports, "a" * 40)
        for arch in ("Win32", "x64"):
            self.assertEqual(result["gates"][arch]["COMBINED"],
                             "SYNTHETIC_RISK_REGRESSION")
            self.assertEqual(result["paired"][arch]["COMBINED"]["LOSS_CORRECT_020"], 1)
            self.assertEqual(result["paired"][arch]["COMBINED"]["GAIN_WRONG_AUTO"], 1)
        drift = [fixture("Win32"), fixture("x64")]
        # Swap outcomes but not case identities: totals remain identical.
        a, b = drift[1]["cases"][0], drift[1]["cases"][1]
        a["ablation"], b["ablation"] = b["ablation"], a["ablation"]
        with self.assertRaisesRegex(ProspectiveError, "case-level drift"):
            validate(drift, "a" * 40)

    def test_causal_stage_join_and_adversarial_forgery(self):
        reports = [fixture("Win32"), fixture("x64")]
        out = validate(reports, "a" * 40)
        for arch in ("Win32", "x64"):
            self.assertEqual(out["causal"][arch]["root"]["reference_in_selected_output"], 2)
            self.assertEqual(out["causal"][arch]["root"]["primary_selected_other"], 4)
            self.assertEqual(sum(out["causal"][arch]["comparator"].values()), len(CASES))
        mutations = (
            lambda x: x[0]["cases"][0]["recall"].pop("RAW_NEAR_020"),
            lambda x: x[0]["cases"][0]["recall"].update(PRIVATE_PATH="forbidden"),
            lambda x: x[0]["cases"][0]["recall"].update(ROOT_CAUSE_STAGE="unknown"),
            lambda x: x[0]["cases"][0]["recall"].update(PRIMARY_COMPARATOR_DIAGNOSIS="unknown"),
            lambda x: x[0]["cases"][0]["recall"].update(FINAL_ACCEPTED_NEAR_020=0),
            lambda x: x[0]["cases"][1]["recall"].update(RAW_NEAR_020=-1),
            lambda x: x[0]["cases"][1]["recall"].update(PRIMARY_NEAR_020=2),
            lambda x: x[0]["cases"][1]["recall"].update(ROOT_CAUSE_STAGE="missing_raw_reference"),
            lambda x: x[0]["cases"][0]["recall"].update(RAW_NEAR_020=2),
        )
        for index, mutation in enumerate(mutations):
            if index == 8:
                # A source denominator above one is legal: not a mutation.
                continue
            bad = copy.deepcopy(reports)
            mutation(bad)
            with self.subTest(mutation=index), self.assertRaises(ProspectiveError):
                validate(bad, "a" * 40)
        drift = copy.deepcopy(reports)
        drift[1]["cases"][0]["recall"]["RAW_NEAR_020"] = 2
        with self.assertRaisesRegex(ProspectiveError, "case-level drift"):
            validate(drift, "a" * 40)

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
        for case in row["cases"]:
            case["recall"]["FINAL_ACCEPTED_NEAR_020"] = 0
            case["recall"]["ROOT_CAUSE_STAGE"] = "primary_selected_other"
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
        self.assertIn("ROOT_CAUSE_STAGE", host)
        self.assertIn("PRIMARY_COMPARATOR_DIAGNOSIS", host)
        self.assertNotIn("audit_smart_tempo_b_prospective_ablations",
                         (ROOT / "src/foo_smart_tempo/modern_bpm_analyzer.cpp").read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
