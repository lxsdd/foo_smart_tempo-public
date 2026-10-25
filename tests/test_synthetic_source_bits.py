"""Positive and fail-closed CI-only provenance tests (no music/media writes)."""
from __future__ import annotations

import csv
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from audit_synthetic_source_bits import audit_source_bits, load_source_bits
from generate_synthetic_complex_challenge import CASES
from test_synthetic_export_rank import fixture


def masks_for(signals):
    return {name: [1] * len(board) for name, board in signals.items()}


def save_csv(path, signals, masks):
    with path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        writer.writerow(("id", "rank", "bpm", "source_bits"))
        for name, _, _ in CASES:
            for row, mask in zip(signals[name], masks[name]):
                writer.writerow((name, row["rank"], row["bpm"], mask))


class ProvenanceTests(unittest.TestCase):
    def test_positive_source_and_stage_denominators(self):
        scored, signals, stages = fixture()
        masks = masks_for(signals)
        masks[CASES[0][0]] = [1, 2]
        masks[CASES[1][0]] = [1, 2]
        masks[CASES[2][0]] = [16]
        result = audit_source_bits(scored, signals, stages, masks, CASES)
        total = result["totals"]
        self.assertEqual((total["wrong"], total["precise"],
                          total["precise_missing"]), (3, 2, 1))
        self.assertEqual((total["output"], total["output_missing"]), (3, 0))
        self.assertEqual((total["precise_full"], total["precise_primary"]), (1, 1))
        self.assertEqual((total["output_full"], total["output_primary"],
                          total["output_supplement_only"]), (1, 1, 1))
        self.assertEqual(result["interpretation"],
                         "merged_review_sources_not_primary_eligibility")
        self.assertEqual(result["cohorts"]["clear"]["cases"], 11)
        self.assertEqual(result["cohorts"]["risk"]["cases"], 5)
        self.assertEqual(result["cohorts"]["clear"]["candidate_rows"], 13)
        self.assertEqual(result["cohorts"]["risk"]["candidate_rows"], 5)
        self.assertEqual(result["cohorts"]["clear"]["candidate_full_rows"], 10)
        self.assertEqual(result["cohorts"]["clear"]["candidate_primary_rows"], 2)
        self.assertEqual(result["cohorts"]["clear"]["candidate_supplement_only_rows"], 1)
        self.assertEqual(result["cohorts"]["clear"]["cases_with_primary"], 2)
        self.assertEqual(result["cohorts"]["clear"]["cases_with_supplement_only"], 1)

    def test_merged_and_supplemental_provenance(self):
        scored, signals, stages = fixture()
        masks = masks_for(signals)
        masks[CASES[0][0]][0] = 3
        masks[CASES[1][0]][1] = 8
        result = audit_source_bits(scored, signals, stages, masks, CASES)
        self.assertEqual((result["totals"]["precise_full"],
                          result["totals"]["precise_primary"],
                          result["totals"]["precise_supplement_only"]), (1, 1, 1))

    def test_csv_strict_rank_bpm_id_and_mask(self):
        _, signals, _ = fixture()
        masks = masks_for(signals)
        names = [case[0] for case in CASES]
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "synthetic_sources.csv"
            save_csv(path, signals, masks)
            self.assertEqual(load_source_bits(path, names, signals), masks)
            for bad in ("0", "32", "256", "01", "nan", "1.5", "-1", "1;private", ""):
                with self.subTest(bad=bad):
                    with path.open(encoding="utf-8") as f:
                        rows = list(csv.reader(f))
                    rows[1][3] = bad
                    with path.open("w", encoding="utf-8", newline="") as f:
                        csv.writer(f).writerows(rows)
                    with self.assertRaises(ValueError):
                        load_source_bits(path, names, signals)
                    save_csv(path, signals, masks)
            for column, bad in ((0, "private_track"), (1, "123"),
                                (2, "120.125")):
                with self.subTest(column=column):
                    with path.open(encoding="utf-8") as f:
                        rows = list(csv.reader(f))
                    rows[1][column] = bad
                    with path.open("w", encoding="utf-8", newline="") as f:
                        csv.writer(f).writerows(rows)
                    with self.assertRaises(ValueError):
                        load_source_bits(path, names, signals)
                    save_csv(path, signals, masks)
            with path.open(encoding="utf-8") as f:
                rows = list(csv.reader(f))
            with path.open("w", encoding="utf-8", newline="") as f:
                csv.writer(f).writerows(rows[:-1])
            with self.assertRaises(ValueError):
                load_source_bits(path, names, signals)

    def test_wrong_count_and_board_errors_fail_closed(self):
        scored, signals, stages = fixture()
        masks = masks_for(signals)
        masks[CASES[0][0]] = []
        with self.assertRaises(ValueError):
            audit_source_bits(scored, signals, stages, masks, CASES)
        masks = masks_for(signals)
        masks["private_track"] = [1]
        with self.assertRaises(ValueError):
            audit_source_bits(scored, signals, stages, masks, CASES)
        masks = masks_for(signals)
        stages["stages"]["writer_override"]["clear_wrong"] += 1
        with self.assertRaises(ValueError):
            audit_source_bits(scored, signals, stages, masks, CASES)
        scored, signals, stages = fixture()
        masks = masks_for(signals)
        signals[CASES[0][0]][0]["bpm"] = "nan"
        with self.assertRaises(ValueError):
            audit_source_bits(scored, signals, stages, masks, CASES)

    def test_compiled_ci_source_mask_contract(self):
        header = (ROOT / "tests" / "ci_bpm_oracle_sink.h").read_text(encoding="utf-8")
        cpp = (ROOT / "src" / "foo_smart_tempo" /
               "mir_candidate_pipeline.cpp").read_text(encoding="utf-8")
        for token in ("full_board", "primary_selector", "material_risk_probe",
                      "partial_bar_aggregate", "partial_bar_topk"):
            self.assertIn('"' + token + '"', header)
            self.assertIn('"' + token + '"', cpp)
        self.assertIn("bounded_source_bits", header)
        self.assertIn("candidate.sources.get_ptr()", header)
        self.assertIn('rank,bpm,source_bits', header)
        self.assertNotIn("origins += candidate.sources", header)


    def test_ci_powershell_source_sidecar_contract(self):
        script = (ROOT / "tests" / "diagnostic_live_host_smoke.ps1").read_text(
            encoding="utf-8")
        self.assertEqual(script.count("SMART_TEMPO_DIAGNOSTIC_RUNTIME_"), 1)
        self.assertEqual(script.count("$challengeSourceRows.Add("), 1)
        self.assertEqual(script.count("Import-Csv -LiteralPath ($wav+'.sources.csv')"), 1)
        self.assertEqual(script.count("foreach ($candidate in $evidence)"), 1)
        self.assertEqual(script.count("--source-bits $challengeSourceCsv"), 1)
        self.assertIn("^(?:[1-9]|[12][0-9]|3[01])$') {", script)
        self.assertIn("throw 'Complex provenance bitmask or board mismatch'", script)
        self.assertIn("if ($sources.Count -ne $evidence.Count)", script)
        self.assertIn("source_bits=$source.source_bits", script)
        self.assertEqual(script.count("} finally { if (Test-Path $root)"), 1)
        self.assertTrue(script.rstrip().endswith(
            "} finally { if (Test-Path $root) { Remove-Item $root -Recurse "
            "-Force -ErrorAction SilentlyContinue } }"))

    def test_source_csv_rejects_extra_column_reordering_and_duplicate(self):
        _, signals, _ = fixture()
        masks = masks_for(signals)
        names = [case[0] for case in CASES]
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "source.csv"
            save_csv(path, signals, masks)
            with path.open(encoding="utf-8") as f:
                rows = list(csv.reader(f))
            for corrupted in (
                [rows[0] + ["raw_source"]] + [row + ["private"] for row in rows[1:]],
                [rows[0]] + list(reversed(rows[1:])),
                [rows[0], rows[1], rows[1]] + rows[2:],
                [rows[0][::-1]] + rows[1:],
            ):
                with self.subTest(corrupted=len(corrupted)):
                    with path.open("w", newline="", encoding="utf-8") as f:
                        csv.writer(f).writerows(corrupted)
                    with self.assertRaises(ValueError):
                        load_source_bits(path, names, signals)

    def test_held_and_risk_cases_cannot_be_relabelled_wrong(self):
        scored, signals, stages = fixture()
        masks = masks_for(signals)
        held = next(i for i, row in enumerate(scored["results"]) if row["held"])
        self.assertGreaterEqual(held, 0)
        stages["details"][held] = (
            stages["details"][held][0], "primary", "wrong", "na", "na")
        with self.assertRaises(ValueError):
            audit_source_bits(scored, signals, stages, masks, CASES)


    def test_source_cohort_overlap_is_nonexclusive(self):
        scored, signals, stages = fixture()
        masks = masks_for(signals)
        masks[CASES[0][0]][0] = 31  # full, primary, three supplements
        masks[CASES[1][0]][0] = 4   # supplement-only
        result = audit_source_bits(scored, signals, stages, masks, CASES)
        clear = result["cohorts"]["clear"]
        self.assertEqual(clear["cases"], 11)
        self.assertEqual(clear["candidate_rows"], 13)
        self.assertEqual(clear["candidate_primary_rows"], 1)
        self.assertEqual(clear["candidate_supplement_only_rows"], 1)
        self.assertEqual(clear["candidate_full_rows"], 12)
        self.assertEqual(clear["cases_with_primary"], 1)
        self.assertEqual(clear["cases_with_supplement_only"], 1)
        self.assertLessEqual(clear["cases_with_full"], clear["cases"])

    def test_empty_risk_board_is_explicitly_counted(self):
        scored, signals, stages = fixture()
        masks = masks_for(signals)
        risk = next(name for name, _, style in CASES if style == "tone")
        signals[risk] = []
        masks[risk] = []
        result = audit_source_bits(scored, signals, stages, masks, CASES)
        self.assertEqual(result["cohorts"]["risk"]["cases"], 5)
        self.assertEqual(result["cohorts"]["risk"]["cases_with_board"], 4)
        self.assertEqual(result["cohorts"]["risk"]["candidate_rows"], 4)


if __name__ == "__main__":
    unittest.main()
