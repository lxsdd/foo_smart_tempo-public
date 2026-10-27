"""Native-trace adversarial proof: same real-host codepath and synthetic controls."""
import copy
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import audit_smart_tempo_native_mutations as subject
from audit_smart_tempo_ci_pipeline_trace import TraceError
from test_smart_tempo_ci_pipeline_trace import (
    attach_inventory, empty_board_native_trace, event, trace, with_resource,
)


def empty_with_segment():
    rows = empty_board_native_trace()
    segment = event("mir_segment", None, [], 0., "observed",
                    "insufficient_onsets", 0)
    segment["metrics"] = dict(
        bpm=0., score=0., support=0., segment_index=0,
        odf_peak_count=0, top_k_count=0, candidate=0, aggregated=0)
    rows.insert(1, segment)
    for i, row in enumerate(rows):
        row["sequence"] = i
    return rows


def selected_with_segment():
    rows = with_resource(trace())
    window = event("decode_window", None, [], 0., "observed",
                   "actual_decoded_extent", 0)
    window["metrics"] = dict(
        bpm=0., score=0., support=0., pass_index=0, pass_total=1,
        start_sec=0., requested_frames=16000, decoded_frames=16000,
        analysis_frames=16000, source_rate_hz=16000, analysis_rate_hz=16000)
    segment = event("mir_segment", None, [], 128., "observed",
                    "audacity_mir_full", 0)
    segment["metrics"] = dict(
        bpm=128., score=.9, support=0., segment_index=0,
        odf_peak_count=8, top_k_count=1, candidate=1, aggregated=1)
    rows[0:0] = [window, segment]
    return attach_inventory(rows)


class NativeMutationGateTests(unittest.TestCase):
    def test_negative_no_candidate_host_trace(self):
        rows = empty_with_segment()
        original = copy.deepcopy(rows)
        count, _ = subject.audit(rows, native_comparator=False)
        self.assertGreaterEqual(count, 19)
        self.assertEqual(rows, original)

    def test_positive_measured_candidate_host_trace(self):
        rows = selected_with_segment()
        original = copy.deepcopy(rows)
        count, extra = subject.audit(rows, native_comparator=False)
        self.assertGreaterEqual(count, 23)
        self.assertGreaterEqual(extra, 4)
        self.assertEqual(rows, original)

    def test_fail_closed_if_mutated_trace_were_accepted(self):
        rows = empty_with_segment()
        with patch.object(subject, "validate", return_value={}):
            with patch.object(subject, "reconcile", return_value={}):
                with self.assertRaisesRegex(TraceError, "corruption escaped"):
                    subject.audit(rows, native_comparator=False)

    def test_missing_required_source_is_rejected(self):
        rows = empty_with_segment()
        rows = [row for row in rows if row["stage"] != "source_inventory"]
        with self.assertRaisesRegex(TraceError, "native mutation source missing|missing required native terminal"):
            subject.mutations(rows)

    def test_original_host_checks_exact_product_source_sha(self):
        workflow = (ROOT / ".github" / "workflows" / "source.yml").read_text(
            encoding="utf-8")
        self.assertEqual(workflow.count(
            "ref: ${{ github.event.pull_request.head.sha || github.sha }}"), 1)
        self.assertIn("SMART_TEMPO_EXACT_CHECKOUT_SHA=$actual", workflow)
        self.assertIn("Assert exact checked-out product source SHA", workflow)
        self.assertIn("$actual -cne $expected.ToLowerInvariant()", workflow)

    def test_optional_mutations_are_source_indexed_not_bpm_joined(self):
        rows = selected_with_segment()
        names = {name for name, _ in subject.mutations(rows)}
        self.assertIn("orphan_cluster", names)
        self.assertIn("wrong_disposition_source", names)
        self.assertIn("unknown_raw_origin", names)


if __name__ == "__main__":
    unittest.main()
