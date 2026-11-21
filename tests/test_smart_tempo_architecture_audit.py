"""Independent positive/negative source-level architecture tests.

No foobar host, no audio files, no metadata, no private data, no writes.
"""
from __future__ import annotations
import copy
import json
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from audit_smart_tempo_pipeline import (
    ANALYZER, AuditError, MIR, PRIMARY, STAGES, WINDOW, WRITE, PASS,
    SOURCES, audit_sources, load_sources, window_geometry
)


class ArchitectureAuditTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.sources = load_sources(ROOT)

    def test_complete_28_stage_static_inventory_and_reproducible_fingerprints(self):
        result = audit_sources(self.sources)
        self.assertEqual(result["schema"], "smart_tempo_architecture_source_audit_v1")
        self.assertEqual(result["stage_count"], 28)
        self.assertEqual(len(set(row["id"] for row in result["stages"])), 28)
        self.assertEqual(set(result["source_hashes"]), set(SOURCES))
        self.assertTrue(all(row["line"] > 0 for row in result["stages"]))
        self.assertEqual(result["core_bpm_rail"], [50.0, 200.0])
        self.assertEqual(result["production_bpm_rail"], [40.0, 220.0])
        self.assertIn("real_music_blind_holdout_not_qualified",
                      result["unqualified_evidence_gaps"])
        self.assertEqual(audit_sources(self.sources), result)

    def test_20_by_50_window_redundancy_is_theoretical_not_cpu_claim(self):
        result = audit_sources(self.sources)
        g = result["default_geometry_180s"]
        self.assertEqual(g["requested_decode_window_seconds"], 1000)
        self.assertAlmostEqual(g["unique_interval_union_seconds"], 128, places=3)
        self.assertAlmostEqual(g["estimated_repeat_factor"], 7.8125, places=3)
        self.assertIn("theoretical_window_geometry", g["kind"])
        single = window_geometry(180, 20, 1)
        self.assertEqual(single["requested_decode_window_seconds"], 20)
        self.assertEqual(single["unique_interval_union_seconds"], 20)
        self.assertEqual(single["estimated_repeat_factor"], 1)
        self.assertEqual(window_geometry(180, 20, 3)["passes"], 3)
        with self.assertRaises(AuditError):
            window_geometry(float("nan"), 20, 50)
        with self.assertRaises(AuditError):
            window_geometry(180, -1, 50)

    def test_missing_required_decision_stage_fails_closed(self):
        src = dict(self.sources)
        src[ANALYZER] = src[ANALYZER].replace(
            "compute_mir_policy_decision(", "do_not_select_policy(", 1)
        with self.assertRaisesRegex(AuditError, "missing source stage policy_select"):
            audit_sources(src)

    def test_wrong_primary_policy_order_fails_closed(self):
        src = dict(self.sources)
        a = src[ANALYZER]
        src[ANALYZER] = a.replace(
            "compute_mir_primary_selection(", "PRIMARY_REPLACED(", 1
        ).replace("compute_mir_policy_decision(", "compute_mir_primary_selection(", 1
        ).replace("PRIMARY_REPLACED(", "compute_mir_policy_decision(", 1)
        with self.assertRaisesRegex(AuditError, "source stage order drift"):
            audit_sources(src)

    def test_writer_guard_or_embedded_cue_safety_drift_fails_closed(self):
        src = dict(self.sources)
        src[WRITE] = src[WRITE].replace(
            "selection_is_safe(items, &blockedPath", "selection_unchecked(items, &blockedPath", 1)
        with self.assertRaises(AuditError):
            audit_sources(src)
        src = dict(self.sources)
        src[WRITE] = src[WRITE].replace('"flac"', '"not_flac"', 1)
        with self.assertRaisesRegex(AuditError, "virtual-subsong"):
            audit_sources(src)

    def test_candidate_provenance_and_hold_protection_fail_closed(self):
        src = dict(self.sources)
        src[PRIMARY] = src[PRIMARY].replace('"primary_selector"', '"unknown"', 1)
        with self.assertRaisesRegex(AuditError, "provenance"):
            audit_sources(src)
        src = dict(self.sources)
        src[ANALYZER] = src[ANALYZER].replace(
            "mir_writer_invariant_hold", "disabled_writer_invariant", 1)
        with self.assertRaisesRegex(AuditError, "review-hold"):
            audit_sources(src)

    def test_core_rail_default_geometry_and_low_lane_precedence_drift(self):
        cases = (
            (MIR, "constexpr double kMaxBpm = 200.0", "constexpr double kMaxBpm = 350.0"),
            (WINDOW, "kDefaultAnalysisSamplePasses = 50", "kDefaultAnalysisSamplePasses = 6"),
            (PRIMARY, "routeCenter <= 95.0 || routeMax <= 110.0", "routeCenter <= 0.0"),
            (PASS, "std::clamp(offset.startSec, 0.0, maxAllowedStartSec)",
             "std::clamp(offset.startSec, 1.0, maxAllowedStartSec)"),
        )
        for path, old, new in cases:
            with self.subTest(path=path, old=old):
                src = dict(self.sources)
                self.assertIn(old, src[path])
                src[path] = src[path].replace(old, new, 1)
                with self.assertRaises(AuditError):
                    audit_sources(src)

    def test_future_trace_schema_disallows_private_payload_fields(self):
        schema = json.loads((ROOT / "tools" /
                             "smart_tempo_decision_trace_v1.schema.json").read_text(
                                 encoding="utf-8"))
        self.assertEqual(schema["properties"]["schema"]["const"],
                         "smart_tempo_source_decision_trace_v1")
        self.assertFalse(schema["additionalProperties"])
        self.assertGreaterEqual(len(schema["properties"]["stage"]["enum"]), 16)
        for name in ("title", "artist", "genre", "path", "filename",
                     "raw_audio", "oracle_bpm"):
            self.assertNotIn(name, schema["properties"])
        self.assertIn("write_guard", schema["properties"]["stage"]["enum"])
        self.assertIn("review_hold", schema["properties"]["outcome"]["enum"])
        self.assertIn("reason_code", schema["required"])
        self.assertIn("candidate_id", schema["required"])
        self.assertIn("parent_candidate_ids", schema["required"])

    def test_current_inventory_is_explicitly_source_only_not_musical_go(self):
        result = audit_sources(self.sources)
        self.assertEqual(result["claim"],
                         "static_source_structure_only_not_musical_accuracy")
        self.assertGreaterEqual(len(result["unqualified_evidence_gaps"]), 5)
        self.assertIn("internal_50_200_vs_output_40_220_rail_mismatch",
                      result["observed_source_risks"])


if __name__ == "__main__":
    unittest.main()
