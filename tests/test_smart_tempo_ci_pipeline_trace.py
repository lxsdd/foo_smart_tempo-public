"""CI-only native BPM-pipeline lineage observer: strict positive/negative tests."""
from __future__ import annotations
import copy
import json
import math
import re
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from audit_smart_tempo_ci_pipeline_trace import TraceError, load, validate, SCHEMA, ENUMS


def event(stage, cid, parents, bpm, outcome, reason, number):
    return {
        "schema": SCHEMA, "analysis_id": "cafefeed01234567",
        "sequence": number, "stage": stage, "outcome": outcome,
        "reason_code": reason, "candidate_id": cid,
        "parent_candidate_ids": parents,
        "metrics": {"bpm": bpm, "score": .95, "support": .85,
                    **({f"tie_{i}": 0.0 for i in range(4)}
                       if stage == "primary_compare" and
                       reason.startswith(("raw_compare_", "general_baseline_compare_",
                                          "residual_harmonic_compare_",
                                          "harmonic_direct_compare_"))
                       else {})}
    }


def trace(held=False):
    rows = [
        event("candidate_create", "c0", [], 128., "observed", "raw_topk", 0),
        event("candidate_transform", "c1", ["c0"], 128., "observed", "cluster_from_raw", 1),
        event("primary_eligibility", "c1", [], 128., "observed", "pool_only_gate_unresolved", 2),
        event("policy_eligibility", "c2", ["c1"], 127.99, "included", "full_board_member", 3),
        event("primary_select", None, ["c1"], 128., "selected", "primary_auto", 4),
        event("policy_resolution", None, ["c2"], 127.99, "observed",
              "policy_one_same_bpm", 5),
        event("policy_pointer", None, [], 127.99, "observed",
              "pointer_not_observed", 6),
        event("policy_change", None, [], 127.99, "selected", "keep_primary", 7),
        event("final_output", None, [], 0. if held else 127.99,
              "review_hold" if held else "selected",
              "final_abstained" if held else "final_numeric", 8),
        event("measured_witness", None, [] if held else ["c2"],
              0. if held else 127.99,
              "rejected" if held else "included",
              "not_accepted" if held else "indexed_native_witness", 9),
        event("write_guard", None, [], 0., "write_blocked", "ci_dry_run", 10)
    ]
    rows[5]["metrics"]["score"] = 0.
    rows[5]["metrics"]["support"] = 0.
    rows[6]["metrics"]["score"] = 0.
    rows[6]["metrics"]["support"] = 0.
    # The real CI recorder emits a zero-valued write_guard; do not let the
    # generic event helper's placeholder score/support pollute positive fixtures.
    rows[-1]["metrics"]["score"] = 0.
    rows[-1]["metrics"]["support"] = 0.
    return rows


def lineage_trace(raw_bpms, cluster_bpms, cluster_sources):
    """Independent source-only raw/cluster/FullBoard lineage fixture."""
    if not raw_bpms or len(cluster_bpms) != len(cluster_sources):
        raise ValueError("invalid synthetic lineage fixture")
    raw_count = len(raw_bpms)
    cluster_count = len(cluster_bpms)
    policy_id = f"c{raw_count + cluster_count}"
    primary_id = f"c{raw_count}"
    rows = [
        event("candidate_create", f"c{i}", [], bpm,
              "observed", "raw_topk", 0)
        for i, bpm in enumerate(raw_bpms)
    ]
    rows += [
        event("candidate_transform", f"c{raw_count+i}",
              [f"c{j}" for j in sources], bpm,
              "observed", "cluster_from_raw", 0)
        for i, (bpm, sources) in enumerate(zip(cluster_bpms, cluster_sources))
    ]
    rows += [
        event("primary_eligibility", f"c{raw_count+i}", [], bpm,
              "observed", "pool_only_gate_unresolved", 0)
        for i, bpm in enumerate(cluster_bpms)
    ]
    rows.append(event("policy_eligibility", policy_id, [primary_id],
                      127.99, "included", "full_board_member", 0))
    tail = copy.deepcopy(trace()[4:])
    for row in tail:
        row["parent_candidate_ids"] = [
            primary_id if p == "c1" else policy_id if p == "c2" else p
            for p in row["parent_candidate_ids"]
        ]
    rows += tail
    for i, row in enumerate(rows):
        row["sequence"] = i
    return rows


def resource_event():
    item = event("resource_usage", None, [], 0., "observed",
                 "process_resource_snapshot", 0)
    item["metrics"] = {
        "bpm": 0., "score": 0., "support": 0.,
        "wall_seconds": 4.5, "process_cpu_seconds": 3.75,
        "process_peak_rss_before_bytes": 35_000_000,
        "process_peak_rss_after_bytes": 46_000_000,
        "process_working_set_after_bytes": 42_000_000
    }
    return item


def attach_inventory(rows):
    """Synthetic-only source cardinality fixture, not a production measurement."""
    if any(row["stage"] == "source_inventory" for row in rows):
        raise ValueError("duplicate fixture inventory")
    counts = {
        "raw_count": sum(r["stage"] == "candidate_create" for r in rows),
        "collapsed_count": sum(r["stage"] == "candidate_transform" for r in rows),
        "policy_board_count": sum(r["stage"] == "policy_eligibility" for r in rows),
        "selector_event_count": sum(r["stage"] in (
            "candidate_filter", "primary_rank", "primary_compare",
            "primary_terminal") for r in rows),
        "decode_window_count": sum(r["stage"] == "decode_window" for r in rows),
        "supplemental_count": sum(r["stage"] == "supplemental_candidate" for r in rows),
    }
    raw_count = counts["raw_count"]
    counts.update(
        mir_segments_evaluated=max(1, counts["decode_window_count"]),
        mir_segment_results=1 if raw_count else 0,
        mir_topk_rows_seen=raw_count, mir_topk_add_calls=raw_count,
        mir_topk_new_raw=raw_count, mir_family_rank_rows=0,
        mir_alias_add_calls=0, mir_alias_new_raw=0,
        mir_continuous_add_calls=0, mir_continuous_new_raw=0,
    )
    row = event("source_inventory", None, [], 0., "observed",
                "native_count_conservation", 0)
    row["metrics"] = dict(bpm=0., score=0., support=0., **counts)
    rows.append(row)
    for i, item in enumerate(rows):
        item["sequence"] = i
    return rows


def with_resource(rows):
    # Host-valid minimal selector lineage: the native recorder emits these
    # three ordered snapshots before primary_select for a chosen BPM.
    primary_index = next(i for i, row in enumerate(rows)
                         if row["stage"] == "primary_select")
    primary = rows[primary_index]
    if primary["metrics"]["bpm"] > 0:
        for reason in ("terminal_lane_choice", "terminal_residual_choice",
                       "terminal_final_choice"):
            rows.insert(primary_index, event(
                "primary_terminal", primary["parent_candidate_ids"][0], [],
                primary["metrics"]["bpm"], "selected", reason, 0))
            primary_index += 1
    # The native recorder now emits a disposition for each actual
    # collapsed candidate. This helper models the one-candidate positive
    # synthetic host fixture, not the production scoring path.
    selected_cid = primary["parent_candidate_ids"][0]
    for i, row in enumerate(rows):
        if row["stage"] == "primary_select":
            rows.insert(i, event("primary_disposition", selected_cid, [],
                                 primary["metrics"]["bpm"], "observed",
                                 "selected_final_object", 0))
            rows[i]["metrics"]["score"] = 0.
            rows[i]["metrics"]["support"] = 0.
            break
    rows.insert(-1, resource_event())
    for i, row in enumerate(rows):
        row["sequence"] = i
    return rows


def empty_board_native_trace():
    """Independent negative fixture: actual windows, no candidate objects."""
    w = event("decode_window", None, [], 0., "observed",
              "actual_decoded_extent", 0)
    w["metrics"] = {
        "bpm": 0., "score": 0., "support": 0.,
        "pass_index": 0, "pass_total": 1, "start_sec": 0.,
        "requested_frames": 16000, "decoded_frames": 16000,
        "analysis_frames": 16000, "source_rate_hz": 16000,
        "analysis_rate_hz": 16000,
    }
    rows = [
        w,
        event("primary_select", None, [], 0., "no_candidate",
              "primary_no_candidate", 1),
        event("policy_resolution", None, [], 0., "observed",
              "policy_no_output", 2),
        event("policy_pointer", None, [], 0., "observed",
              "pointer_no_output", 3),
        event("policy_change", None, [], 0., "review_hold",
              "no_policy_write", 4),
        event("final_output", None, [], 0., "review_hold",
              "final_abstained", 5),
        event("measured_witness", None, [], 0., "rejected",
              "not_accepted", 6),
        resource_event(),
        event("write_guard", None, [], 0., "write_blocked",
              "ci_dry_run", 8),
    ]
    for row in rows:
        if row["stage"] not in ("decode_window", "resource_usage"):
            row["metrics"] = {"bpm": 0., "score": 0., "support": 0.}
    for i, row in enumerate(rows):
        row["sequence"] = i
    return attach_inventory(rows)


class NativePipelineTraceTests(unittest.TestCase):

    def test_actual_mir_evaluator_first_return_conservation_and_mutations(self):
        # This fixture checks the native evaluator boundary, NOT musical GO.
        # The existing synthetic 1-window/1-candidate trace is reused.
        rows = with_resource(trace())
        w = event("decode_window", None, [], 0., "observed",
                  "actual_decoded_extent", 0)
        w["metrics"] = dict(bpm=0., score=0., support=0.,
                            pass_index=0, pass_total=1, start_sec=0.,
                            requested_frames=16000, decoded_frames=16000,
                            analysis_frames=16000, source_rate_hz=16000,
                            analysis_rate_hz=16000)
        seg = event("mir_segment", None, [], 128., "observed",
                    "audacity_mir_full", 0)
        seg["metrics"] = dict(bpm=128., score=.9, support=0.,
                              segment_index=0, odf_peak_count=8,
                              top_k_count=1, candidate=1, aggregated=1)
        rows[0:0] = [w, seg]
        attach_inventory(rows)
        self.assertEqual(validate(rows, require_windows=True,
                                  require_segments=True)["mir_segment_native_verified"], 1)
        mutations = (
            ("unknown_reason", lambda x: x.update(reason_code="unknown")),
            ("candidate_disagreement", lambda x: x["metrics"].update(candidate=0)),
            ("invented_topk", lambda x: x["metrics"].update(top_k_count=2)),
            ("fractional_index", lambda x: x["metrics"].update(segment_index=.5)),
            ("foreign_path", lambda x: x.update(path="private.wav")),
            ("bad_peak_count", lambda x: x["metrics"].update(odf_peak_count=2)),
            ("bad_aggregated", lambda x: x["metrics"].update(aggregated=0)),
        )
        for name, mutate in mutations:
            bad = copy.deepcopy(rows)
            mutate(bad[1])
            with self.subTest(name=name), self.assertRaises(TraceError):
                validate(bad, require_windows=True, require_segments=True)
        missing = [x for x in rows if x["stage"] != "mir_segment"]
        for i, x in enumerate(missing): x["sequence"] = i
        with self.assertRaises(TraceError):
            validate(missing, require_windows=True, require_segments=True)

        empty = empty_board_native_trace()
        reason = event("mir_segment", None, [], 0., "observed",
                       "insufficient_onsets", 0)
        reason["metrics"] = dict(bpm=0., score=0., support=0.,
                                 segment_index=0, odf_peak_count=0,
                                 top_k_count=0, candidate=0, aggregated=0)
        empty.insert(1, reason)
        for i, x in enumerate(empty): x["sequence"] = i
        self.assertEqual(validate(empty, require_windows=True,
                                  require_segments=True)["mir_segment_reason_counts"],
                         "insufficient_onsets:1")
        for field, value in (("odf_peak_count", 3), ("candidate", 1),
                             ("top_k_count", 1), ("aggregated", 1)):
            bad = copy.deepcopy(empty)
            bad[1]["metrics"][field] = value
            with self.subTest(field=field), self.assertRaises(TraceError):
                validate(bad, require_windows=True, require_segments=True)


    def test_native_source_supply_origin_conservation_and_negative_mutations(self):
        positive = attach_inventory(with_resource(trace()))
        self.assertEqual(validate(positive)["raw"], 1)
        for reason, new, calls in (
            ("raw_pulse_alias", (0, 1, 0), (0, 1, 0)),
            ("raw_continuous", (0, 0, 1), (0, 0, 1)),
            ("raw_all_sources", (1, 0, 0), (1, 1, 1)),
        ):
            rows = copy.deepcopy(positive)
            next(r for r in rows if r["stage"] == "candidate_create")["reason_code"] = reason
            m = rows[-1]["metrics"]
            m["mir_topk_new_raw"], m["mir_alias_new_raw"], m["mir_continuous_new_raw"] = new
            m["mir_topk_add_calls"], m["mir_alias_add_calls"], m["mir_continuous_add_calls"] = calls
            m["mir_topk_rows_seen"] = max(1, calls[0])
            m["mir_family_rank_rows"] = int(bool(calls[1] or calls[2]))
            self.assertEqual(validate(rows)["raw"], 1, reason)
        for key, value in (
            ("mir_topk_new_raw", 0), ("mir_topk_add_calls", 0),
            ("mir_segments_evaluated", 0), ("mir_segment_results", 2),
            ("mir_alias_add_calls", 1), ("mir_continuous_new_raw", 1),
            ("mir_topk_rows_seen", 0),
        ):
            rows = copy.deepcopy(positive)
            rows[-1]["metrics"][key] = value
            with self.assertRaises(TraceError, msg=key):
                validate(rows)
        rows = copy.deepcopy(positive)
        next(r for r in rows if r["stage"] == "candidate_create")["reason_code"] = "raw_unproven"
        with self.assertRaises(TraceError):
            validate(rows)
        empty = empty_board_native_trace()
        self.assertEqual(validate(empty, require_windows=True)["raw"], 0)
        for key in ("mir_topk_new_raw", "mir_alias_new_raw", "mir_continuous_new_raw"):
            rows = copy.deepcopy(empty)
            rows[-1]["metrics"][key] = 1
            with self.assertRaises(TraceError):
                validate(rows, require_windows=True)

    def test_native_empty_board_negative_chain_is_verified_but_not_qualified(self):
        rows = empty_board_native_trace()
        result = validate(rows, require_windows=True)
        self.assertEqual((result["raw"], result["collapsed"],
                          result["primary_auto"], result["final_accept"],
                          result["write_guard_blocked"]), (0, 0, 0, 0, 1))
        self.assertEqual(result["negative_no_candidate_trace"], 1)
        self.assertEqual(result["native_source_inventory_verified"], 1)
        self.assertEqual(result["native_empty_board_boundary_verified"], 1)
        self.assertEqual(result["phase_b_causal_completeness"],
                         "INCOMPLETE_EMPTY_BOARD_CAUSAL")
        rows[1]["outcome"], rows[1]["reason_code"] = (
            "review_hold", "primary_review_hold")
        self.assertEqual(validate(rows, require_windows=True)["final_accept"], 0)

    def test_native_empty_board_forgery_matrix_fails_closed(self):
        base = empty_board_native_trace()
        for index, field, value in (
            (1, "outcome", "selected"),
            (2, "reason_code", "policy_one_same_bpm"),
            (3, "reason_code", "pointer_not_observed"),
            (4, "outcome", "selected"),
            (5, "outcome", "selected"),
            (6, "outcome", "included"),
            (8, "outcome", "selected"),
        ):
            with self.subTest(index=index, field=field):
                rows = copy.deepcopy(base)
                rows[index][field] = value
                with self.assertRaises(TraceError):
                    validate(rows, require_windows=True)
        for index, key, value in (
            (1, "bpm", 128.0), (2, "score", 1.0),
            (3, "support", 1.0), (4, "bpm", 128.0),
            (5, "bpm", 128.0), (6, "bpm", 128.0),
            (8, "score", 1.0),
        ):
            with self.subTest(index=index, metric=key):
                rows = copy.deepcopy(base)
                rows[index]["metrics"][key] = value
                with self.assertRaises(TraceError):
                    validate(rows, require_windows=True)
        for removed in (0, 7, 8):
            rows = copy.deepcopy(base)
            rows.pop(removed)
            for i, row in enumerate(rows):
                row["sequence"] = i
            with self.subTest(removed=removed), self.assertRaises(TraceError):
                validate(rows, require_windows=True)


    def test_positive_exact_lineage_and_unknown_gate_reported_not_hidden(self):
        result = validate(trace())
        self.assertEqual((result["raw"], result["collapsed"], result["primary_pool"],
                          result["policy_board"]), (1, 1, 1, 1))
        self.assertEqual((result["primary_auto"], result["final_accept"],
                          result["write_guard_blocked"]), (1, 1, 1))
        self.assertEqual(result["primary_veto_reason_unknown"], 1)
        self.assertEqual(result["phase_b_causal_completeness"], "INCOMPLETE_PRIMARY_VETO")

    def test_primary_outcome_reason_pairs_are_not_interchangeable(self):
        for outcome, reason in (("selected", "primary_review_hold"),
                                ("review_hold", "primary_auto"),
                                ("no_candidate", "primary_auto"),
                                ("rejected", "primary_auto")):
            with self.subTest(outcome=outcome, reason=reason):
                rows = trace()
                rows[4]["outcome"] = outcome
                rows[4]["reason_code"] = reason
                with self.assertRaises(TraceError):
                    validate(rows)
        rows = trace()
        rows[4]["metrics"]["bpm"] = 0.
        with self.assertRaises(TraceError):
            validate(rows)

    def test_policy_flags_and_action_must_match_both_directions(self):
        # Native reason precedence: override > hold release > keep > no write.
        for override, release, outcome, reason, should_pass in (
            (0, 0, "selected", "keep_primary", True),
            (1, 0, "selected", "writer_override", True),
            (0, 1, "selected", "hold_release", True),
            (1, 1, "selected", "writer_override", True),
            (0, 0, "selected", "writer_override", False),
            (0, 0, "selected", "hold_release", False),
            (1, 0, "selected", "keep_primary", False),
            (0, 1, "selected", "keep_primary", False),
            (1, 1, "selected", "hold_release", False),
            (0, 0, "selected", "no_policy_write", False),
            (0, 0, "review_hold", "no_policy_write", True),
        ):
            with self.subTest(override=override, release=release,
                              outcome=outcome, reason=reason):
                rows = trace()
                rows[5]["metrics"]["score"] = float(override)
                rows[5]["metrics"]["support"] = float(release)
                rows[7]["outcome"] = outcome
                rows[7]["reason_code"] = reason
                if should_pass:
                    self.assertEqual(validate(rows)["policy_override_active"],
                                     override)
                else:
                    with self.assertRaises(TraceError):
                        validate(rows)

    def test_write_guard_rejects_forged_numeric_evidence(self):
        for metric in ("bpm", "score", "support"):
            with self.subTest(metric=metric):
                rows = trace()
                rows[-1]["metrics"][metric] = 1.
                with self.assertRaises(TraceError):
                    validate(rows)

    def test_host_primary_requires_executed_terminal_chain(self):
        rows = with_resource(trace())
        window = event("decode_window", None, [], 0., "observed",
                       "actual_decoded_extent", 0)
        window["metrics"] = dict(
            bpm=0., score=0., support=0., pass_index=0, pass_total=1,
            start_sec=0., requested_frames=32000, decoded_frames=32000,
            analysis_frames=32000, source_rate_hz=16000, analysis_rate_hz=16000)
        rows.insert(0, window)
        for i, row in enumerate(rows):
            row["sequence"] = i
        attach_inventory(rows)
        self.assertEqual(validate(rows, require_windows=True)["selector_terminal_observations"], 3)
        for missing in ("terminal_lane_choice", "terminal_residual_choice",
                        "terminal_final_choice"):
            with self.subTest(missing=missing):
                bad = copy.deepcopy(rows)
                bad = [row for row in bad if row["reason_code"] != missing]
                for i, row in enumerate(bad):
                    row["sequence"] = i
                with self.assertRaises(TraceError):
                    validate(bad, require_windows=True)
        no_terminal = [row for row in rows if row["stage"] not in
                       ("primary_terminal", "primary_disposition", "source_inventory")]
        for i, row in enumerate(no_terminal):
            row["sequence"] = i
        self.assertEqual(validate(no_terminal)["selector_terminal_observations"], 0)
        with self.assertRaises(TraceError):
            validate(no_terminal, require_windows=True)

    def test_held_result_preserves_no_write_semantics(self):
        actual = trace(held=True)
        self.assertEqual(validate(actual)["final_accept"], 0)
        actual[8]["outcome"] = "selected"
        with self.assertRaises(TraceError):
            validate(actual)

    def test_missing_duplicate_and_reordered_terminal_stages_rejected(self):
        for edit in ("missing", "duplicate", "swap"):
            with self.subTest(edit=edit):
                actual = trace()
                if edit == "missing":
                    actual.pop()
                elif edit == "duplicate":
                    actual.insert(9, copy.deepcopy(actual[8]))
                else:
                    actual[8], actual[9] = actual[9], actual[8]
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
            (8, "reason_code", "oracle_label"),
            (10, "reason_code", "write_permitted"),
        ):
            with self.subTest(field=field):
                actual = trace()
                actual[index][field] = changed
                with self.assertRaises(TraceError):
                    validate(actual)

    def test_numeric_nan_bool_and_out_of_rail_rejected(self):
        for value in (float("nan"), float("inf"), -2., 2001., "128", True):
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
        actual[7]["reason_code"] = "writer_override"
        actual[5]["metrics"]["score"] = 1.
        self.assertEqual(validate(actual)["policy_board"], 1)
        actual[3]["parent_candidate_ids"] = []
        actual[3]["reason_code"] = "full_board_origin_unknown"
        with self.assertRaises(TraceError):
            validate(actual)

    def test_executed_selector_gate_events_are_joined_to_raw_or_cluster(self):
        events = trace()
        gates = [
            event("candidate_filter", "c0", [], 128., "included",
                  "raw_alias_allowed", 0),
            event("candidate_filter", "c0", [], 128., "excluded",
                  "raw_support_provisional_exclusion", 0),
            event("primary_rank", "c0", [], 128., "included",
                  "raw_rank_scored", 0),
            event("candidate_filter", "c1", [], 128., "included",
                  "general_in_rail", 0),
            event("primary_rank", "c1", [], 128., "included",
                  "harmonic_rank_scored", 0),
        ]
        events[4:4] = gates
        for i, row in enumerate(events):
            row["sequence"] = i
        outcome = validate(events)
        self.assertEqual((outcome["selector_branch_checks"],
                          outcome["selector_rank_observations"]), (3, 2))
        self.assertEqual(outcome["primary_veto_reason_unknown"], 1)
        for broken in ("wrong_source", "unknown_reason", "invalid_outcome"):
            with self.subTest(broken=broken):
                rows = copy.deepcopy(events)
                if broken == "wrong_source":
                    rows[4]["candidate_id"] = "c1"
                if broken == "unknown_reason":
                    rows[4]["reason_code"] = "unvalidated_mir_rescue"
                if broken == "invalid_outcome":
                    rows[4]["outcome"] = "review_hold"
                with self.assertRaises(TraceError):
                    validate(rows)

    def test_executed_fallback_readmission_requires_prior_exclusion(self):
        rows = trace()
        first = event("candidate_filter", "c1", [], 128., "excluded",
                      "low_threshold_provisional_exclusion", 0)
        second = event("candidate_filter", "c1", [], 128., "included",
                       "low_threshold_fallback_readmitted", 0)
        rescue = event("candidate_filter", "c1", [], 128., "included",
                       "residual_high_gate_allowed", 0)
        rank = event("primary_rank", "c1", [], 128., "included",
                     "low_rank_scored", 0)
        compare = event("primary_compare", "c1", [], 128., "included",
                        "low_compare_won", 0)
        rank["metrics"]["low_pool_epoch"] = 1
        compare["metrics"]["low_pool_epoch"] = 1
        rows[4:4] = [first, second, rescue, rank, compare]
        for i, row in enumerate(rows):
            row["sequence"] = i
        report = validate(rows)
        self.assertEqual(report["selector_fallback_readmissions"], 1)
        self.assertEqual(report["selector_nested_rescue_checks"], 1)
        for kind in ("missing_exclusion", "wrong_order", "wrong_readmission_outcome",
                     "foreign_readmission_candidate", "unknown_rescue"):
            with self.subTest(kind=kind):
                broken = copy.deepcopy(rows)
                if kind == "missing_exclusion":
                    broken.pop(4)
                elif kind == "wrong_order":
                    broken[4], broken[5] = broken[5], broken[4]
                elif kind == "wrong_readmission_outcome":
                    broken[5]["outcome"] = "excluded"
                elif kind == "foreign_readmission_candidate":
                    broken[5]["candidate_id"] = "c0"
                else:
                    broken[6]["reason_code"] = "residual_unverified_guess"
                for i, row in enumerate(broken):
                    row["sequence"] = i
                with self.assertRaises(TraceError):
                    validate(broken)

    def test_actual_lane_residual_final_terminal_chain(self):
        rows = trace()
        terminal = [
            event("primary_terminal", "c1", [], 128., "selected",
                  "terminal_lane_choice", 0),
            event("primary_terminal", "c1", [], 128., "selected",
                  "terminal_residual_choice", 0),
            event("primary_terminal", "c1", [], 128., "selected",
                  "terminal_final_choice", 0),
        ]
        rows[4:4] = terminal
        for i, row in enumerate(rows):
            row["sequence"] = i
        self.assertEqual(validate(rows)["selector_terminal_observations"], 3)
        for corruption in ("missing", "reordered", "wrong_identity"):
            with self.subTest(corruption=corruption):
                bad = copy.deepcopy(rows)
                if corruption == "missing":
                    bad.pop(5)
                elif corruption == "reordered":
                    bad[4], bad[5] = bad[5], bad[4]
                else:
                    bad[4]["candidate_id"] = "c0"
                with self.assertRaises(TraceError):
                    validate(bad)

    def test_primary_terminal_winner_must_be_exact_object_not_equal_bpm(self):
        rows = trace()
        # Two collapsed candidates can share identical BPM; the actual native
        # final pointer, not a match on bpm text, chooses the winner.
        # Keep staged creator IDs and terminal/event ordering internally sound.
        # A simple falsification is to assign an unrelated primary identity
        # while retaining the exactly equal BPM and valid known object.
        terminal = [
            event("primary_terminal", "c1", [], 128., "selected",
                  "terminal_lane_choice", 0),
            event("primary_terminal", "c1", [], 128., "selected",
                  "terminal_residual_choice", 0),
            event("primary_terminal", "c1", [], 128., "selected",
                  "terminal_final_choice", 0)
        ]
        rows[4:4] = terminal
        for i, row in enumerate(rows):
            row["sequence"] = i
        self.assertEqual(validate(rows)["selector_terminal_observations"], 3)
        rows[7]["parent_candidate_ids"] = ["c2"]  # same ~BPM, policy object != primary object
        with self.assertRaisesRegex(TraceError, "matching clustered witness|exact terminal object"):
            validate(rows)

    def test_native_lane_terminal_must_descend_from_actual_pool_winner(self):
        # This closes the gap between individually valid comparator rows and
        # a forged terminal lane ID. No reference BPM or private audio enters
        # selection; the independent oracle uses only executed source IDs.
        families = {
            "raw": ("c0", "c1", "raw_rank_scored"),
            "harmonic": ("c2", "c3", "harmonic_rank_scored"),
            "general_baseline": ("c2", "c3", "general_baseline_rank_scored"),
            "general_alt": ("c2", "c3", "general_alt_rank_scored"),
            "low": ("c2", "c3", "low_rank_scored"),
        }

        def case(family, second_wins=False, continuous_rescue=False):
            first, second, rank_reason = families[family]
            rows = lineage_trace([128., 140.], [128., 140.],
                                 [[0], [1]])
            insert_at = next(i for i, row in enumerate(rows)
                             if row["stage"] == "primary_select")
            observed = []
            for idx, (cid, bpm) in enumerate(((first, 128.), (second, 140.))):
                score = 1.0 if idx == 0 else (2.0 if second_wins else .5)
                won = idx == 0 or second_wins
                rank = event("primary_rank", cid, [], bpm, "included",
                             rank_reason, 0)
                compare = event("primary_compare", cid, [], bpm,
                                "included" if won else "excluded",
                                family + ("_compare_won" if won else "_compare_lost"), 0)
                for row in (rank, compare):
                    row["metrics"]["score"] = score
                    row["metrics"]["support"] = 0.
                    if family == "low":
                        row["metrics"]["low_pool_epoch"] = 1
                observed.extend((rank, compare))
            if continuous_rescue:
                observed.append(event("candidate_filter", "c2", [], 128.,
                                      "included", "low_continuous_allowed", 0))
            observed.extend(
                event("primary_terminal", "c2", [], 128., "selected", reason, 0)
                for reason in ("terminal_lane_choice", "terminal_residual_choice",
                               "terminal_final_choice"))
            rows[insert_at:insert_at] = observed
            for idx, row in enumerate(rows):
                row["sequence"] = idx
            return rows

        for family in families:
            with self.subTest(family=family):
                positive = validate(case(family), require_native_comparator=True)
                self.assertEqual(positive["selector_lane_comparator_winner_proven"], 1)
                self.assertEqual(positive["selector_lane_continuous_rescue_unranked"], 0)
                # A legitimate final comparator replacement must invalidate
                # the stale lane pointer even if its BPM and object still exist.
                with self.assertRaisesRegex(
                        TraceError, "terminal lane candidate was not"):
                    validate(case(family, second_wins=True),
                             require_native_comparator=True)

        # The old gate-only continuous fixture must NOT pass as genuine
        # original-host winner provenance after the five-tuple upgrade.
        with self.assertRaisesRegex(TraceError, "terminal lane candidate was not"):
            validate(case("low", second_wins=True, continuous_rescue=True),
                     require_native_comparator=True)
        rescue = validate(case("low", second_wins=True, continuous_rescue=True))
        self.assertEqual(rescue["selector_lane_comparator_winner_proven"], 0)
        self.assertEqual(rescue["selector_lane_continuous_rescue_unranked"], 1)
        forged = case("low", second_wins=True, continuous_rescue=True)
        next(x for x in forged if x["reason_code"] == "low_continuous_allowed")[
            "outcome"] = "excluded"
        with self.assertRaises(TraceError):
            validate(forged, require_native_comparator=True)
        # Unit-only window/terminal fixtures remain independently testable;
        # the actual original-host CLI always demands executed comparisons.
        no_comparator = case("raw")
        no_comparator = [row for row in no_comparator if row["stage"] not in
                         ("primary_rank", "primary_compare")]
        for i, row in enumerate(no_comparator):
            row["sequence"] = i
        self.assertEqual(validate(no_comparator)["selector_terminal_observations"], 3)
        with self.assertRaisesRegex(TraceError, "no executed comparator"):
            validate(no_comparator, require_native_comparator=True)

    def test_raw_aliased_candidates_can_exceed_output_rail_but_not_safety_limit(self):
        # A 512 BPM raw alias may be present without becoming a 40-220 BPM
        # policy candidate. Never launder it through an unknown source.
        rows = lineage_trace([128., 512.], [128., 512.], [[0], [1]])
        result = validate(rows)
        self.assertEqual(result["raw"], 2)
        self.assertEqual(result["policy_board_filtered_clusters"], 1)
        rows[1]["metrics"]["bpm"] = 2001.
        with self.assertRaises(TraceError):
            validate(rows)

    def test_native_first_match_raw_cluster_partition_and_overlap(self):
        # c2 lies within 0.001 BPM of BOTH clusters. Actual native collapse
        # assigns it to the first, never to both by approximate join.
        rows = lineage_trace([128., 128.0015, 128.0008],
                             [128., 128.0015], [[0, 2], [1]])
        result = validate(rows)
        self.assertEqual(result["raw_cluster_partition_verified"], 3)
        self.assertEqual(result["raw_lineage_overlap_disambiguated"], 1)
        self.assertEqual(result["policy_board_unique_cluster_sources"], 1)
        for mutation in ("wrong_parent", "missing_parent", "duplicate_parent",
                         "reordered_parent", "wrong_representative"):
            bad = copy.deepcopy(rows)
            cluster = bad[3]
            if mutation == "wrong_parent":
                cluster["parent_candidate_ids"] = ["c0", "c1"]
            elif mutation == "missing_parent":
                cluster["parent_candidate_ids"] = ["c0"]
            elif mutation == "duplicate_parent":
                cluster["parent_candidate_ids"] = ["c0", "c2", "c2"]
            elif mutation == "reordered_parent":
                cluster["parent_candidate_ids"] = ["c2", "c0"]
            else:
                cluster["metrics"]["bpm"] = 128.0005
            with self.subTest(mutation=mutation), self.assertRaises(TraceError):
                validate(bad)

    def test_fullboard_source_bijection_and_no_fabricated_unknown_origin(self):
        rows = lineage_trace([128., 140.], [128., 140.], [[0], [1]])
        position = next(i for i, row in enumerate(rows)
                        if row["stage"] == "primary_select")
        rows.insert(position, event("policy_eligibility", "c5", ["c3"],
                                    140., "included", "full_board_member", 0))
        for i, row in enumerate(rows):
            row["sequence"] = i
        result = validate(rows)
        self.assertEqual(result["policy_board_unique_cluster_sources"], 2)
        self.assertEqual(result["policy_board_filtered_clusters"], 0)
        for mutation in ("duplicate_source", "unknown_source", "orphan_source",
                         "missing_source"):
            bad = copy.deepcopy(rows)
            second = next(x for x in bad if x["candidate_id"] == "c5")
            if mutation == "duplicate_source":
                second["parent_candidate_ids"] = ["c2"]
            elif mutation == "unknown_source":
                second["reason_code"] = "full_board_origin_unknown"
            elif mutation == "orphan_source":
                second["parent_candidate_ids"] = ["c99"]
            else:
                second["parent_candidate_ids"] = []
            with self.subTest(mutation=mutation), self.assertRaises(TraceError):
                validate(bad)

    def test_native_first_match_source_contract_is_ci_only(self):
        header = (ROOT / "tests" / "ci_pipeline_trace.h").read_text(encoding="utf-8")
        source = (ROOT / "src" / "foo_smart_tempo" /
                  "mir_candidate_pipeline.cpp").read_text(encoding="utf-8")
        self.assertIn("raw_cluster_sources", header)
        self.assertIn("raw_cluster_sources[assigned].push_back(raw_index)", header)
        self.assertIn("fullboard_source_seen", header)
        self.assertIn("if (parents.size() != 1) return reject_trace_at(__LINE__);", header)
        self.assertIn("collapse_mir_candidates(", source)
        self.assertIn("#ifndef SMART_TEMPO_CI_ORACLE_PROBE", header)

    def test_complete_nested_low_pool_and_general_alt_ci_source_gates(self):
        source = (ROOT / "src/foo_smart_tempo/mir_candidate_pipeline.cpp").read_text(
            encoding="utf-8")
        low = source.split("select_mir_low_pulse(", 1)[1].split(
            "select_mir_low_pulse_continuous_center_rescue(", 1)[0]
        gates = ["low_mid_58_61","low_lower_mid_58_61","low_midlow_56_58","low_deeplow_64_67","low_lower_mid_79_81","low_upper_direct_86_90","low_verylow_50_53","low_upper_recovery_50_53"]
        for gate in gates:
            with self.subTest(gate=gate):
                self.assertEqual(low.count('"' + gate + '_allowed"'), 1)
                self.assertEqual(low.count('"' + gate + '_provisional_exclusion"'), 1)
        self.assertEqual(low.count("const bool ciLowEligible = ("), len(gates))
        self.assertEqual(source.count('"raw_alias_fallback_readmitted"'), 1)
        self.assertEqual(source.count('"raw_support_fallback_readmitted"'), 1)
        self.assertEqual(source.count('"general_alt_window_allowed"'), 1)
        self.assertIn("#if defined(SMART_TEMPO_CI_ORACLE_PROBE)", low)

    def test_executed_nested_gates_and_raw_fallback_fail_closed(self):
        low_gates = ["low_mid_58_61","low_lower_mid_58_61","low_midlow_56_58","low_deeplow_64_67","low_lower_mid_79_81","low_upper_direct_86_90","low_verylow_50_53","low_upper_recovery_50_53"]
        for gate in low_gates + ["general_alt_window"]:
            for verdict in ("allowed", "provisional_exclusion"):
                with self.subTest(gate=gate, verdict=verdict):
                    rows = trace()
                    reason = gate + "_" + verdict
                    outcome = "included" if verdict == "allowed" else "excluded"
                    rows.insert(4, event("candidate_filter", "c1", [], 128.,
                                         outcome, reason, 0))
                    for i, row in enumerate(rows):
                        row["sequence"] = i
                    self.assertEqual(validate(rows)["selector_branch_checks"], 1)
                    bad = copy.deepcopy(rows)
                    bad[4]["outcome"] = "excluded" if outcome == "included" else "included"
                    with self.assertRaises(TraceError):
                        validate(bad)
                    bad = copy.deepcopy(rows)
                    bad[4]["candidate_id"] = "c0"
                    with self.assertRaises(TraceError):
                        validate(bad)
        for prefix in ("raw_alias", "raw_support"):
            rows = trace()
            rows[4:4] = [
                event("candidate_filter", "c0", [], 128., "excluded",
                      prefix + "_provisional_exclusion", 0),
                event("candidate_filter", "c0", [], 128., "included",
                      prefix + "_fallback_readmitted", 0),
            ]
            for i, row in enumerate(rows):
                row["sequence"] = i
            self.assertEqual(validate(rows)["selector_fallback_readmissions"], 1)
            for mutation in ("missing_exclusion", "wrong_order", "wrong_source"):
                with self.subTest(prefix=prefix, mutation=mutation):
                    bad = copy.deepcopy(rows)
                    if mutation == "missing_exclusion":
                        bad.pop(4)
                    elif mutation == "wrong_order":
                        bad[4], bad[5] = bad[5], bad[4]
                    else:
                        bad[5]["candidate_id"] = "c1"
                    for i, row in enumerate(bad):
                        row["sequence"] = i
                    with self.assertRaises(TraceError):
                        validate(bad)

    def test_jsonl_duplicate_keys_rejected_even_inside_nested_metrics(self):
        with tempfile.TemporaryDirectory() as temp:
            p = Path(temp) / "pipeline.jsonl"
            baseline = "".join(json.dumps(x) + "\n" for x in trace())
            p.write_bytes(baseline.encode("utf-8"))
            self.assertEqual(validate(load(p))["rows"], 11)
            for original, duplicate in (
                ('"schema": ', '"schema": "forged", "schema": '),
                ('"metrics": {', '"metrics": {"bpm": 666, '),
            ):
                with self.subTest(original=original):
                    p.write_bytes(baseline.replace(original, duplicate, 1).encode("utf-8"))
                    with self.assertRaises(TraceError):
                        load(p)

    def test_low_gate_predicates_are_exact_source_parity_not_shadow_tuning(self):
        source = (ROOT / "src/foo_smart_tempo/mir_candidate_pipeline.cpp").read_text(
            encoding="utf-8")
        low = source.split("select_mir_low_pulse(", 1)[1].split(
            "select_mir_low_pulse_continuous_center_rescue(", 1)[0]
        gates = ["low_mid_58_61", "low_lower_mid_58_61", "low_midlow_56_58",
                 "low_deeplow_64_67", "low_lower_mid_79_81",
                 "low_upper_direct_86_90", "low_verylow_50_53",
                 "low_upper_recovery_50_53"]

        def balanced(text, opening):
            depth = 0
            for index in range(opening, len(text)):
                if text[index] == "(":
                    depth += 1
                elif text[index] == ")":
                    depth -= 1
                    if depth == 0:
                        return text[opening + 1:index]
            raise AssertionError("unterminated source condition")

        def compare(text):
            for gate in gates:
                marker = text.index('"' + gate + '_allowed"')
                start = text.rfind("const bool ciLowEligible = (", 0, marker)
                if start < 0:
                    return False
                guard = balanced(text, text.index("(", start))
                production = text.index("if (", marker)
                real = balanced(text, text.index("(", production))
                if re.sub(r"\s+", "", guard) != re.sub(r"\s+", "", real):
                    return False
            return True

        self.assertTrue(compare(low))
        mutated = low.replace("candidate.support >= 0.45",
                              "candidate.support >= 0.46", 1)
        self.assertFalse(compare(mutated))

    def test_strict_file_framing_limits_and_source_id(self):
        with tempfile.TemporaryDirectory() as temp:
            p = Path(temp) / "pipeline.jsonl"
            p.write_bytes("".join(json.dumps(x) + "\n" for x in trace()).encode("utf-8"))
            self.assertEqual(validate(load(p))["rows"], 11)
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

    def test_final_bpm_witness_falsification_and_unindexed_distinction(self):
        rows = trace()
        self.assertEqual(validate(rows)["indexed_native_final_witness"], 1)
        for mutation in ("missing", "unrelated_value", "orphan", "fake_indexed",
                         "fake_unindexed", "wrong_outcome"):
            with self.subTest(mutation=mutation):
                modified = copy.deepcopy(rows)
                if mutation == "missing":
                    modified.pop(9)
                elif mutation == "unrelated_value":
                    modified[8]["metrics"]["bpm"] = 167.9
                elif mutation == "orphan":
                    modified[9]["parent_candidate_ids"] = ["c99"]
                elif mutation == "fake_indexed":
                    modified[9]["parent_candidate_ids"] = ["c0"]
                elif mutation == "fake_unindexed":
                    modified[9]["reason_code"] = "supplemental_witness_unindexed"
                else:
                    modified[9]["outcome"] = "rejected"
                with self.assertRaises(TraceError):
                    validate(modified)
        # A genuine supplemental candidate is not assigned a fake FullBoard
        # candidate ID: preserve explicit incomplete causal attribution.
        rows[9]["parent_candidate_ids"] = []
        rows[9]["reason_code"] = "supplemental_witness_unindexed"
        result = validate(rows)
        self.assertEqual(result["indexed_native_final_witness"], 0)
        self.assertEqual(result["unindexed_supplemental_final_witness"], 1)

    def test_run_local_id_must_match_every_event_and_have_hex_shape(self):
        rows = trace()
        for row in rows:
            row["analysis_id"] = "19a2b3c4d5e6f708"
        self.assertEqual(validate(rows)["raw"], 1)
        rows[4]["analysis_id"] = "cafefeed01234567"
        with self.assertRaises(TraceError):
            validate(rows)
        rows[4]["analysis_id"] = "19a2b3c4d5e6f708"
        rows[0]["analysis_id"] = "REUSED_CONSTANT"
        with self.assertRaises(TraceError):
            validate(rows)


    def test_native_comparator_chronology_positive_and_adversarial(self):
        base = with_resource(trace())
        insert_at = next(i for i, row in enumerate(base)
                         if row["stage"] == "primary_disposition")
        rank = event("primary_rank", "c1", [], 128., "included",
                     "general_baseline_rank_scored", 0)
        comparison = event("primary_compare", "c1", [], 128., "included",
                           "general_baseline_compare_won", 0)
        base[insert_at:insert_at] = [rank, comparison]
        for i, row in enumerate(base):
            row["sequence"] = i
        result = validate(base)
        self.assertEqual(result["selector_comparator_observations"], 1)
        self.assertEqual(result["selector_comparator_families"], 1)
        for kind in ("orphan", "wrong_id", "wrong_score", "wrong_family",
                     "first_loses", "bad_outcome", "unapproved_reason"):
            with self.subTest(kind=kind):
                bad = copy.deepcopy(base)
                comp = next(row for row in bad if row["stage"] == "primary_compare")
                if kind == "orphan":
                    bad = [row for row in bad if row["stage"] != "primary_rank"]
                elif kind == "wrong_id":
                    comp["candidate_id"] = "c0"
                elif kind == "wrong_score":
                    comp["metrics"]["score"] += .01
                elif kind == "wrong_family":
                    comp["reason_code"] = "harmonic_compare_won"
                elif kind == "first_loses":
                    comp["reason_code"] = "general_baseline_compare_lost"
                    comp["outcome"] = "excluded"
                elif kind == "bad_outcome":
                    comp["outcome"] = "excluded"
                else:
                    comp["reason_code"] = "fabricated_compare_won"
                for i, row in enumerate(bad):
                    row["sequence"] = i
                with self.assertRaises(TraceError):
                    validate(bad)

    def test_native_comparator_source_is_compile_only_and_all_four_branches_instrumented(self):
        source = (ROOT / "src/foo_smart_tempo/mir_candidate_pipeline.cpp").read_text(
            encoding="utf-8")
        reporter = (ROOT / "tests/ci_pipeline_trace.h").read_text(encoding="utf-8")
        for family in ("raw", "harmonic", "general_baseline", "general_alt", "low"):
            self.assertEqual(source.count('"' + family + '_compare_won"'), 1)
            self.assertEqual(source.count('"' + family + '_compare_lost"'), 1)
        self.assertIn('comparison ? "primary_compare"', reporter)
        self.assertIn('#if defined(SMART_TEMPO_CI_ORACLE_PROBE)', source)

    def test_all_four_comparator_families_and_outcome_denominators(self):
        rows = with_resource(trace())
        insert_at = next(i for i, row in enumerate(rows)
                         if row["stage"] == "primary_disposition")
        events = []
        families = ("raw", "harmonic", "general_baseline", "general_alt", "low")
        for family in families:
            candidate = "c0" if family == "raw" else "c1"
            for score, verdict in ((.95, "won"), (.90, "lost")):
                rank = event("primary_rank", candidate, [], 128., "included",
                             family + "_rank_scored", 0)
                comp = event("primary_compare", candidate, [], 128.,
                             "included" if verdict == "won" else "excluded",
                             family + "_compare_" + verdict, 0)
                rank["metrics"]["score"] = score
                comp["metrics"]["score"] = score
                if family == "low":
                    rank["metrics"]["low_pool_epoch"] = 1
                    comp["metrics"]["low_pool_epoch"] = 1
                events.extend((rank, comp))
        rows[insert_at:insert_at] = events
        for index, row in enumerate(rows):
            row["sequence"] = index
        result = validate(rows)
        self.assertEqual(result["selector_comparator_observations"], 10)
        self.assertEqual(result["selector_comparator_families"], 5)
        self.assertEqual(result["selector_low_bestby_episodes"], 1)
        self.assertEqual((result["selector_comparator_wins"],
                          result["selector_comparator_losses"]), (5, 5))
        for family in families:
            self.assertEqual(result["selector_compare_" + family + "_wins"], 1)
            self.assertEqual(result["selector_compare_" + family + "_losses"], 1)
        for family in families:
            for mutation in ("higher_lost", "lower_won", "missing_rank"):
                with self.subTest(family=family, mutation=mutation):
                    bad = copy.deepcopy(rows)
                    matches = [x for x in bad if x["stage"] == "primary_compare"
                               and x["reason_code"].startswith(family + "_compare_")]
                    second = matches[1]
                    if mutation == "higher_lost":
                        second["metrics"]["score"] = 1.20
                        # Preserve rank/comparator equality to isolate chronology.
                        prior = bad[bad.index(second) - 1]
                        prior["metrics"]["score"] = 1.20
                    elif mutation == "lower_won":
                        second["reason_code"] = family + "_compare_won"
                        second["outcome"] = "included"
                    else:
                        bad.remove(bad[bad.index(second) - 1])
                    for index, row in enumerate(bad):
                        row["sequence"] = index
                    with self.assertRaises(TraceError):
                        validate(bad)


    def test_executed_five_field_low_continuous_center_winner_and_adversaries(self):
        # Two distinct native collapsed source IDs, second candidate beats
        # first ONLY on the fifth, center-distance, tuple field.
        rows = lineage_trace([128., 130.], [128., 130.], [[0], [1]])
        at = next(i for i, r in enumerate(rows)
                  if r["stage"] == "primary_select")
        events = []
        for cid, bpm, negative_distance in (
                ("c3", 130., -4.), ("c2", 128., -2.)):
            allowed = event("candidate_filter", cid, [], bpm, "included",
                            "low_continuous_allowed", 0)
            rank = event("primary_rank", cid, [], bpm, "included",
                         "low_continuous_rank_scored", 0)
            comp = event("primary_compare", cid, [], bpm, "included",
                         "low_continuous_compare_won", 0)
            for r in (rank, comp):
                r["metrics"]["score"] = 1.9
            comp["metrics"].update(
                tie_0=1.9, tie_1=.85, tie_2=.78, tie_3=.5,
                tie_4=negative_distance)
            events.extend((allowed, rank, comp))
        events += [
            event("primary_terminal", "c2", [], 128., "selected",
                  "terminal_lane_choice", 0),
            event("primary_terminal", "c2", [], 128., "selected",
                  "terminal_residual_choice", 0),
            event("primary_terminal", "c2", [], 128., "selected",
                  "terminal_final_choice", 0),
            event("primary_disposition", "c2", [], 128., "observed",
                  "selected_final_object", 0),
            event("primary_disposition", "c3", [], 130., "observed",
                  "ranked_not_final", 0),
        ]
        for item in events:
            if item["stage"] == "primary_disposition":
                item["metrics"]["score"] = 0.
                item["metrics"]["support"] = 0.
        rows[at:at] = events
        for i, item in enumerate(rows):
            item["sequence"] = i
        result = validate(rows, require_native_comparator=True)
        self.assertEqual(result["selector_compare_low_continuous_wins"], 2)
        self.assertEqual(result["selector_lane_comparator_winner_proven"], 1)
        self.assertEqual(result["selector_lane_continuous_rescue_unranked"], 0)
        self.assertEqual(result["phase_b_causal_completeness"],
                         "INCOMPLETE_PRIMARY_VETO")
        for mutation in ("missing_gate", "false_second_lost", "false_second_won",
                         "missing_fifth", "extra_sixth", "five_tuple_nan",
                         "wrong_source", "swapped_terminal", "missing_rank",
                         "gate_excluded", "wrong_score"):
            with self.subTest(mutation=mutation):
                bad = copy.deepcopy(rows)
                comps = [x for x in bad if x["stage"] == "primary_compare"]
                ranks = [x for x in bad if x["stage"] == "primary_rank"]
                gates = [x for x in bad if x["stage"] == "candidate_filter"]
                terminals = [x for x in bad if x["stage"] == "primary_terminal"]
                if mutation == "missing_gate":
                    bad.remove(gates[0])
                elif mutation == "false_second_lost":
                    comps[1]["reason_code"] = "low_continuous_compare_lost"
                    comps[1]["outcome"] = "excluded"
                elif mutation == "false_second_won":
                    comps[1]["metrics"]["tie_4"] = -5.
                elif mutation == "missing_fifth":
                    del comps[1]["metrics"]["tie_4"]
                elif mutation == "extra_sixth":
                    comps[1]["metrics"]["tie_5"] = .1
                elif mutation == "five_tuple_nan":
                    comps[1]["metrics"]["tie_4"] = math.inf
                elif mutation == "wrong_source":
                    comps[1]["candidate_id"] = "c3"
                elif mutation == "swapped_terminal":
                    terminals[0]["candidate_id"] = "c3"
                elif mutation == "missing_rank":
                    bad.remove(ranks[0])
                elif mutation == "gate_excluded":
                    gates[1]["reason_code"] = "low_continuous_provisional_exclusion"
                    gates[1]["outcome"] = "excluded"
                else:
                    ranks[1]["metrics"]["score"] = 1.8
                for i, r in enumerate(bad):
                    r["sequence"] = i
                with self.assertRaises(TraceError):
                    validate(bad, require_native_comparator=True)

    def test_continuous_five_field_ci_tuple_source_parity(self):
        cpp = (ROOT / "src/foo_smart_tempo/mir_candidate_pipeline.cpp").read_text(
            encoding="utf-8")
        sub = cpp.split("select_mir_low_pulse_continuous_center_rescue(", 1)[1]
        sub = sub.split("select_mir_harmonic_soft(", 1)[0]
        def normal(fragment):
            return re.sub(r"\s+", "", fragment)
        # Independent source contract: exactly the SAME ordered five
        # productive tuple operands; no hidden CI shadow selector tuning.
        candidate_fields = (
            "candidate.continuous_score_ratio",
            "candidate.continuous_section_support",
            "candidate.continuous_section_stability",
            "candidate.continuous_score",
            "-std::abs(candidate.bpm - routeCenter)",
        )
        incumbent_fields = tuple(
            field.replace("candidate.", "best->") for field in candidate_fields
        )
        for candidate, needle in (
            (candidate_fields, "const auto ciCandidateTuple = std::make_tuple("),
            (incumbent_fields, ": std::make_tuple(")):
            where = sub.index(needle)
            tail = sub[where:where + 550]
            for token in candidate:
                self.assertIn(token, tail)
        # Build canonical source expressions without looking at a BPM oracle.
        def source_tuples(content):
            tuples = []
            for match in re.finditer(r"std::make_tuple\(", content):
                opening = content.index("(", match.start())
                depth = 0
                for at in range(opening, len(content)):
                    depth += (content[at] == "(") - (content[at] == ")")
                    if depth == 0:
                        tuples.append(normal(content[opening+1:at]))
                        break
                else:
                    raise AssertionError("unclosed native lexicographic tuple")
            return tuples
        native = source_tuples(sub)
        self.assertEqual(len(native), 4)
        # First two CI-only tuples MUST equal the following two production
        # comparison tuples, field-for-field and in the same order.
        self.assertEqual(native[0], native[2])
        self.assertEqual(native[1], native[3])
        for poison in (
            sub.replace("candidate.continuous_section_stability,",
                        "candidate.continuous_section_stability + .1,", 1),
            sub.replace("best->continuous_score,",
                        "best->continuous_score + .01,", 1),
        ):
            mutated = source_tuples(poison)
            self.assertTrue(mutated[0] != mutated[2] or
                            mutated[1] != mutated[3])
        self.assertIn("ciCandidateTuple > ciBestTuple", sub)
        self.assertIn("if (best == nullptr ||", sub)
        self.assertIn("std::get<4>(ciCandidateTuple)", sub)
        self.assertIn('"low_continuous_compare_won"', sub)
        self.assertIn('"low_continuous_compare_lost"', sub)
        for fragment in ("candidate.continuous_score_ratio",
                         "candidate.continuous_section_support",
                         "candidate.continuous_section_stability",
                         "candidate.continuous_score",
                         "-std::abs(candidate.bpm - routeCenter)"):
            self.assertGreaterEqual(sub.count(fragment), 2)
        # Mutations to the CI predicate itself are caught by the structural
        # source contract, even if the productive expression is unchanged.
        fake = sub.replace("ciCandidateTuple > ciBestTuple",
                           "ciCandidateTuple < ciBestTuple", 1)
        self.assertNotIn("ciCandidateTuple > ciBestTuple", fake)

    def test_nested_low_bestby_epoch_reset_replay_and_poisoning(self):
        rows = with_resource(trace())
        i = next(i for i, row in enumerate(rows)
                 if row["stage"] == "primary_disposition")
        additions = []
        for epoch in (1, 2, 3):
            for score, verdict in ((.9, "won"), (.7, "lost")):
                rank = event("primary_rank", "c1", [], 128., "included",
                             "low_rank_scored", 0)
                comp = event("primary_compare", "c1", [], 128.,
                             "included" if verdict == "won" else "excluded",
                             "low_compare_" + verdict, 0)
                rank["metrics"].update(score=score, low_pool_epoch=epoch)
                comp["metrics"].update(score=score, low_pool_epoch=epoch)
                additions.extend((rank, comp))
        rows[i:i] = additions
        for n, row in enumerate(rows):
            row["sequence"] = n
        result = validate(rows)
        self.assertEqual(result["selector_low_bestby_episodes"], 3)
        self.assertEqual(result["selector_comparator_families"], 1)
        self.assertEqual((result["selector_comparator_wins"],
                          result["selector_comparator_losses"]), (3, 3))
        poison = ("no_reset", "missing_rank_epoch", "wrong_comparator_epoch",
                  "pool_reentry", "first_comp_lost", "fractional_epoch",
                  "zero_epoch", "duplicate_missing_compare", "reversed_score")
        for corruption in poison:
            with self.subTest(corruption=corruption):
                bad = copy.deepcopy(rows)
                rank = [x for x in bad if x["stage"] == "primary_rank"]
                comp = [x for x in bad if x["stage"] == "primary_compare"]
                if corruption == "no_reset":
                    comp[2]["reason_code"] = "low_compare_lost"
                    comp[2]["outcome"] = "excluded"
                elif corruption == "missing_rank_epoch":
                    del rank[2]["metrics"]["low_pool_epoch"]
                elif corruption == "wrong_comparator_epoch":
                    comp[3]["metrics"]["low_pool_epoch"] = 3
                elif corruption == "pool_reentry":
                    for x in (rank[4], rank[5], comp[4], comp[5]):
                        x["metrics"]["low_pool_epoch"] = 1
                elif corruption == "first_comp_lost":
                    comp[0]["reason_code"] = "low_compare_lost"
                    comp[0]["outcome"] = "excluded"
                elif corruption == "fractional_epoch":
                    rank[0]["metrics"]["low_pool_epoch"] = 1.5
                elif corruption == "zero_epoch":
                    rank[0]["metrics"]["low_pool_epoch"] = 0
                elif corruption == "duplicate_missing_compare":
                    bad.remove(comp[1])
                else:
                    rank[1]["metrics"]["score"] = 1.2
                    comp[1]["metrics"]["score"] = 1.2
                for n, row in enumerate(bad):
                    row["sequence"] = n
                with self.assertRaises(TraceError):
                    validate(bad)

    def test_exact_native_raw_and_baseline_tie_tuple_replay(self):
        # The independent scorer must reproduce lexicographic tie decisions,
        # including a slightly lower score within the production epsilon.
        for family in ("raw", "general_baseline"):
            with self.subTest(family=family):
                rows = with_resource(trace())
                insert_at = next(i for i, row in enumerate(rows)
                                 if row["stage"] == "primary_terminal")
                cid = "c0" if family == "raw" else "c1"
                scenarios = (
                    (1.0, (.7, .5, .4, -3.), True),
                    (1.0, (.7, .6, .1, -3.), True),
                    (1.0, (.7, .6, .0, -3.), False),
                    (1.0 - 5e-10, (.8, .1, .1, -2.), True),
                )
                inserted = []
                for score, tie, won in scenarios:
                    rank = event("primary_rank", cid, [], 128., "included",
                                 family + "_rank_scored", 0)
                    comp = event("primary_compare", cid, [], 128.,
                                 "included" if won else "excluded",
                                 family + ("_compare_won" if won else "_compare_lost"), 0)
                    rank["metrics"].update(score=score, support=0.)
                    comp["metrics"].update(score=score, support=0.)
                    for index, value in enumerate(tie):
                        comp["metrics"][f"tie_{index}"] = value
                    inserted.extend((rank, comp))
                rows[insert_at:insert_at] = inserted
                for i, row in enumerate(rows):
                    row["sequence"] = i
                result = validate(rows)
                self.assertEqual((result["selector_comparator_wins"],
                                  result["selector_comparator_losses"]), (3, 1))
                for mutation in ("wrong_winner_tie", "wrong_loser_tie",
                                 "missing_tie", "extra_tie", "nonfinite_tie",
                                 "equal_tuple_promoted", "score_too_low"):
                    with self.subTest(mutation=mutation):
                        bad = copy.deepcopy(rows)
                        comps = [x for x in bad if x["stage"] == "primary_compare"]
                        if mutation == "wrong_winner_tie":
                            comps[1]["metrics"]["tie_1"] = .1
                        elif mutation == "wrong_loser_tie":
                            comps[2]["metrics"]["tie_2"] = .2
                        elif mutation == "missing_tie":
                            del comps[0]["metrics"]["tie_0"]
                        elif mutation == "extra_tie":
                            comps[0]["metrics"]["tie_4"] = 1.
                        elif mutation == "nonfinite_tie":
                            comps[0]["metrics"]["tie_2"] = math.inf
                        elif mutation == "equal_tuple_promoted":
                            comps[1]["metrics"]["tie_1"] = .5
                            comps[1]["metrics"]["tie_2"] = .4
                        else:
                            comps[3]["metrics"]["score"] = .9
                            bad[bad.index(comps[3]) - 1]["metrics"]["score"] = .9
                        with self.assertRaises(TraceError):
                            validate(bad)

    def test_first_executed_cluster_exclusion_is_not_final_veto(self):
        rows = with_resource(trace())
        insert_at = next(i for i, row in enumerate(rows)
                         if row["stage"] == "primary_terminal")
        events = [
            event("candidate_filter", "c0", [], 128., "excluded",
                  "raw_alias_provisional_exclusion", 0),
            event("candidate_filter", "c1", [], 128., "excluded",
                  "general_out_of_rail_provisional", 0),
            event("candidate_filter", "c1", [], 128., "included",
                  "general_rail_fallback_readmitted", 0),
            event("primary_rank", "c1", [], 128., "included",
                  "general_alt_rank_scored", 0),
            event("primary_compare", "c1", [], 128., "included",
                  "general_alt_compare_won", 0),
        ]
        rows[insert_at:insert_at] = events
        for i, row in enumerate(rows):
            row["sequence"] = i
        result = validate(rows)
        self.assertEqual(result["selector_cluster_gate_checked"], 1)
        self.assertEqual(result["selector_cluster_gate_unvisited"], 0)
        self.assertEqual(result["selector_cluster_first_exclusion_observed"], 1)
        self.assertEqual(result["selector_cluster_excluded_then_ranked"], 1)
        self.assertEqual(result["selector_cluster_excluded_then_readmitted"], 1)
        self.assertEqual(result["selector_cluster_first_exclusion_reasons"],
                         "general_out_of_rail_provisional:1")
        self.assertEqual(result["primary_veto_reason_unknown"], 1)
        self.assertEqual(result["phase_b_causal_completeness"],
                         "INCOMPLETE_PRIMARY_VETO")

    def test_tie_tuple_source_guard_and_reporter_contract(self):
        source = (ROOT / "src/foo_smart_tempo/mir_candidate_pipeline.cpp").read_text(
            encoding="utf-8")
        reporter = (ROOT / "tests/ci_pipeline_trace.h").read_text(encoding="utf-8")
        header = (ROOT / "src/foo_smart_tempo/mir_candidate_pipeline.h").read_text(
            encoding="utf-8")
        self.assertEqual(source.count("std::get<0>(tie)"), 2)
        self.assertEqual(source.count("std::get<3>(tie)"), 2)
        self.assertEqual(source.count("tieMetrics, hasTieMetrics"), 1)
        self.assertIn("bool has_tie_metrics = false", header)
        self.assertIn("if (item.has_tie_metrics != tieExpected) return reject_trace_at(__LINE__)", reporter)
        for i in range(4):
            self.assertIn('"tie_' + str(i), reporter)

    def test_ci_comparator_shadow_is_guarded_and_matches_production(self):
        source = (ROOT / "src/foo_smart_tempo/mir_candidate_pipeline.cpp").read_text(
            encoding="utf-8")
        families = ("raw", "general_baseline", "harmonic", "general_alt", "low")
        pattern = re.compile(
            r"#if defined\(SMART_TEMPO_CI_ORACLE_PROBE\)\s+"
            r"const bool ciComparisonWon =\s*(.*?);\s*#endif\s*if\s*\(",
            re.DOTALL)

        def verify(content):
            matches = list(pattern.finditer(content))
            self.assertEqual(len(matches), 5)
            observed = set()
            for match in matches:
                pos = match.end() - 1
                depth = 0
                end = None
                for index in range(pos, len(content)):
                    char = content[index]
                    if char == "(":
                        depth += 1
                    elif char == ")":
                        depth -= 1
                        if depth == 0:
                            end = index
                            break
                self.assertIsNotNone(end)
                ci_expression = "".join(match.group(1).split())
                production_expression = "".join(content[pos + 1:end].split())
                self.assertEqual(ci_expression, production_expression)
                tail = content[end:end + 450]
                candidates = [f for f in families
                              if '"' + f + '_compare_won"' in tail
                              and '"' + f + '_compare_lost"' in tail]
                self.assertEqual(len(candidates), 1)
                observed.add(candidates[0])
                self.assertIn('ciComparisonWon ? "' + candidates[0] +
                              '_compare_won"', tail)
            self.assertEqual(observed, set(families))

        verify(source)
        # Independent negative mutations must fail before the costly SDK build.
        mutations = (
            source.replace("const bool ciComparisonWon = best == nullptr || score > bestScore;",
                           "const bool ciComparisonWon = best == nullptr || score < bestScore;", 1),
            source.replace("#endif\n    if (best == nullptr || score > bestScore) {",
                           "#endif\n    if (best == nullptr || score < bestScore) {", 1),
            source.replace("#if defined(SMART_TEMPO_CI_ORACLE_PROBE)\n    const bool ciComparisonWon",
                           "#if 0\n    const bool ciComparisonWon", 1),
            source.replace('ciComparisonWon ? "harmonic_compare_won"',
                           'false ? "harmonic_compare_won"', 1),
        )
        for mutation in mutations:
            self.assertNotEqual(mutation, source)
            with self.assertRaises(AssertionError):
                verify(mutation)

    def test_per_candidate_disposition_recomputed_from_executed_events(self):
        rows = with_resource(trace())
        self.assertEqual(validate(rows)["selector_disposition_observations"], 1)
        disposition = next(x for x in rows if x["stage"] == "primary_disposition")
        for reason in ("ranked_not_final", "observed_not_ranked",
                       "no_cluster_observation"):
            bad = copy.deepcopy(rows)
            next(x for x in bad if x["stage"] == "primary_disposition")["reason_code"] = reason
            with self.subTest(reason=reason), self.assertRaises(TraceError):
                validate(bad)
        missing = [x for x in rows if x["stage"] != "primary_disposition"]
        for i, row in enumerate(missing):
            row["sequence"] = i
        # Legacy short unit traces are allowed, but native host qualification isn't.
        self.assertEqual(validate(missing)["selector_disposition_observations"], 0)
        window = event("decode_window", None, [], 0., "observed",
                       "actual_decoded_extent", 0)
        window["metrics"] = dict(bpm=0., score=0., support=0.,
                                pass_index=0, pass_total=1, start_sec=0.,
                                requested_frames=16000, decoded_frames=16000,
                                analysis_frames=16000, source_rate_hz=16000,
                                analysis_rate_hz=16000)
        missing.insert(0, window)
        for i, row in enumerate(missing):
            row["sequence"] = i
        with self.assertRaises(TraceError):
            validate(missing, require_windows=True)

    def test_actual_decoder_window_coverage_and_overlap(self):
        rows = with_resource(trace())
        windows = []
        for index, start in enumerate((0.0, 0.5, 1.0)):
            row = event("decode_window", None, [], 0., "observed",
                        "actual_decoded_extent", 0)
            row["metrics"] = dict(bpm=0., score=0., support=0.,
                pass_index=index, pass_total=3, start_sec=start,
                requested_frames=32000, decoded_frames=32000,
                analysis_frames=32000, source_rate_hz=16000,
                analysis_rate_hz=16000)
            windows.append(row)
        rows[:0] = windows
        for index, row in enumerate(rows):
            row["sequence"] = index
        attach_inventory(rows)
        result = validate(rows, require_windows=True)
        self.assertEqual((result["decoded_windows"], result["declared_windows"],
                          result["windows_complete"]), (3, 3, 1))
        self.assertEqual(result["requested_window_seconds"], 6.)
        self.assertEqual(result["decoded_window_seconds"], 6.)
        self.assertEqual(result["unique_covered_seconds"], 3.)
        self.assertEqual(result["repeat_factor"], 2.)

    def test_window_capture_missing_partial_and_out_of_order_fail_closed(self):
        rows = with_resource(trace())
        with self.assertRaises(TraceError):
            validate(rows, require_windows=True)
        row = event("decode_window", None, [], 0., "observed",
                    "actual_decoded_extent", 0)
        row["metrics"] = dict(bpm=0., score=0., support=0.,
            pass_index=0, pass_total=2, start_sec=1.5,
            requested_frames=32000, decoded_frames=24000,
            analysis_frames=24000, source_rate_hz=16000,
            analysis_rate_hz=16000)
        rows.insert(0, row)
        for index, item in enumerate(rows):
            item["sequence"] = index
        attach_inventory(rows)
        self.assertEqual(validate(rows)["windows_complete"], 0)
        with self.assertRaises(TraceError):
            validate(rows, require_windows=True)
        rows[0]["metrics"]["pass_total"] = 1
        self.assertEqual(validate(rows, require_windows=True)["decoded_windows"], 1)
        rows[0]["metrics"]["pass_index"] = 1
        with self.assertRaises(TraceError):
            validate(rows)

    def test_decoder_window_adversarial_numeric_and_privacy_inputs(self):
        rows = with_resource(trace())
        row = event("decode_window", None, [], 0., "observed",
                    "actual_decoded_extent", 0)
        row["metrics"] = dict(bpm=0., score=0., support=0.,
            pass_index=0, pass_total=1, start_sec=0.,
            requested_frames=32000, decoded_frames=32000,
            analysis_frames=32000, source_rate_hz=16000,
            analysis_rate_hz=16000)
        rows.insert(0, row)
        for index, item in enumerate(rows):
            item["sequence"] = index
        attach_inventory(rows)
        self.assertEqual(validate(rows, require_windows=True)["windows_complete"], 1)
        for key, value in (("decoded_frames", 32001), ("analysis_frames", 50000),
                           ("source_rate_hz", 0), ("analysis_rate_hz", 96000),
                           ("pass_index", .5), ("start_sec", -1.),
                           ("start_sec", float("nan")), ("requested_frames", True),
                           ("pass_total", 129), ("score", .5)):
            with self.subTest(key=key, value=value):
                bad = copy.deepcopy(rows)
                bad[0]["metrics"][key] = value
                with self.assertRaises(TraceError):
                    validate(bad, require_windows=True)
        for field, value in (("file_path", "private.wav"),
                             ("candidate_id", "c0"),
                             ("parent_candidate_ids", ["c0"])):
            with self.subTest(field=field):
                bad = copy.deepcopy(rows)
                bad[0][field] = value
                with self.assertRaises(TraceError):
                    validate(bad, require_windows=True)

    def test_policy_same_bpm_comparison_never_proves_chosen_pointer(self):
        baseline = trace()
        result = validate(baseline)
        self.assertEqual(result["policy_output_source_comparison"], "policy_one_same_bpm")
        self.assertEqual(result["policy_pointer_identity_complete"], 0)
        self.assertEqual(baseline[6]["parent_candidate_ids"], [])
        # Two distinct collapsed sources may independently refine to the
        # SAME exact BPM. A duplicated FullBoard row from one cluster cannot
        # simulate this genuine ambiguity under the stricter identity gate.
        doubled = lineage_trace([128., 140.], [128., 140.], [[0], [1]])
        position = next(i for i, row in enumerate(doubled)
                        if row["stage"] == "primary_select")
        doubled.insert(position, event("policy_eligibility", "c5", ["c3"], 127.99,
                                       "included", "full_board_member", 0))
        for i, row in enumerate(doubled):
            row["sequence"] = i
        resolution = next(row for row in doubled
                          if row["stage"] == "policy_resolution")
        resolution["reason_code"] = "policy_multiple_same_bpm"
        resolution["parent_candidate_ids"] = []
        self.assertEqual(validate(doubled)["policy_output_source_comparison"],
                         "policy_multiple_same_bpm")
        bad = copy.deepcopy(doubled)
        forged = next(row for row in bad
                      if row["stage"] == "policy_resolution")
        forged["reason_code"] = "policy_one_same_bpm"
        forged["parent_candidate_ids"] = ["c4"]
        with self.assertRaises(TraceError):
            validate(bad)
        bad = trace()
        bad[5]["reason_code"] = "policy_no_fullboard_match"
        bad[5]["parent_candidate_ids"] = []
        with self.assertRaises(TraceError):
            validate(bad)

    def test_policy_override_and_release_flag_independence(self):
        for override, release in ((1., 0.), (0., 1.), (1., 1.)):
            with self.subTest(override=override, release=release):
                rows = trace()
                rows[5]["metrics"]["score"] = override
                rows[5]["metrics"]["support"] = release
                rows[7]["reason_code"] = "writer_override" if override else "hold_release"
                measured = validate(rows)
                self.assertEqual(measured["policy_override_active"], int(override))
                self.assertEqual(measured["policy_hold_release_active"], int(release))
        invalid = trace()
        invalid[5]["metrics"]["score"] = 2.
        with self.assertRaises(TraceError):
            validate(invalid)

    def test_native_policy_object_pointer_distinguishes_numeric_coincidence(self):
        rows = trace()
        self.assertEqual(validate(rows)["policy_pointer_identity_complete"], 0)
        rows[6]["reason_code"] = "pointer_native_fullboard_consistent"
        rows[6]["parent_candidate_ids"] = ["c2"]
        rows[6]["metrics"]["score"] = 1.
        rows[6]["metrics"]["support"] = 1.
        rows[7]["parent_candidate_ids"] = ["c2"]
        qualified = validate(rows)
        self.assertEqual(qualified["policy_pointer_identity_complete"], 1)
        self.assertEqual(qualified["policy_pointer_reason"],
                         "pointer_native_fullboard_consistent")
        for change in ("bad_parent", "invented_pointer", "different_policy", "foreign_id"):
            with self.subTest(change=change):
                bad = copy.deepcopy(rows)
                if change == "bad_parent":
                    bad[6]["parent_candidate_ids"] = ["c0"]
                elif change == "invented_pointer":
                    bad[6]["metrics"]["score"] = 0.
                elif change == "different_policy":
                    bad[7]["parent_candidate_ids"] = []
                else:
                    bad[6]["parent_candidate_ids"] = ["c99"]
                with self.assertRaises(TraceError):
                    validate(bad)

    def test_unobserved_supplemental_policy_cannot_invent_fullboard_pointer(self):
        rows = trace()
        rows[6]["reason_code"] = "pointer_supplemental_or_changed"
        rows[6]["metrics"]["score"] = 1.
        self.assertEqual(validate(rows)["policy_pointer_identity_complete"], 0)
        bad = copy.deepcopy(rows)
        bad[6]["parent_candidate_ids"] = ["c2"]
        with self.assertRaises(TraceError):
            validate(bad)

    def test_resource_provenance_is_process_scoped_and_fail_closed(self):
        rows = with_resource(trace())
        self.assertEqual(validate(rows)["process_cpu_seconds"], 3.75)
        with self.assertRaises(TraceError):
            validate(trace(), require_windows=True)
        cases = (
            ("wall_seconds", -1.), ("wall_seconds", float("nan")),
            ("process_cpu_seconds", float("inf")),
            ("process_cpu_seconds", 2000.),
            ("process_peak_rss_after_bytes", 1),
            ("process_peak_rss_after_bytes", 4.5),
            ("process_working_set_after_bytes", 47_000_000),
            ("score", 1.), ("path", "private")
        )
        for key, value in cases:
            with self.subTest(key=key):
                bad = copy.deepcopy(rows)
                bad[-2]["metrics"][key] = value
                with self.assertRaises(TraceError):
                    validate(bad)
        bad = copy.deepcopy(rows)
        bad.insert(-1, resource_event())
        for i, row in enumerate(bad):
            row["sequence"] = i
        with self.assertRaises(TraceError):
            validate(bad)

    def test_supplemental_selected_object_identity_rejects_bpm_substitution(self):
        rows = trace()
        rows.insert(4, event("supplemental_candidate", "c3", [], 127.99,
                             "observed", "partial_bar_topk_source", 0))
        rows[7]["reason_code"] = "pointer_native_supplemental_consistent"
        rows[7]["parent_candidate_ids"] = ["c3"]
        rows[7]["metrics"]["score"] = 1.
        rows[8]["parent_candidate_ids"] = ["c3"]
        rows[10]["parent_candidate_ids"] = ["c3"]
        for i, row in enumerate(rows):
            row["sequence"] = i
        result = validate(rows)
        self.assertEqual(result["policy_supplemental_pointer_identity_complete"], 1)
        self.assertEqual(result["supplemental_selected_sources"], 1)
        self.assertEqual(result["indexed_supplemental_final_witness"], 1)
        for key in ("wrong_id", "lost_source", "wrong_board_flag",
                    "wrong_bpm", "foreign_kind", "duplicate",
                    "witness_source_lost", "witness_source_forged",
                    "unindexed_selected_source"):
            with self.subTest(key=key):
                bad = copy.deepcopy(rows)
                if key == "wrong_id":
                    bad[7]["parent_candidate_ids"] = ["c2"]
                    bad[8]["parent_candidate_ids"] = ["c2"]
                elif key == "lost_source": bad.pop(4)
                elif key == "wrong_board_flag": bad[7]["metrics"]["support"] = 1.
                elif key == "wrong_bpm": bad[4]["metrics"]["bpm"] = 140.
                elif key == "foreign_kind": bad[4]["reason_code"] = "reference_bpm_source"
                elif key == "duplicate":
                    duplicate = copy.deepcopy(bad[4])
                    duplicate["candidate_id"] = "c4"
                    bad.insert(5, duplicate)
                elif key == "witness_source_lost":
                    bad[10]["parent_candidate_ids"] = ["c2"]
                elif key == "unindexed_selected_source":
                    bad[10]["parent_candidate_ids"] = []
                    bad[10]["reason_code"] = "supplemental_witness_unindexed"
                elif key == "witness_source_forged":
                    bad[10]["parent_candidate_ids"] = ["c2", "c3"]
                    bad[7]["reason_code"] = "pointer_not_observed"
                    bad[7]["parent_candidate_ids"] = []
                    bad[7]["metrics"]["score"] = 0.
                    bad[8]["parent_candidate_ids"] = []
                for i, row in enumerate(bad):
                    row["sequence"] = i
                with self.assertRaises(TraceError):
                    validate(bad)

    def test_fullboard_pointer_cannot_launder_supplemental_final_witness(self):
        rows = trace()
        rows.insert(4, event("supplemental_candidate", "c3", [], 127.99,
                             "observed", "partial_bar_topk_source", 0))
        rows[10]["parent_candidate_ids"] = ["c2", "c3"]
        for i, row in enumerate(rows):
            row["sequence"] = i
        with self.assertRaises(TraceError):
            validate(rows)

    def test_native_inventory_conserves_input_and_executed_counts(self):
        rows = attach_inventory(with_resource(trace()))
        self.assertEqual(validate(rows)["native_source_inventory_verified"], 1)
        self.assertEqual(rows[-1]["metrics"]["selector_event_count"], 3)
        for key in ("raw_count", "collapsed_count", "policy_board_count",
                    "selector_event_count", "decode_window_count",
                    "supplemental_count"):
            for delta in (-1, 1, .5, float("nan")):
                with self.subTest(key=key, delta=delta):
                    bad = copy.deepcopy(rows)
                    bad[-1]["metrics"][key] += delta
                    with self.assertRaises(TraceError):
                        validate(bad)
        for field, value in (("candidate_id", "c1"),
                             ("parent_candidate_ids", ["c1"]),
                             ("reason_code", "source_counts_assumed"),
                             ("outcome", "selected")):
            with self.subTest(field=field):
                bad = copy.deepcopy(rows)
                bad[-1][field] = value
                with self.assertRaises(TraceError):
                    validate(bad)
        for mutation in ("missing", "duplicate", "before_guard", "foreign"):
            with self.subTest(mutation=mutation):
                bad = copy.deepcopy(rows)
                if mutation == "missing":
                    bad.pop()
                elif mutation == "duplicate":
                    bad.append(copy.deepcopy(bad[-1]))
                elif mutation == "before_guard":
                    bad[-1], bad[-2] = bad[-2], bad[-1]
                else:
                    bad[-1]["metrics"]["private_path"] = 1
                for i, row in enumerate(bad):
                    row["sequence"] = i
                with self.assertRaises(TraceError):
                    validate(bad, require_windows=True)

    def test_native_inventory_negative_empty_board(self):
        rows = empty_board_native_trace()
        self.assertEqual(validate(rows, require_windows=True)[
            "native_empty_board_boundary_verified"], 1)
        for name in ("raw_count", "collapsed_count", "policy_board_count",
                     "selector_event_count", "decode_window_count"):
            bad = copy.deepcopy(rows)
            bad[-1]["metrics"][name] += 1
            with self.subTest(name=name), self.assertRaises(TraceError):
                validate(bad, require_windows=True)
        missing = copy.deepcopy(rows)
        missing.pop()
        with self.assertRaises(TraceError):
            validate(missing, require_windows=True)
        self.assertEqual(validate(trace())["native_source_inventory_verified"], 0)
        with self.assertRaises(TraceError):
            validate(with_resource(trace()), require_windows=True)

    def test_native_selector_capture_fails_closed_on_loss(self):
        """CI capture overflow/OOM must never be reported as complete provenance."""
        source = (ROOT / "src" / "foo_smart_tempo" /
                  "mir_candidate_pipeline.cpp").read_text(encoding="utf-8")
        header = (ROOT / "src" / "foo_smart_tempo" /
                  "mir_candidate_pipeline.h").read_text(encoding="utf-8")
        component = (ROOT / "src" / "foo_smart_tempo" /
                     "modern_bpm_analyzer.cpp").read_text(encoding="utf-8")
        self.assertIn("mir_ci_primary_selector_trace_complete() noexcept", header)
        self.assertIn("g_ci_primary_events.size() >= 4096", source)
        self.assertEqual(source.count("g_ci_primary_trace_complete = false;"), 3)
        self.assertEqual(source.count("g_ci_primary_trace_complete = true;"), 2)
        self.assertIn("g_ci_primary_events.clear();\n  g_ci_primary_pool.clear();\n  g_ci_primary_trace_complete = true;", source)
        self.assertIn("mir_ci_primary_selector_trace_complete() &&", component)
        self.assertLess(component.index("mir_ci_primary_selector_trace_complete() &&"),
                        component.index("smart_tempo::ci_pipeline_trace::record("))
        self.assertIn("#if defined(SMART_TEMPO_CI_ORACLE_PROBE)", source)

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
        self.assertIn("ciWindows.push_back", component)
        self.assertIn("ciWindowCaptureFailed", component)
        self.assertIn('out.push_window(w)', header)
        self.assertIn('"actual_decoded_extent"', header)
        self.assertIn("--require-windows", host)
        self.assertIn('L"GITHUB_ACTIONS"', header)
        self.assertIn('L"RUNNER_TEMP"', header)
        self.assertIn('L"SMART_TEMPO_CI_FIXTURE"', header)
        # Both CI observers must share the exact fixed synthetic allowlist;
        # moving the literal to the shared header must not weaken the guard.
        fixture_guard = (ROOT / "tests" / "ci_fixture_allowlist.h").read_text(
            encoding="utf-8")
        self.assertIn('#include "ci_fixture_allowlist.h"', header)
        self.assertIn("ci_fixture::permitted(fixture, root)", header)
        for case in ("pulse_60", "pulse_128", "pulse_120_sparse", "silence"):
            self.assertIn(case + ".wav", fixture_guard)
        self.assertIn("_wcsnicmp", fixture_guard)
        self.assertIn("CREATE_NEW", header)
        self.assertIn("mir_ci_primary_selector_pool()", component)
        self.assertIn("ci_run_id()", header)
        self.assertIn("same_run_measured_witness", header)
        self.assertIn('out.push("measured_witness"', header)
        self.assertIn('out.push("policy_resolution"', header)
        self.assertIn('out.push("policy_pointer"', header)
        self.assertIn('out.push("source_inventory"', header)
        self.assertIn('native_count_conservation', header)
        self.assertIn("mir_ci_policy_pointer_source()", component)
        source = (ROOT / "src" / "foo_smart_tempo" /
                  "mir_candidate_pipeline.cpp").read_text(encoding="utf-8")
        self.assertIn("ci_policy_selected(selected)", source)
        self.assertIn("g_ci_policy_board = candidates", source)
        self.assertIn("MirCiSupplementalKind::partial_bar_topk, selected);", source)
        self.assertIn("MirCiSupplementalKind::material_risk, materialRiskProbe);", source)
        self.assertIn("MirCiSupplementalKind::partial_bar_aggregate, partialBarAggregate);", source)
        schema = json.loads((ROOT / "tools" /
            "smart_tempo_decision_trace_v1.schema.json").read_text(encoding="utf-8"))
        self.assertTrue(set(ENUMS).issubset(set(schema["properties"]["stage"]["enum"])))
        self.assertNotIn("std::isfinite(value) ? value : 0.0", header)
        self.assertIn("pool_only_gate_unresolved", header)
        self.assertIn("mir_ci_primary_selector_events()", component)
        self.assertIn("candidate_filter", header)
        self.assertIn("primary_rank", header)
        self.assertIn("item.source_index >= count", header)
        self.assertIn("primary_parent.push_back(cluster_ids[item.source_index])", header)
        self.assertIn("terminal_final_choice", header)
        self.assertIn("ids[item.source_index]", header)
        self.assertIn('"primary_terminal"', header)
        self.assertIn("general_baseline_rank_scored", header)
        self.assertNotIn("std::size_t match = count", header)
        selector_header = (ROOT / "src" / "foo_smart_tempo" /
                           "mir_candidate_pipeline.h").read_text(encoding="utf-8")
        self.assertIn("std::size_t source_index = 0", selector_header)
        selector = (ROOT / "src" / "foo_smart_tempo" /
                    "mir_candidate_pipeline.cpp").read_text(encoding="utf-8")
        self.assertIn("candidate - board.raw_candidates.data()", selector)
        self.assertIn("&candidate - candidates.data()", selector)
        for snippet in ("raw_alias_provisional_exclusion", "raw_support_provisional_exclusion",
                        "low_upper_gate_provisional_exclusion", "low_threshold_allowed",
                        "harmonic_rank_scored", "general_out_of_rail_provisional",
                        "general_alt_rank_scored", "low_continuous_allowed",
                        "terminal_lane_choice", "terminal_residual_choice",
                        "terminal_final_choice"):
            self.assertIn(snippet, selector)
        self.assertIn("audit_smart_tempo_ci_pipeline_trace.py --trace", host)
        self.assertIn("test_smart_tempo_ci_pipeline_trace.py", workflow)
        self.assertNotRegex(workflow, r"(?m)^\s*uses:\s*actions/upload-artifact@")

    def test_native_residual_and_direct_terminal_causal_replay(self):
        # Synthetic source objects, never private audio or oracle BPM input.
        # Raw c0/c1 -> collapsed c2/c3, independent FullBoard c4.
        def make_case(kind):
            rows = lineage_trace([128., 130.], [128., 130.], [[0], [1]])
            at = next(i for i, row in enumerate(rows)
                      if row["stage"] == "primary_select")
            source = "c3"
            gate = ("residual_high_gate_allowed" if kind == "high" else
                    "residual_harmonic_gate_allowed" if kind == "harmonic" else
                    "harmonic_direct_gate_allowed")
            family = ("residual_high" if kind == "high" else
                      "residual_harmonic" if kind == "harmonic" else
                      "harmonic_direct")
            selected_bpm = 130.
            rank = event("primary_rank", source, [], selected_bpm, "included",
                         family + "_rank_scored", 0)
            compare = event("primary_compare", source, [], selected_bpm,
                            "included", family + "_compare_won", 0)
            if family != "residual_high":
                compare["metrics"].update(
                    {f"tie_{i}": value for i, value in enumerate(
                        (0.95, 0.85, 0.50, 0.0))})
            gate_row = event("candidate_filter", source, [], selected_bpm,
                             "included", gate, 0)
            lane = event("primary_terminal", "c2", [], 128., "selected",
                         "terminal_lane_choice", 0)
            residual = event("primary_terminal",
                             source if kind != "direct" else "c2", [],
                             selected_bpm if kind != "direct" else 128.,
                             "selected", "terminal_residual_choice", 0)
            final = event("primary_terminal", source, [], selected_bpm,
                          "selected", "terminal_final_choice", 0)
            if kind == "direct":
                middle = [residual, gate_row, rank, compare]
            else:
                middle = [gate_row, rank, compare, residual]
            # The first lane must independently win its own native
            # comparator before a later rescue can change the object.
            lane_rank = event("primary_rank", "c2", [], 128., "included",
                              "general_baseline_rank_scored", 0)
            lane_compare = event("primary_compare", "c2", [], 128., "included",
                                 "general_baseline_compare_won", 0)
            rows[at:at] = [lane_rank, lane_compare, lane] + middle + [final]
            primary = next(row for row in rows if row["stage"] == "primary_select")
            primary["parent_candidate_ids"] = [source]
            primary["metrics"]["bpm"] = selected_bpm
            for i, row in enumerate(rows):
                row["sequence"] = i
            return rows

        for kind in ("high", "harmonic", "direct"):
            with self.subTest(kind=kind):
                family = ("residual_high" if kind == "high" else
                          "residual_harmonic" if kind == "harmonic" else
                          "harmonic_direct")
                rows = make_case(kind)
                result = validate(rows)
                self.assertEqual(result["selector_residual_winner_proven"],
                                 int(kind != "direct"))
                self.assertEqual(result["selector_direct_winner_proven"],
                                 int(kind == "direct"))
                # Every forged step is rejected independently, even when
                # the final BPM still exists in the source candidate board.
                for mutation in ("gate_removed", "rank_removed",
                                 "false_loss", "terminal_reverted",
                                 "comparison_outside_phase",
                                 "foreign_tie", "missing_comparison"):
                    bad = copy.deepcopy(rows)
                    gate = next(r for r in bad if r["reason_code"].endswith(
                        "_gate_allowed"))
                    rank = next(r for r in bad if r["reason_code"] ==
                                family + "_rank_scored")
                    compare = next(r for r in bad if r["reason_code"] ==
                                   family + "_compare_won")
                    if mutation == "gate_removed":
                        bad.remove(gate)
                    elif mutation == "rank_removed":
                        bad.remove(rank)
                    elif mutation == "false_loss":
                        compare["reason_code"] = compare["reason_code"].replace(
                            "_won", "_lost")
                        compare["outcome"] = "excluded"
                    elif mutation == "terminal_reverted":
                        target = ("terminal_final_choice" if kind == "direct"
                                  else "terminal_residual_choice")
                        row = next(r for r in bad if r["reason_code"] == target)
                        row["candidate_id"] = "c2"
                        row["metrics"]["bpm"] = 128.
                    elif mutation == "comparison_outside_phase":
                        bad.remove(rank)
                        bad.remove(compare)
                        pos = next(i for i, r in enumerate(bad)
                                   if r["reason_code"] == "terminal_final_choice")
                        bad[pos+1:pos+1] = [rank, compare]
                    elif mutation == "foreign_tie":
                        compare["metrics"]["tie_0"] = float("nan") if (
                            kind != "high") else 1.
                        if kind == "high":
                            compare["metrics"]["private_path"] = 1.
                    elif mutation == "missing_comparison":
                        bad.remove(compare)
                    for i, row in enumerate(bad):
                        row["sequence"] = i
                    with self.assertRaises(TraceError, msg=f"{kind}:{mutation}"):
                        validate(bad)

    def test_lane_comparator_after_terminal_is_not_laundered_as_provenance(self):
        rows = lineage_trace([128., 130.], [128., 130.], [[0], [1]])
        at = next(i for i, r in enumerate(rows) if r["stage"] == "primary_select")
        rank = event("primary_rank", "c2", [], 128., "included",
                     "general_baseline_rank_scored", 0)
        comp = event("primary_compare", "c2", [], 128., "included",
                     "general_baseline_compare_won", 0)
        terminals = [event("primary_terminal", "c2", [], 128., "selected",
                           reason, 0) for reason in
                     ("terminal_lane_choice", "terminal_residual_choice",
                      "terminal_final_choice")]
        rows[at:at] = [rank, comp] + terminals
        for i, row in enumerate(rows):
            row["sequence"] = i
        self.assertEqual(validate(rows)["selector_lane_comparator_winner_proven"], 1)
        bad = copy.deepcopy(rows)
        bad.remove(bad[at])
        bad.remove(bad[at])
        final_index = next(i for i, r in enumerate(bad)
                           if r["reason_code"] == "terminal_final_choice")
        bad[final_index+1:final_index+1] = [rank, comp]
        for i, row in enumerate(bad):
            row["sequence"] = i
        with self.assertRaises(TraceError):
            validate(bad, require_native_comparator=True)

    def test_rescue_trace_source_guarded_and_not_production_mutation(self):
        source = (ROOT / "src/foo_smart_tempo/mir_candidate_pipeline.cpp").read_text(
            encoding="utf-8")
        reporter = (ROOT / "tests/ci_pipeline_trace.h").read_text(
            encoding="utf-8")
        for family in ("residual_high", "residual_harmonic", "harmonic_direct"):
            self.assertIn('"' + family + '_rank_scored"', source)
            self.assertIn('"' + family + '_compare_won"', source)
            self.assertIn('"' + family + '_compare_lost"', source)
            self.assertIn('"' + family + '_rank_scored"', reporter)
        self.assertIn('item.has_tie_metrics != tieExpected', reporter)
        self.assertIn('sizeof("residual_harmonic_compare_") - 1', reporter)
        self.assertIn('sizeof("harmonic_direct_compare_") - 1', reporter)
        self.assertEqual(source.count("const bool ciRescueWon ="), 3)



    def test_native_supplemental_selected_object_writer_and_negative_host_contract(self):
        # The real policy selector and measured writer invariant run inside
        # original Win32/x64 foobar, but only on a fabricated CI candidate
        # board. Do not equate this with a decoded WAV selecting TopK.
        probe = (ROOT / "tests" / "smart_tempo_ci_probe.cpp").read_text(
            encoding="utf-8")
        host = (ROOT / "tests" / "diagnostic_live_host_smoke.ps1").read_text(
            encoding="utf-8")
        pipeline = (ROOT / "src" / "foo_smart_tempo" /
                    "mir_candidate_pipeline.cpp").read_text(encoding="utf-8")
        for name in ("native_supplemental_policy_contract",
                     "try_apply_mir_policy_partial_bar_topk_exact_release",
                     "mir_ci_policy_pointer_source",
                     "mir_output_has_measured_candidate_witness",
                     "enforce_mir_measured_candidate_invariant"):
            self.assertIn(name, probe)
        self.assertIn("selected.object != &rows[1]", probe)
        self.assertIn("rows[1].local_exact_score = .95", probe)
        self.assertIn("forged.output_bpm = 145.0", probe)
        self.assertIn("forged.output_would_write", probe)
        self.assertIn("badScore", probe)
        self.assertIn("!reject(false, false, true, true, candidates)", probe)
        self.assertIn("!reject(true, true, true, true, candidates)", probe)
        self.assertIn("!reject(true, false, false, true, candidates)", probe)
        self.assertIn("!reject(true, false, true, false, candidates)", probe)
        self.assertIn("SMART_TEMPO_CI_NATIVE_SUPPLEMENTAL=PASS", probe)
        self.assertIn("SMART_TEMPO_NATIVE_SUPPLEMENTAL_{0}_{1}=PASS", host)
        self.assertIn("Original host did not prove native supplemental", host)
        self.assertIn("ci_policy_supplemental(selected->local_exact_bpm", pipeline)
        self.assertIn("MirCiSupplementalKind::partial_bar_topk, selected", pipeline)
        self.assertIn("SMART_TEMPO_CI_ORACLE_PROBE", probe)
        self.assertNotIn("metadb_io::get()->update_info", probe)
        self.assertNotIn("upload-artifact", probe)


    def test_native_nested_low_rescue_three_epochs_and_missing_alias_host_contract(self):
        # One source ID, three sequential bestBy episodes, and an independent
        # one-bit absence-of-rescue variant must execute inside original host.
        probe = (ROOT / "tests" / "smart_tempo_ci_probe.cpp").read_text(
            encoding="utf-8")
        host = (ROOT / "tests" / "diagnostic_live_host_smoke.ps1").read_text(
            encoding="utf-8")
        source = (ROOT / "src" / "foo_smart_tempo" /
                  "mir_candidate_pipeline.cpp").read_text(encoding="utf-8")
        for snippet in (
            "native_low_nested_rescue_contract(true, 71.0, 3)",
            "native_low_nested_rescue_contract(false, 80.0, 2)",
            "MirAlias::two_thirds : MirAlias::direct",
            "MirAlias::half",
            "MirAlias::three_quarters",
            "const double expectedWinners[4] = {0.0, 60.0, 80.0, 71.0}",
            "ranks[1] != 3 || ranks[2] != 2",
            "wins[1] != 1 || wins[2] != 1",
            "losses[1] != 2 || losses[2] != 1",
            "maxEpoch != expectedEpochs",
            "event.source_index != rankIndex",
            "event.low_pool_epoch < 1",
            "event.bpm != expectedWinners[ep]",
            "event.source_index != (twoThirdsAlias ? 2u : 1u)",
            "CREATE_NEW", "SMART_TEMPO_CI_NATIVE_LOW_NESTED=PASS",
        ):
            self.assertIn(snippet, probe)
        self.assertIn("SMART_TEMPO_NATIVE_LOW_NESTED_{0}_{1}=PASS", host)
        self.assertIn("Original foobar host did not prove native nested low rescue", host)
        self.assertIn("const auto bestBy =", source)
        self.assertIn("const std::size_t ciThisPoolEpoch = ++ciLowBestByEpoch;", source)
        self.assertNotIn("metadb_io::get()->update_info", probe)
        self.assertNotIn("upload-artifact", probe)

    def test_native_nested_low_rescue_mutation_contract_fails_closed(self):
        # Textual source-to-test negative parity: removing either case, the
        # run-local epoch bound or the exact source-ID edge is unacceptable.
        probe = (ROOT / "tests" / "smart_tempo_ci_probe.cpp").read_text(
            encoding="utf-8")
        required = (
            "!native_low_nested_rescue_contract(true, 71.0, 3)",
            "!native_low_nested_rescue_contract(false, 80.0, 2)",
            "if (pendingRank || maxEpoch != expectedEpochs",
            "event.source_index != rankIndex",
            "event.bpm != expectedWinners[ep]",
        )
        for guard in required:
            with self.subTest(guard=guard):
                self.assertNotEqual(probe.find(guard), -1)


    def test_native_raw_alias_and_support_fallback_positive_negative_host_parity(self):
        probe = (ROOT / "tests" / "smart_tempo_ci_probe.cpp").read_text(
            encoding="utf-8")
        host = (ROOT / "tests" / "diagnostic_live_host_smoke.ps1").read_text(
            encoding="utf-8")
        source = (ROOT / "src" / "foo_smart_tempo" /
                  "mir_candidate_pipeline.cpp").read_text(encoding="utf-8")
        for text in ("native_raw_fallback_contract(0)",
                     "native_raw_fallback_contract(1)",
                     "native_raw_fallback_contract(2)",
                     "raw_alias_fallback_readmitted",
                     "raw_support_fallback_readmitted",
                     "raw_alias_provisional_exclusion",
                     "raw_support_provisional_exclusion",
                     "raw_compare_won",
                     "terminal_final_choice",
                     "const std::array<E, 15> fallbackBoth",
                     "const std::array<E, 9> directOnly",
                     "const std::array<E, 12> aliasOnly",
                     "event.source_index == expected.source",
                     "event.raw == expected.raw",
                     "event.included == expected.included",
                     "if (events.size() != expected.size())",
                     "SMART_TEMPO_CI_NATIVE_RAW_FALLBACK=PASS",
                     "MirAlias::one_third", "MirAlias::quarter",
                     "MirAlias::direct"):
            self.assertIn(text, probe)
        self.assertIn("SMART_TEMPO_NATIVE_RAW_FALLBACK_{0}_{1}=PASS", host)
        self.assertIn(
            "Original host did not prove native raw alias/support fallback sequence",
            host)
        for reason in ("raw_alias_fallback_readmitted",
                       "raw_support_fallback_readmitted",
                       "raw_alias_provisional_exclusion",
                       "raw_support_provisional_exclusion"):
            self.assertIn('"' + reason + '"', source)
        self.assertIn("const MirRawCandidate* select_mir_raw_conservative", source)
        self.assertNotIn("metadb_io::get()->update_info", probe)

    def test_native_raw_fallback_no_private_artifacts_and_frozen_oracle(self):
        probe = (ROOT / "tests" / "smart_tempo_ci_probe.cpp").read_text(
            encoding="utf-8")
        workflow = (ROOT / ".github" / "workflows" / "source.yml").read_text(
            encoding="utf-8")
        self.assertIn("CREATE_NEW", probe)
        self.assertIn("g_ci_fixture + L\".native-raw-fallback\"", probe)
        self.assertNotIn("reference_bpm", probe)
        # Workflow comments may mention forbidden uploads; prohibit only
        # actual upload-artifact actions, preserving documentation.
        self.assertNotRegex(workflow, r"(?m)^\\s*uses:\\s*actions/upload-artifact@")

class RawDecisiveVetoTests(unittest.TestCase):
    """Same-BPM raw objects retain different causal identities."""

    def cases(self, support=False):
        rows = lineage_trace([128., 128.], [128.], [[0, 1]])
        gates = (
            [event("candidate_filter", "c0", [], 128., "included", "raw_alias_allowed", 0),
             event("candidate_filter", "c1", [], 128., "included", "raw_alias_allowed", 0),
             event("candidate_filter", "c0", [], 128., "excluded", "raw_support_provisional_exclusion", 0),
             event("candidate_filter", "c1", [], 128., "included", "raw_support_allowed", 0),
             event("candidate_filter", "c0", [], 128., "excluded", "raw_support_decisive_veto", 0)]
            if support else
            [event("candidate_filter", "c0", [], 128., "excluded", "raw_alias_provisional_exclusion", 0),
             event("candidate_filter", "c1", [], 128., "included", "raw_alias_allowed", 0),
             event("candidate_filter", "c0", [], 128., "excluded", "raw_alias_decisive_veto", 0),
             event("candidate_filter", "c1", [], 128., "included", "raw_support_allowed", 0)])
        at = next(i for i, r in enumerate(rows) if r["stage"] == "primary_select")
        rows[at:at] = gates
        for i, r in enumerate(rows): r["sequence"] = i
        return rows

    def test_both_native_decisive_veto_types(self):
        for support in (False, True):
            with self.subTest(support=support):
                out = validate(self.cases(support))
                self.assertEqual(out["selector_raw_decisive_source_ids"], 1)
                self.assertEqual(out["selector_raw_decisive_support_vetos"], int(support))
                self.assertEqual(out["selector_raw_decisive_alias_vetos"], int(not support))

    def test_adversarial_source_fate_corruption_rejected(self):
        for support in (False, True):
            for mutation in ("source", "readmit", "rank", "duplicate", "precursor", "survivor"):
                with self.subTest(support=support, mutation=mutation):
                    bad = copy.deepcopy(self.cases(support))
                    veto = next(i for i, r in enumerate(bad) if
                                r["reason_code"].endswith("_decisive_veto"))
                    if mutation == "source":
                        bad[veto]["candidate_id"] = "c1"
                    elif mutation == "readmit":
                        bad.insert(veto+1, event("candidate_filter", "c0", [], 128.,
                                   "included", "raw_support_fallback_readmitted" if support
                                   else "raw_alias_fallback_readmitted", 0))
                    elif mutation == "rank":
                        bad.insert(veto+1, event("primary_rank", "c0", [], 128.,
                                                "included", "raw_rank_scored", 0))
                    elif mutation == "duplicate":
                        bad.insert(veto+1, copy.deepcopy(bad[veto]))
                    elif mutation == "precursor":
                        bad = [r for r in bad if not (
                            r["candidate_id"] == "c0" and
                            r["reason_code"].endswith("_provisional_exclusion"))]
                    elif mutation == "survivor":
                        survive = "raw_support_allowed" if support else "raw_alias_allowed"
                        bad = [r for r in bad if not (
                            r["candidate_id"] == "c1" and r["reason_code"] == survive)]
                    for i, r in enumerate(bad): r["sequence"] = i
                    with self.assertRaises(TraceError): validate(bad)

    def test_six_original_native_branch_scenarios_are_frozen(self):
        cpp = (ROOT / "tests/smart_tempo_ci_probe.cpp").read_text(encoding="utf-8")
        src = (ROOT / "src/foo_smart_tempo/mir_candidate_pipeline.cpp").read_text(
            encoding="utf-8")
        for i in range(6):
            self.assertIn(f"native_raw_fallback_contract({i})", cpp)
        for reason in ("raw_alias_decisive_veto", "raw_support_decisive_veto"):
            self.assertIn('"' + reason + '"', src)
            self.assertIn('E{"' + reason + '"', cpp)
        for marker in ("variant == 3 ? .08", "variant == 4 ? .079999",
                       "supportAtFloor", "supportBelowFloor", "supportVetoOnly",
                       "mir_ci_primary_selector_trace_complete()",
                       "event.source_index == expected.source"):
            self.assertIn(marker, cpp)
        self.assertIn("support >= supportFloor", src)
        self.assertIn("if (!supported.empty())", src)


if __name__ == "__main__":
    unittest.main()
