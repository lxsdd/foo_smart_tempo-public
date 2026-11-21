"""Synthetic-only adversarial tests for the read-only paired Phase-B scorecard."""
import contextlib
import copy
import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

MODULE_PATH = Path(__file__).resolve().parents[1] / "tools/audit_smart_tempo_frozen_matrix.py"
spec = importlib.util.spec_from_file_location("frozen_matrix_score", MODULE_PATH)
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
SHA = "a" * 40


def fixture(count=5):
    manifest = {"schema": "smart_tempo_frozen_matrix_reference_v1",
                "cohort": "public_synthetic_frozen", "cases": []}
    for i in range(count):
        category = ("meter_ambiguous" if i == 2 else
                    "nonstationary" if i == 3 else
                    "nonrhythmic" if i == 4 else "clear")
        bpm = None if category in ("nonstationary", "nonrhythmic") else 120.0
        manifest["cases"].append({"id": f"probe_{i:03d}", "reference_bpm": bpm, "class": category})
    native_rows = []
    for i, c in enumerate(manifest["cases"]):
        measured = 120.0 if c["class"] == "clear" else 0.0
        native_rows.append({"id": c["id"], "native_bpm": measured,
                            "native_held": not measured, "shadow_bpm": measured,
                            "shadow_held": not measured, "cpu_ms": 90 + i,
                            "wall_ms": 180 + i, "peak_rss_bytes": 40_000_000,
                            "native_source_indexed": bool(measured),
                            "supplemental_witnessed": False,
                            "shadow_source_indexed": bool(measured),
                            "media_no_write": True})
    reports = [{"schema": "smart_tempo_frozen_matrix_host_v1",
                "architecture": arch, "source_sha": SHA,
                "cases": copy.deepcopy(native_rows)} for arch in ("Win32", "x64")]
    return manifest, reports


class ScorecardTests(unittest.TestCase):
    def test_exact_paired_baseline(self):
        manifest, reports = fixture()
        output = m.score(manifest, reports, SHA)
        self.assertEqual(output["synthetic_case_configurations"], 5)
        self.assertEqual(output["clear"], 2)
        self.assertEqual(output["architectures"]["Win32"]["outcomes"]["native"]["correct_005"], 2)
        self.assertEqual(output["architectures"]["x64"]["outcomes"]["native"]["held"], 3)
        self.assertEqual(output["musical_quality_admission"], "NOT_ESTABLISHED")
        self.assertEqual(output["production_selector_admission"], "NO_GO")
        self.assertEqual(output["architectures"]["Win32"]["resources"]["cpu_ms_p95"], 94)
        self.assertEqual(output["architectures"]["x64"]["resources"]["wall_ms_p99"], 184)

    def test_paired_gains_losses_and_false_accepts(self):
        manifest, reports = fixture()
        for report in reports:
            report["cases"][0]["native_bpm"] = 60.0  # half tempo alias
            report["cases"][1]["shadow_bpm"] = 0.0
            report["cases"][1]["shadow_held"] = True
            report["cases"][3]["shadow_bpm"] = 110.0
            report["cases"][3]["shadow_held"] = False
            report["cases"][3]["shadow_source_indexed"] = True
        result = m.score(manifest, reports, SHA)["architectures"]["Win32"]
        self.assertEqual(result["outcomes"]["native"]["metric_alias"], 1)
        self.assertEqual(result["outcomes"]["shadow"]["risk_accept"], 1)
        self.assertEqual(result["paired_changes"]["gain_020"], 1)
        self.assertEqual(result["paired_changes"]["loss_020"], 1)
        self.assertEqual(result["paired_changes"]["new_risk"], 1)

    def test_no_output_without_two_distinct_host_reports(self):
        manifest, reports = fixture()
        for bad in (reports[:1], [reports[0], reports[0]], reports + reports[:1]):
            with self.subTest(count=len(bad)), self.assertRaises(m.EvidenceError):
                m.score(manifest, bad, SHA)

    def test_same_totals_but_different_cases_are_rejected(self):
        manifest, reports = fixture()
        reports[1]["cases"][0]["native_bpm"] = 0.0
        reports[1]["cases"][0]["native_held"] = True
        reports[1]["cases"][1]["native_bpm"] = 240.0
        with self.assertRaisesRegex(m.EvidenceError, "cross-architecture"):
            m.score(manifest, reports, SHA)

    def test_adversarial_report_mutations(self):
        mutations = (
            ("source_sha", "b" * 40),
            ("schema", "private_media_dump"),
            ("architecture", "Linux"),
        )
        for key, value in mutations:
            manifest, reports = fixture()
            reports[0][key] = value
            with self.subTest(key=key), self.assertRaises(m.EvidenceError):
                m.score(manifest, reports, SHA)
        row_mutations = (
            ("id", "personal_track_name"),
            ("native_bpm", float("nan")),
            ("native_bpm", True),
            ("native_bpm", -1.0),
            ("shadow_bpm", float("inf")),
            ("native_held", "false"),
            ("media_no_write", False),
            ("native_source_indexed", False),
            ("cpu_ms", -3),
            ("wall_ms", float("inf")),
            ("peak_rss_bytes", True),
            ("peak_rss_bytes", 1.5),
            ("shadow_source_indexed", "yes"),
        )
        for key, value in row_mutations:
            manifest, reports = fixture()
            reports[0]["cases"][0][key] = value
            with self.subTest(field=key), self.assertRaises(m.EvidenceError):
                m.score(manifest, reports, SHA)
        manifest, reports = fixture()
        reports[0]["cases"][0]["private_file_path"] = "/home/user/song.wav"
        with self.assertRaises(m.EvidenceError):
            m.score(manifest, reports, SHA)

    def test_supplemental_witness_and_source_gap(self):
        manifest, reports = fixture()
        for report in reports:
            row = report["cases"][0]
            row["native_source_indexed"] = False
            row["supplemental_witnessed"] = True
        self.assertTrue(m.score(manifest, reports, SHA)["cross_arch_case_parity"])
        reports[0]["cases"][0]["supplemental_witnessed"] = False
        with self.assertRaisesRegex(m.EvidenceError, "source"):
            m.score(manifest, reports, SHA)

    def test_shadow_cannot_claim_unwitnessed_oracle_bpm(self):
        manifest, reports = fixture()
        for report in reports:
            report["cases"][0]["shadow_bpm"] = 120.0
            report["cases"][0]["shadow_source_indexed"] = False
        with self.assertRaisesRegex(m.EvidenceError, "shadow BPM lacks indexed"):
            m.score(manifest, reports, SHA)

    def test_unknown_case_missing_case_and_duplicate_case_fail(self):
        manifest, reports = fixture()
        reports[0]["cases"][0]["id"] = "challenge_unknown"
        with self.assertRaises(m.EvidenceError):
            m.score(manifest, reports, SHA)
        manifest, reports = fixture()
        reports[0]["cases"].append(copy.deepcopy(reports[0]["cases"][0]))
        with self.assertRaises(m.EvidenceError):
            m.score(manifest, reports, SHA)
        manifest, reports = fixture()
        reports[0]["cases"][0]["id"] = reports[0]["cases"][1]["id"]
        with self.assertRaises(m.EvidenceError):
            m.score(manifest, reports, SHA)

    def test_non_clear_grid_is_not_accepted_as_single_correct_bpm(self):
        manifest, reports = fixture()
        for report in reports:
            report["cases"][2]["native_bpm"] = 120
            report["cases"][2]["native_held"] = False
            report["cases"][2]["native_source_indexed"] = True
        result = m.score(manifest, reports, SHA)
        self.assertEqual(result["architectures"]["Win32"]["outcomes"]["native"]["risk_accept"], 1)

    def test_manifest_truth_integrity_and_private_ids(self):
        cases = (("cases", []), ("cohort", "private_audio"))
        for field, changed in cases:
            manifest, reports = fixture()
            manifest[field] = changed
            with self.assertRaises(m.EvidenceError):
                m.score(manifest, reports, SHA)
        for field, changed in (("id", "private song"), ("reference_bpm", float("inf")),
                               ("class", "owners_song")):
            manifest, reports = fixture()
            manifest["cases"][0][field] = changed
            with self.assertRaises(m.EvidenceError):
                m.score(manifest, reports, SHA)
        manifest, reports = fixture()
        manifest["cases"][3]["reference_bpm"] = 127.0
        with self.assertRaises(m.EvidenceError):
            m.score(manifest, reports, SHA)

    def test_scale_to_239_frozen_case_configurations_without_audio_files(self):
        manifest, reports = fixture(239)
        result = m.score(manifest, reports, SHA)
        self.assertEqual(result["synthetic_case_configurations"], 239)
        self.assertEqual(result["architectures"]["Win32"]["count"], 239)
        self.assertFalse(result["architectures"]["x64"]["paired_changes"]["new_risk"])
        self.assertEqual(result["private_blind_holdout"], "NOT_RUN")

    def test_cli_duplicate_json_bom_nan_and_refuse_overwrite(self):
        manifest, reports = fixture()
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            files = [folder / f for f in ("manifest.json", "win32.json", "x64.json")]
            for f, value in zip(files, [manifest, *reports]):
                f.write_text(json.dumps(value), encoding="utf-8")
            out = folder / "scored.json"
            args = ["audit", "--manifest", str(files[0]), "--report", str(files[1]),
                    "--report", str(files[2]), "--expected-source-sha", SHA,
                    "--json-output", str(out)]
            with patch.object(sys, "argv", args), contextlib.redirect_stdout(io.StringIO()) as stdout:
                m.main()
            self.assertIn("FROZEN_MATRIX_CASES=5", stdout.getvalue())
            self.assertEqual(json.loads(out.read_text())["production_selector_admission"], "NO_GO")
            with patch.object(sys, "argv", args), contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit):
                    m.main()
            files[0].write_text('{"schema":"a","schema":"b"}', encoding="utf-8")
            with self.assertRaisesRegex(m.EvidenceError, "duplicate JSON"):
                m.parse_json(files[0])
            files[0].write_bytes(b'\xef\xbb\xbf' + json.dumps(manifest).encode())
            self.assertEqual(m.parse_json(files[0]), manifest)
            files[0].write_text('{"bad":NaN}', encoding="utf-8")
            with self.assertRaises(m.EvidenceError):
                m.parse_json(files[0])


if __name__ == "__main__":
    unittest.main()
