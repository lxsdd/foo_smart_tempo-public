"""CI-only native BPM-pipeline lineage observer: strict positive/negative tests."""
from __future__ import annotations
import copy
import json
import math
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from audit_smart_tempo_ci_pipeline_trace import TraceError, load, validate, SCHEMA


def event(stage, cid, parents, bpm, outcome, reason, number):
    return {
        "schema": SCHEMA, "analysis_id": "cafefeed01234567",
        "sequence": number, "stage": stage, "outcome": outcome,
        "reason_code": reason, "candidate_id": cid,
        "parent_candidate_ids": parents,
        "metrics": {"bpm": bpm, "score": .95, "support": .85}
    }


def trace(held=False):
    return [
        event("candidate_create", "c0", [], 128., "observed", "raw_alias", 0),
        event("candidate_transform", "c1", ["c0"], 128., "observed", "cluster_from_raw", 1),
        event("primary_eligibility", "c1", [], 128., "observed", "pool_only_gate_unresolved", 2),
        event("policy_eligibility", "c2", ["c1"], 127.99, "included", "full_board_member", 3),
        event("primary_select", None, ["c1"], 128., "selected", "primary_auto", 4),
        event("policy_change", None, ["c2"], 127.99, "selected", "keep_primary", 5),
        event("final_output", None, [], 0. if held else 127.99,
              "review_hold" if held else "selected",
              "final_abstained" if held else "final_numeric", 6),
        event("write_guard", None, [], 0., "write_blocked", "ci_dry_run", 7)
    ]


class NativePipelineTraceTests(unittest.TestCase):
    def test_positive_exact_lineage_and_unknown_gate_reported_not_hidden(self):
        result = validate(trace())
        self.assertEqual((result["raw"], result["collapsed"], result["primary_pool"],
                          result["policy_board"]), (1, 1, 1, 1))
        self.assertEqual((result["primary_auto"], result["final_accept"],
                          result["write_guard_blocked"]), (1, 1, 1))
        self.assertEqual(result["primary_veto_reason_unknown"], 1)
        self.assertEqual(result["phase_b_causal_completeness"], "INCOMPLETE_PRIMARY_VETO")

    def test_held_result_preserves_no_write_semantics(self):
        actual = trace(held=True)
        self.assertEqual(validate(actual)["final_accept"], 0)
        actual[6]["outcome"] = "selected"
        with self.assertRaises(TraceError):
            validate(actual)

    def test_missing_duplicate_and_reordered_terminal_stages_rejected(self):
        for edit in ("missing", "duplicate", "swap"):
            with self.subTest(edit=edit):
                actual = trace()
                if edit == "missing":
                    actual.pop()
                elif edit == "duplicate":
                    actual.insert(7, copy.deepcopy(actual[6]))
                else:
                    actual[6], actual[7] = actual[7], actual[6]
                with self.assertRaises(TraceError):
                    validate(actual)

    def test_foreign_or_orphan_candidate_ids_fail_closed(self):
        for index, key, wrong in (
            (1, "parent_candidate_ids", ["c99"]),
            (4, "parent_candidate_ids", ["c0"]),
            (2, "candidate_id", "c0"),
            (3, "candidate_id", "c1"),
            (0, "candidate_id", "c4"),
        ):
            with self.subTest(index=index,key=key):
                actual = trace()
                actual[index][key] = wrong
                with self.assertRaises(TraceError):
                    validate(actual)

    def test_unsupported_origin_private_data_or_relabeling_rejected(self):
        for index, field, changed in (
            (0, "title", "private-track"),
            (2, "reason_code", "primary_gate_passed"),
            (3, "reason_code", "tag_from_owner"),
            (4, "stage", "metadata_source"),
            (6, "reason_code", "oracle_label"),
            (7, "reason_code", "write_permitted"),
        ):
            with self.subTest(field=field):
                actual = trace()
                actual[index][field] = changed
                with self.assertRaises(TraceError):
                    validate(actual)

    def test_numeric_nan_bool_and_out_of_rail_rejected(self):
        for value in (float("nan"), float("inf"), -2., 221., "128", True):
            with self.subTest(value=str(value)):
                actual = trace()
                actual[0]["metrics"]["bpm"] = value
                with self.assertRaises(TraceError):
                    validate(actual)
        actual = trace()
        actual[0]["metrics"]["confidence_percent"] = 98.
        with self.assertRaises(TraceError):
            validate(actual)

    def test_numeric_policy_class_and_supplement_must_not_invent_witness(self):
        actual = trace()
        actual[5]["reason_code"] = "writer_override"
        self.assertEqual(validate(actual)["policy_board"], 1)
        actual[3]["parent_candidate_ids"] = []
        actual[3]["reason_code"] = "full_board_origin_unknown"
        r = validate(actual)
        self.assertEqual(r["policy_origin_unknown"], 1)
        self.assertEqual(r["primary_veto_reason_unknown"], 1)

    def test_strict_file_framing_limits_and_source_id(self):
        with tempfile.TemporaryDirectory() as temp:
            p = Path(temp) / "pipeline.jsonl"
            p.write_bytes("".join(json.dumps(x) + "\n" for x in trace()).encode("utf-8"))
            self.assertEqual(validate(load(p))["rows"], 8)
            raw = p.read_text(encoding="utf-8")
            p.write_bytes(raw.rstrip("\n").encode("utf-8"))
            with self.assertRaises(TraceError):
                load(p)
            p.write_bytes(raw.replace(SCHEMA, "private_report", 1).encode("utf-8"))
            with self.assertRaises(TraceError):
                validate(load(p))
            p.write_bytes(raw.replace('"schema": ', '"music_path": "owner", "schema": ', 1).encode("utf-8"))
            with self.assertRaises(TraceError):
                validate(load(p))

    def test_ci_only_compilation_and_host_provenance_chain(self):
        component = (ROOT / "src" / "foo_smart_tempo" /
                     "modern_bpm_analyzer.cpp").read_text(encoding="utf-8")
        header = (ROOT / "tests" / "ci_pipeline_trace.h").read_text(encoding="utf-8")
        workflow = (ROOT / ".github" / "workflows" /
                    "source.yml").read_text(encoding="utf-8")
        host = (ROOT / "tests" / "diagnostic_live_host_smoke.ps1").read_text(
            encoding="utf-8")
        self.assertIn("#ifndef SMART_TEMPO_CI_ORACLE_PROBE", header)
        self.assertIn("#if defined(SMART_TEMPO_CI_ORACLE_PROBE)", component)
        self.assertIn("ci_pipeline_trace::record(", component)
        self.assertIn('L"GITHUB_ACTIONS"', header)
        self.assertIn('L"RUNNER_TEMP"', header)
        self.assertIn('L"SMART_TEMPO_CI_FIXTURE"', header)
        self.assertIn('L"\\synthetic-oracle\\pulse_128.wav"', header.replace('\\\\', '\\'))
        self.assertIn("CREATE_NEW", header)
        self.assertIn("pool_only_gate_unresolved", header)
        self.assertIn("audit_smart_tempo_ci_pipeline_trace.py --trace", host)
        self.assertIn("test_smart_tempo_ci_pipeline_trace.py", workflow)
        self.assertNotRegex(workflow, r"(?m)^\s*uses:\s*actions/upload-artifact@")


if __name__ == "__main__":
    unittest.main()
