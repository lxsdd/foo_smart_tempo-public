#!/usr/bin/env python3
"""Strict synthetic-only actual C++ MIR-pool/Primary/Policy JSONL observer audit.

No user media, no path/title/artist/genre fields in record. This validates
observed lineage; it cannot infer per-candidate selector vetoes that the
instrumented production selector has not yet exposed.
"""
from __future__ import annotations
import argparse
import json
import math
from pathlib import Path
import re
from audit_smart_tempo_b_raw_fates import RawFateError, reconcile_native_raw
from audit_smart_tempo_b_cluster_rail_fates import RailFateError, reconcile_native_rail
from audit_smart_tempo_b_writer_resolution import (
    WriterResolutionError, reconcile as reconcile_writer_resolution)
from audit_smart_tempo_b_low_upper_fates import LowUpperFateError, reconcile_native_low_upper
from audit_smart_tempo_b_low_continuous_fates import (
    LowContinuousFateError, reconcile_native_low_continuous)
from audit_smart_tempo_b_manual_export import (
    ManualTraceError, reconcile as reconcile_manual_export)

SCHEMA = "smart_tempo_source_decision_trace_v1"
MAX_BYTES, MAX_LINES = 2 * 1024 * 1024, 16500
FIELDS = frozenset(("schema", "analysis_id", "sequence", "stage", "outcome",
                    "reason_code", "candidate_id", "parent_candidate_ids", "metrics"))
METRICS = frozenset(("bpm", "score", "support"))
LOW_POOL_METRICS = METRICS | frozenset(("low_pool_epoch",))
MANUAL_METRICS = METRICS | frozenset((
    "winner_support", "local_exact_score", "evidence_rank", "origin_mask"))
TIE_METRICS = METRICS | frozenset(("tie_0", "tie_1", "tie_2", "tie_3"))
TIE5_METRICS = TIE_METRICS | frozenset(("tie_4",))
RESOURCE_METRICS = METRICS | frozenset((
    "wall_seconds", "process_cpu_seconds",
    "process_peak_rss_before_bytes", "process_peak_rss_after_bytes",
    "process_working_set_after_bytes"))
INVENTORY_METRICS = METRICS | frozenset((
    "raw_count", "collapsed_count", "policy_board_count",
    "selector_event_count", "decode_window_count", "supplemental_count",
    "mir_segments_evaluated", "mir_segment_results", "mir_topk_rows_seen",
    "mir_topk_add_calls", "mir_topk_new_raw", "mir_family_rank_rows",
    "mir_alias_add_calls", "mir_alias_new_raw",
    "mir_continuous_add_calls", "mir_continuous_new_raw"))
SEGMENT_METRICS = METRICS | frozenset((
    "segment_index", "odf_peak_count", "top_k_count", "candidate", "aggregated"))
MIR_SEGMENT_REASONS = frozenset((
    "invalid_input", "insufficient_odf", "insufficient_onsets",
    "single_event", "no_loop_hypotheses", "ambiguous_tatum_fit",
    "missing_bar_division", "audacity_mir_full",
    "audacity_mir_full_below_threshold"))
WINDOW_METRICS = METRICS | frozenset(("pass_index", "pass_total", "start_sec",
    "requested_frames", "decoded_frames", "analysis_frames",
    "source_rate_hz", "analysis_rate_hz"))
CREATORS = frozenset(("candidate_create", "candidate_transform",
                      "policy_eligibility", "supplemental_candidate",
                      "manual_source", "manual_export"))
ENUMS = {
    "decode_window": ("observed", "actual_decoded_extent"),
    "mir_segment": ("observed", None),
    "candidate_create": ("observed", None),
    "candidate_transform": ("observed", "cluster_from_raw"),
    "primary_eligibility": ("observed", "pool_only_gate_unresolved"),
    "policy_filter": (None, None),
    "candidate_filter": (None, None),
    "primary_rank": ("included", None),
    "primary_compare": (None, None),
    "primary_terminal": ("selected", None),
    "primary_disposition": ("observed", None),
    "primary_first_exclusion": (None, None),
    "policy_eligibility": ("included", None),
    "supplemental_candidate": ("observed", None),
    "primary_select": (None, None),
    "policy_resolution": ("observed", None),
    "policy_pointer": ("observed", None),
    "policy_change": (None, None),
    "writer_resolution": (None, None),
    "manual_state": ("observed", None),
    "manual_source": ("observed", None),
    "manual_export": ("observed", "manual_merged_export"),
    "final_output": (None, None),
    "measured_witness": (None, None),
    "resource_usage": ("observed", "process_resource_snapshot"),
    "write_guard": ("write_blocked", "ci_dry_run"),
    "source_inventory": ("observed", "native_count_conservation")
}
REASON_PATTERN = re.compile(r"^[a-z][a-z0-9_]{0,95}$")
ID_PATTERN = re.compile(r"^c(?:0|[1-9][0-9]{0,6})$")
RAW_SOURCE_MASKS = {
    "raw_topk": 1, "raw_pulse_alias": 2, "raw_topk_pulse_alias": 3,
    "raw_continuous": 4, "raw_topk_continuous": 5,
    "raw_pulse_alias_continuous": 6, "raw_all_sources": 7,
}
ALLOWED_FILTER_REASONS = frozenset((
    "raw_alias_allowed", "raw_alias_provisional_exclusion",
    "raw_alias_decisive_veto", "raw_support_decisive_veto",
    "raw_support_allowed", "raw_support_provisional_exclusion",
    "low_upper_gate_allowed", "low_upper_gate_provisional_exclusion",
    "low_upper_branch_veto",
    "low_threshold_allowed", "low_threshold_provisional_exclusion",
    "harmonic_in_rail", "harmonic_out_of_rail",
    "harmonic_rail_decisive_veto", "general_rail_decisive_veto",
    "general_in_rail", "general_out_of_rail_provisional",
    "low_continuous_allowed", "low_continuous_provisional_exclusion",
    "low_continuous_branch_veto", "low_continuous_branch_ranked_out",
    "low_continuous_branch_winner",
    "low_threshold_fallback_readmitted", "general_rail_fallback_readmitted",
    "residual_high_gate_allowed", "residual_high_gate_provisional_exclusion",
    "residual_harmonic_gate_allowed", "residual_harmonic_gate_provisional_exclusion",
    "harmonic_direct_gate_allowed", "harmonic_direct_gate_provisional_exclusion",
    "raw_alias_fallback_readmitted", "raw_support_fallback_readmitted",
    "general_alt_window_allowed", "general_alt_window_provisional_exclusion",
    "general_alt_baseline_source", "general_alt_bypass_route",
    "general_alt_bypass_non144", "general_alt_no_challenger",
    "general_alt_baseline_evidence", "general_alt_challenger_evidence",
    "general_alt_challenger_promoted", "general_alt_challenger_rejected",
    "low_mid_58_61_allowed", "low_mid_58_61_provisional_exclusion",
    "low_lower_mid_58_61_allowed", "low_lower_mid_58_61_provisional_exclusion",
    "low_midlow_56_58_allowed", "low_midlow_56_58_provisional_exclusion",
    "low_deeplow_64_67_allowed", "low_deeplow_64_67_provisional_exclusion",
    "low_lower_mid_79_81_allowed", "low_lower_mid_79_81_provisional_exclusion",
    "low_upper_direct_86_90_allowed", "low_upper_direct_86_90_provisional_exclusion",
    "low_verylow_50_53_allowed", "low_verylow_50_53_provisional_exclusion",
    "low_upper_recovery_50_53_allowed", "low_upper_recovery_50_53_provisional_exclusion"
))
ALLOWED_RANK_REASONS = frozenset((
    "raw_rank_scored", "harmonic_rank_scored", "low_rank_scored",
    "low_continuous_rank_scored",
    "general_baseline_rank_scored", "general_alt_rank_scored",
    "residual_high_rank_scored", "residual_harmonic_rank_scored",
    "harmonic_direct_rank_scored"
))
COMPARE_TO_RANK = {
    "raw": "raw_rank_scored",
    "harmonic": "harmonic_rank_scored",
    "low": "low_rank_scored",
    "low_continuous": "low_continuous_rank_scored",
    "general_baseline": "general_baseline_rank_scored",
    "general_alt": "general_alt_rank_scored",
    "residual_high": "residual_high_rank_scored",
    "residual_harmonic": "residual_harmonic_rank_scored",
    "harmonic_direct": "harmonic_direct_rank_scored"
}
ALLOWED_COMPARE_REASONS = frozenset(
    prefix + "_compare_" + verdict
    for prefix in COMPARE_TO_RANK for verdict in ("won", "lost")
)
ALLOWED_DISPOSITION_REASONS = frozenset((
    "selected_final_object", "ranked_not_final",
    "observed_not_ranked", "no_cluster_observation"
))
ALLOWED_TERMINAL_REASONS = frozenset((
    "terminal_lane_choice", "terminal_residual_choice",
    "terminal_final_choice"
))
ALLOWED_POLICY_COMPARISONS = frozenset((
    "policy_no_output", "policy_one_same_bpm",
    "policy_no_fullboard_match", "policy_multiple_same_bpm"
))
ALLOWED_SUPPLEMENTAL_SOURCES = frozenset((
    "material_risk_source", "partial_bar_aggregate_source",
    "partial_bar_topk_source"
))
ALLOWED_POINTER_REASONS = frozenset((
    "pointer_no_output", "pointer_native_fullboard_consistent",
    "pointer_native_supplemental_consistent",
    "pointer_supplemental_or_changed", "pointer_not_observed"
))


class TraceError(ValueError):
    pass


def validate(rows: list[dict], *, require_windows: bool = False,
             require_segments: bool = False,
             require_native_comparator: bool = False,
             require_policy_filter: bool = False,
             require_writer_resolution: bool = False) -> dict:
    # Library callers may test legacy minimal traces. The real host CLI
    # always requires both actual windows and actual native MIR segments.
    if require_segments and not require_windows:
        raise TraceError("segment proof requires native window proof")
    if not isinstance(rows, list) or not 8 <= len(rows) <= MAX_LINES:
        raise TraceError("trace missing or out of row bound")
    ids: dict[str, str] = {}
    candidate_bpm: dict[str, float] = {}
    stage_indices: dict[str, list[dict]] = {name: [] for name in ENUMS}
    stage_order = {
        "decode_window": -1, "mir_segment": -1,
        "candidate_create": 0, "candidate_transform": 1,
        "primary_eligibility": 1, "policy_filter": 2, "policy_eligibility": 2,
        "supplemental_candidate": 2,
        "candidate_filter": 3, "primary_rank": 3, "primary_compare": 3,
        "primary_terminal": 3, "primary_disposition": 3,
        "primary_first_exclusion": 3,
        "primary_select": 4,
        "policy_resolution": 5, "policy_pointer": 6,
        "policy_change": 7, "writer_resolution": 8,
            "manual_state": 8, "manual_source": 8, "manual_export": 8,
        "final_output": 9, "measured_witness": 10, "resource_usage": 11,
        "write_guard": 12, "source_inventory": 13
    }
    previous_stage_number = -1
    session_id = rows[0].get("analysis_id") if type(rows[0]) is dict else None
    if type(session_id) is not str or not re.fullmatch(r"[0-9a-f]{16}", session_id):
        raise TraceError("invalid CI run-local session ID")
    for index, item in enumerate(rows):
        if type(item) is not dict or item.keys() != FIELDS and set(item) != FIELDS:
            raise TraceError("trace fields not exact")
        if item["schema"] != SCHEMA or item["analysis_id"] != session_id:
            raise TraceError("foreign trace source/session")
        if type(item["sequence"]) is not int or item["sequence"] != index:
            raise TraceError("trace sequence gap/reordering")
        stage = item["stage"]
        if stage not in ENUMS:
            raise TraceError("unknown stage")
        if stage_order[stage] < previous_stage_number:
            raise TraceError("native stage reordering")
        previous_stage_number = stage_order[stage]
        expected_outcome, expected_reason = ENUMS[stage]
        if (expected_outcome is not None and item["outcome"] != expected_outcome or
            expected_reason is not None and item["reason_code"] != expected_reason or
            item["outcome"] not in ("observed","included","excluded","selected","rejected",
                                    "review_hold","no_candidate","write_blocked")):
            raise TraceError("unapproved outcome/reason")
        if (not isinstance(item["reason_code"], str) or
                not REASON_PATTERN.fullmatch(item["reason_code"])):
            raise TraceError("unbounded reason")
        low_episode = (stage in ("primary_compare", "primary_rank") and
                       item["reason_code"] in ("low_rank_scored",
                                               "low_compare_won",
                                               "low_compare_lost"))
        has_native_tie = (stage == "primary_compare" and
                          item["reason_code"].startswith(("raw_compare_",
                                                          "general_baseline_compare_",
                                                          "residual_harmonic_compare_",
                                                          "harmonic_direct_compare_")))
        continuous_tie = (stage == "primary_compare" and
                          item["reason_code"].startswith("low_continuous_compare_"))
        has_native_tie = has_native_tie or continuous_tie
        expected_metrics = (SEGMENT_METRICS if stage == "mir_segment" else
                            WINDOW_METRICS if stage == "decode_window" else
                            RESOURCE_METRICS if stage == "resource_usage" else
                            INVENTORY_METRICS if stage == "source_inventory" else
                            TIE5_METRICS if continuous_tie else
                            TIE_METRICS if has_native_tie else
                            LOW_POOL_METRICS if low_episode else
                            MANUAL_METRICS if stage in ("manual_source", "manual_export")
                            else METRICS)
        if type(item["metrics"]) is not dict or set(item["metrics"]) != expected_metrics:
            raise TraceError("non-numeric-only telemetry")
        for key, value in item["metrics"].items():
            if type(value) not in (float, int) or not math.isfinite(value):
                raise TraceError("nonfinite or nonnumeric metrics")
        if low_episode and (item["metrics"]["low_pool_epoch"] !=
                            int(item["metrics"]["low_pool_epoch"]) or
                            not 1 <= item["metrics"]["low_pool_epoch"] <= 128):
            raise TraceError("invalid native low-pulse pool epoch")
        bpm = item["metrics"]["bpm"]
        if bpm < 0 or bpm > (220 if stage in ("final_output", "policy_change",
                                                 "writer_resolution") else 2000):
            raise TraceError("out-of-rail BPM")
        candidate_id = item["candidate_id"]
        if candidate_id is not None and (type(candidate_id) is not str or
                                         not ID_PATTERN.fullmatch(candidate_id)):
            raise TraceError("invalid candidate ID")
        parents = item["parent_candidate_ids"]
        if type(parents) is not list or len(parents) > 128 or len(set(parents)) != len(parents):
            raise TraceError("invalid candidate parents")
        if any(type(p) is not str or p not in ids for p in parents):
            raise TraceError("orphan or forward candidate parent")
        if stage in ("decode_window", "mir_segment", "resource_usage", "source_inventory") and (candidate_id is not None or parents):
            raise TraceError("window/resource must not contain candidate identity")
        if stage == "decode_window" and stage_indices["mir_segment"]:
            raise TraceError("decode window emitted after MIR evaluator")
        if stage == "mir_segment":
            m = item["metrics"]
            if item["reason_code"] not in MIR_SEGMENT_REASONS:
                raise TraceError("unknown native MIR first-return reason")
            for key in SEGMENT_METRICS - METRICS:
                if m[key] != int(m[key]):
                    raise TraceError("fractional MIR segment counter")
            if (not 0 <= m["segment_index"] < 128 or
                    not 0 <= m["odf_peak_count"] <= 10000000 or
                    not 0 <= m["top_k_count"] <= 5 or
                    m["candidate"] not in (0, 1) or
                    m["aggregated"] not in (0, 1) or
                    not 0 <= m["score"] <= 1 or m["support"] != 0):
                raise TraceError("invalid native MIR segment metrics")
            late = item["reason_code"].startswith("audacity_mir_full")
            if (not late and (m["bpm"] != 0 or m["score"] != 0 or
                              m["top_k_count"] != 0 or m["candidate"] or
                              m["aggregated"]) or
                late and (m["odf_peak_count"] < 3 or
                          m["candidate"] != int(item["reason_code"] == "audacity_mir_full") or
                          m["aggregated"] != int(m["bpm"] > 0 or m["score"] > 0)) or
                item["reason_code"] == "insufficient_onsets" and
                m["odf_peak_count"] >= 3):
                raise TraceError("MIR first-return reason contradicts native evidence")
        if stage == "resource_usage":
            m = item["metrics"]
            for key in ("process_peak_rss_before_bytes",
                        "process_peak_rss_after_bytes", "process_working_set_after_bytes"):
                if m[key] != int(m[key]) or not 0 < m[key] <= 2**48:
                    raise TraceError("invalid process memory counter")
            if (any(m[key] != 0 for key in METRICS) or
                not 0 < m["wall_seconds"] <= 7200 or
                not 0 <= m["process_cpu_seconds"] <= m["wall_seconds"] * 256 + 1 or
                m["process_peak_rss_after_bytes"] < m["process_peak_rss_before_bytes"] or
                m["process_peak_rss_after_bytes"] < m["process_working_set_after_bytes"]):
                raise TraceError("invalid process resource observation")
        if stage == "decode_window" and (candidate_id is not None or parents):
            raise TraceError("window must not contain candidate identity")
        if stage in CREATORS:
            if stage in ("manual_source", "manual_export"):
                if (not 40 <= bpm <= 220 or
                        item["metrics"]["evidence_rank"] !=
                        int(item["metrics"]["evidence_rank"]) or
                        not 0 <= item["metrics"]["evidence_rank"] <= 8192 or
                        item["metrics"]["origin_mask"] !=
                        int(item["metrics"]["origin_mask"]) or
                        not 1 <= item["metrics"]["origin_mask"] <= 31):
                    raise TraceError("invalid native manual numeric provenance")
                if (stage == "manual_source" and
                        item["reason_code"] not in (
                            "manual_full_board", "manual_primary_selector",
                            "manual_material_risk", "manual_partial_aggregate",
                            "manual_partial_topk")) or (
                        stage == "manual_export" and
                        any(ids[x] != "manual_source" for x in parents)):
                    raise TraceError("unrecognized manual source identity")
            if stage == "candidate_create" and item["reason_code"] not in RAW_SOURCE_MASKS:
                raise TraceError("raw source origin missing or fabricated")
            if stage == "supplemental_candidate" and (
                    parents or item["reason_code"] not in ALLOWED_SUPPLEMENTAL_SOURCES or
                    bpm <= 0):
                raise TraceError("unmeasured or fabricated supplemental source")
            if candidate_id is None or candidate_id in ids:
                raise TraceError("duplicated or missing created candidate ID")
            if int(candidate_id[1:]) != len(ids):
                raise TraceError("noncontiguous candidate lineage IDs")
            if stage == "candidate_transform" and any(
                    abs(bpm - candidate_bpm[p]) >= .001 for p in parents):
                raise TraceError("cluster did not descend from matching raw BPM")
            ids[candidate_id] = stage
            candidate_bpm[candidate_id] = bpm
        elif stage == "manual_state":
            if (candidate_id is not None or parents or
                    item["reason_code"] not in ("manual_requested",
                                               "manual_not_requested") or
                    bpm != 0 or
                    any(item["metrics"][x] != int(item["metrics"][x]) or
                        not 0 <= item["metrics"][x] <= 8192
                        for x in ("score", "support"))):
                raise TraceError("malformed native manual capture state")
        elif stage == "primary_eligibility":
            if candidate_id is None or ids.get(candidate_id) != "candidate_transform":
                raise TraceError("primary pool includes candidate not transformed")
        elif stage == "policy_filter":
            if (candidate_id is None or ids.get(candidate_id) != "candidate_transform" or
                    parents or item["metrics"]["support"] != 0 or
                    abs(bpm - candidate_bpm[candidate_id]) >= .001):
                raise TraceError("Policy rail source identity mismatch")
            resolved = item["metrics"]["score"]
            eligible = 40 <= resolved <= 220
            if (item["outcome"] != ("included" if eligible else "excluded") or
                    item["reason_code"] != ("policy_rail_allowed" if eligible else
                                            "policy_rail_decisive_veto")):
                raise TraceError("Policy rail verdict contradicts native resolved BPM")
        elif stage == "policy_resolution":
            if candidate_id is not None or item["reason_code"] not in ALLOWED_POLICY_COMPARISONS:
                raise TraceError("invalid policy comparison")
            if len(parents) > 1 or any(ids[p] != "policy_eligibility" for p in parents):
                raise TraceError("fabricated policy same-BPM source")
            matched = item["reason_code"] == "policy_one_same_bpm"
            if matched != (len(parents) == 1) or (
                    matched and abs(candidate_bpm[parents[0]] - bpm) > .001):
                raise TraceError("false unique policy BPM comparison")
            if (item["reason_code"] == "policy_no_output") != (bpm == 0):
                raise TraceError("policy output presence mismatch")
            if item["metrics"]["score"] not in (0, 1) or item["metrics"]["support"] not in (0, 1):
                raise TraceError("policy source flags must be booleans")
        elif stage == "policy_pointer":
            if (candidate_id is not None or
                    item["reason_code"] not in ALLOWED_POINTER_REASONS or
                    len(parents) > 1 or
                    any(ids[p] not in ("policy_eligibility", "supplemental_candidate")
                        for p in parents) or
                    item["metrics"]["score"] not in (0, 1) or
                    item["metrics"]["support"] not in (0, 1)):
                raise TraceError("malformed native policy pointer evidence")
            fullboard_pointer = item["reason_code"] == "pointer_native_fullboard_consistent"
            supplemental_pointer = item["reason_code"] == "pointer_native_supplemental_consistent"
            if ((fullboard_pointer or supplemental_pointer) != (len(parents) == 1) or
                    (fullboard_pointer and ids[parents[0]] != "policy_eligibility") or
                    (supplemental_pointer and ids[parents[0]] != "supplemental_candidate") or
                    ((fullboard_pointer or supplemental_pointer) and
                     abs(candidate_bpm[parents[0]] - bpm) > 0.001)):
                raise TraceError("policy pointer identity not verified")
        elif stage == "primary_disposition":
            if (candidate_id is None or ids.get(candidate_id) != "candidate_transform" or
                    parents or item["reason_code"] not in ALLOWED_DISPOSITION_REASONS or
                    any(item["metrics"][key] != 0 for key in ("score", "support"))):
                raise TraceError("invalid native candidate disposition")
        elif stage == "primary_first_exclusion":
            # Native source index is preserved; support is a one-based index
            # into the full executed selector-event stream, not a confidence.
            position = item["metrics"]["support"]
            absent = item["reason_code"] == "no_native_exclusion_observed"
            if (candidate_id is None or ids.get(candidate_id) != "candidate_transform" or
                    parents or abs(bpm - candidate_bpm[candidate_id]) >= .001 or
                    type(position) not in (int, float) or
                    position != int(position) or not 0 <= position <= 4096 or
                    (absent and (item["outcome"] != "observed" or position != 0 or
                                 item["metrics"]["score"] != 0)) or
                    (not absent and (item["outcome"] != "excluded" or
                                     position == 0 or
                                     item["reason_code"] not in ALLOWED_FILTER_REASONS))):
                raise TraceError("malformed native first-exclusion source witness")
        elif stage in ("candidate_filter", "primary_rank", "primary_compare", "primary_terminal"):
            # Actual executed selector probes, with explicit raw/cluster source.
            reason_set = (ALLOWED_FILTER_REASONS if stage == "candidate_filter"
                          else ALLOWED_RANK_REASONS if stage == "primary_rank"
                          else ALLOWED_COMPARE_REASONS if stage == "primary_compare"
                          else ALLOWED_TERMINAL_REASONS)
            if item["reason_code"] not in reason_set:
                raise TraceError("unknown selector-veto/rank reason")
            expect_raw = item["reason_code"].startswith("raw_")
            if (candidate_id is None or ids.get(candidate_id) !=
                    ("candidate_create" if expect_raw else "candidate_transform")):
                raise TraceError("selector event not linked to correct source board")
            if stage in ("candidate_filter", "primary_compare") and item["outcome"] not in ("included", "excluded"):
                raise TraceError("invalid selector gate/comparison outcome")
            if stage == "primary_compare":
                expected_outcome = ("included" if item["reason_code"].endswith("_won")
                                    else "excluded")
                if item["outcome"] != expected_outcome or parents:
                    raise TraceError("native comparison result inconsistent")
            fallback_predecessor = {
                "raw_alias_fallback_readmitted": "raw_alias_provisional_exclusion",
                "raw_support_fallback_readmitted": "raw_support_provisional_exclusion",
                "low_threshold_fallback_readmitted": "low_threshold_provisional_exclusion",
                "general_rail_fallback_readmitted": "general_out_of_rail_provisional",
            }
            if item["reason_code"] in fallback_predecessor:
                predecessor = fallback_predecessor[item["reason_code"]]
                if (item["outcome"] != "included" or
                    not any(previous["candidate_id"] == candidate_id and
                            previous["reason_code"] == predecessor and
                            previous["outcome"] == "excluded"
                            for previous in stage_indices["candidate_filter"])):
                    raise TraceError("readmission without executed exclusion")
            if item["reason_code"].endswith("_allowed") and item["outcome"] != "included":
                raise TraceError("positive selector gate changed to exclusion")
            if (item["reason_code"].endswith("_provisional_exclusion") and
                    item["outcome"] != "excluded"):
                raise TraceError("negative selector gate changed to inclusion")
        elif candidate_id is not None:
            raise TraceError("non-creator stage unexpectedly creates candidate")
        if stage == "candidate_transform" and not parents:
            raise TraceError("cluster lacks raw-source parents")
        if stage == "candidate_transform" and any(ids[p] != "candidate_create" for p in parents):
            raise TraceError("cluster parent is not raw")
        if stage == "policy_eligibility" and any(ids[p] != "candidate_transform" for p in parents):
            raise TraceError("policy candidate parent not collapsed")
        stage_indices[stage].append(item)
    # Reconcile EVERY cluster's first executed exclusion against the complete
    # selector event stream, including raw events and rank/compare episodes.
    # A provisional exclusion is never silently promoted to a decisive veto.
    first_rows = stage_indices["primary_first_exclusion"]
    clusters_for_first = stage_indices["candidate_transform"]
    if first_rows or require_windows:
        if len(first_rows) != len(clusters_for_first):
            raise TraceError("native first-exclusion inventory incomplete")
        executed = [r for r in rows if r["stage"] in (
            "candidate_filter", "primary_rank", "primary_compare", "primary_terminal")]
        for cluster, observed in zip(clusters_for_first, first_rows):
            cid = cluster["candidate_id"]
            if observed["candidate_id"] != cid:
                raise TraceError("native first-exclusion source order drift")
            first = next(((index + 1, r) for index, r in enumerate(executed)
                          if r["stage"] == "candidate_filter" and
                          r["candidate_id"] == cid and r["outcome"] == "excluded"),
                         None)
            if first is None:
                if (observed["outcome"] != "observed" or
                        observed["reason_code"] != "no_native_exclusion_observed" or
                        observed["metrics"]["support"] != 0 or
                        observed["metrics"]["score"] != 0):
                    raise TraceError("fabricated first native cluster exclusion")
            else:
                index, source = first
                if (observed["outcome"] != "excluded" or
                        observed["reason_code"] != source["reason_code"] or
                        observed["metrics"]["support"] != index or
                        observed["metrics"]["score"] != source["metrics"]["score"]):
                    raise TraceError("native first-exclusion chronology or score drift")
    # Independent source-ID and time-ordered proof. The first provisional
    # exclusion is not a veto until the productive fallback has resolved.
    raw_gates = [r for r in stage_indices["candidate_filter"]
                 if ids[r["candidate_id"]] == "candidate_create"]
    decisive = [r for r in raw_gates if r["reason_code"] in (
        "raw_alias_decisive_veto", "raw_support_decisive_veto")]
    raw_veto_ids = set()
    for row in decisive:
        cid = row["candidate_id"]
        if cid in raw_veto_ids or row["outcome"] != "excluded":
            raise TraceError("contradictory or duplicate decisive raw veto")
        raw_veto_ids.add(cid)
        is_alias = row["reason_code"] == "raw_alias_decisive_veto"
        required = ("raw_alias_provisional_exclusion" if is_alias else
                    "raw_support_provisional_exclusion")
        history = [r for r in raw_gates if r["candidate_id"] == cid]
        if sum(r["reason_code"] == required and
               r["sequence"] < row["sequence"] for r in history) != 1:
            raise TraceError("raw veto missing unique earlier native gate")
        if any(r["reason_code"] in (
                "raw_alias_fallback_readmitted", "raw_support_fallback_readmitted")
               for r in history):
            raise TraceError("decisive veto source later or earlier readmitted")
        if is_alias and any(r["reason_code"].startswith("raw_support_")
                            for r in history):
            raise TraceError("alias-veto source entered support pool")
        if any(r["sequence"] > row["sequence"] for r in history) or any(
                r["candidate_id"] == cid for r in
                stage_indices["primary_rank"] + stage_indices["primary_compare"]):
            raise TraceError("raw veto source later ranked or observed")
        surviving = ("raw_alias_allowed" if is_alias else "raw_support_allowed")
        if not any(r["reason_code"] == surviving for r in raw_gates):
            raise TraceError("raw final veto but no alternate surviving pool")
    # Once a decisive path is demonstrated, every raw provisional exclusion
    # in that same native invocation must resolve exactly once. No inferred
    # terminal veto from a generic excluded flag is permitted.
    if decisive:
        for pending, completions in (
            ("raw_alias_provisional_exclusion",
             ("raw_alias_fallback_readmitted", "raw_alias_decisive_veto")),
            ("raw_support_provisional_exclusion",
             ("raw_support_fallback_readmitted", "raw_support_decisive_veto")),
        ):
            for row in raw_gates:
                if row["reason_code"] != pending:
                    continue
                matches = [r for r in raw_gates
                           if r["candidate_id"] == row["candidate_id"] and
                           r["sequence"] > row["sequence"] and
                           r["reason_code"] in completions]
                if len(matches) != 1:
                    raise TraceError("raw provisional fate missing or ambiguous")
    # Executed rank/comparator pairs prove actual incumbent displacement.
    # Nested low-pulse bestBy comparisons reset their independent incumbent
    # at every strictly ordered source-observed pool invocation.
    comparisons = stage_indices["primary_compare"]
    current_best = {}
    low_last_epoch = 0
    low_closed_epochs = set()
    if comparisons or require_windows:
        for index, row in enumerate(rows):
            if row["stage"] == "primary_rank" and row["reason_code"] in COMPARE_TO_RANK.values():
                prefix = next(k for k, v in COMPARE_TO_RANK.items()
                              if v == row["reason_code"])
                required_gate = {
                    "residual_high": "residual_high_gate_allowed",
                    "residual_harmonic": "residual_harmonic_gate_allowed",
                    "harmonic_direct": "harmonic_direct_gate_allowed",
                    "low_continuous": "low_continuous_allowed",
                }.get(prefix)
                if required_gate and (
                        index == 0 or rows[index-1]["stage"] != "candidate_filter" or
                        rows[index-1]["candidate_id"] != row["candidate_id"] or
                        rows[index-1]["reason_code"] != required_gate or
                        rows[index-1]["outcome"] != "included"):
                    raise TraceError("rescue comparator without actual eligible source gate")
                if index + 1 >= len(rows):
                    raise TraceError("native rank missing adjacent comparator")
                comp = rows[index + 1]
                if (comp["stage"] != "primary_compare" or
                        (prefix == "low" and comp["metrics"].get("low_pool_epoch") !=
                         row["metrics"].get("low_pool_epoch")) or
                        comp["reason_code"] not in (prefix + "_compare_won",
                                                     prefix + "_compare_lost") or
                        comp["candidate_id"] != row["candidate_id"] or
                        any(comp["metrics"][key] != row["metrics"][key]
                            for key in METRICS)):
                    raise TraceError("native rank/comparator identity or score mismatch")
            if row["stage"] != "primary_compare":
                continue
            if index == 0 or rows[index - 1]["stage"] != "primary_rank":
                raise TraceError("orphan native comparator")
            prefix = row["reason_code"].rsplit("_compare_", 1)[0]
            if prefix not in COMPARE_TO_RANK:
                raise TraceError("unknown comparator family")
            score = row["metrics"]["score"]
            won = row["outcome"] == "included"
            if prefix == "low":
                epoch = int(row["metrics"]["low_pool_epoch"])
                if epoch != low_last_epoch:
                    if epoch != low_last_epoch + 1 or epoch in low_closed_epochs:
                        raise TraceError("low-pulse bestBy pool skipped or reopened")
                    if low_last_epoch:
                        low_closed_epochs.add(low_last_epoch)
                    low_last_epoch = epoch
                comparator_key = (prefix, epoch)
            else:
                comparator_key = prefix
            uses_tie = prefix in ("raw", "general_baseline",
                                  "residual_harmonic", "harmonic_direct",
                                  "low_continuous")
            tie = tuple(row["metrics"][f"tie_{i}"] for i in range(
                5 if prefix == "low_continuous" else 4)) if uses_tie else ()
            previous = current_best.get(comparator_key)
            if previous is None:
                expected_win = True
            else:
                prior_score, prior_tie, _ = previous
                expected_win = (
                    tie > prior_tie if prefix in
                    ("residual_harmonic", "harmonic_direct", "low_continuous") else
                    (score > prior_score + 1e-9 or
                     (abs(score - prior_score) <= 1e-9 and tie > prior_tie))
                    if uses_tie else score > prior_score
                )
            if won != expected_win:
                raise TraceError("native comparator score/tie chronology contradicted")
            if won:
                current_best[comparator_key] = (score, tie, row["candidate_id"])
    for single in ("primary_select", "policy_resolution",
                   "policy_pointer", "policy_change", "final_output",
                   "measured_witness", "write_guard"):
        if len(stage_indices[single]) != 1:
            raise TraceError("missing or repeated terminal stage " + single)
    # Actual C++ board counts must agree with the parsed source rows.
    inventories = stage_indices["source_inventory"]
    if len(inventories) > 1 or (require_windows and len(inventories) != 1):
        raise TraceError("native source inventory missing or duplicated")
    if inventories:
        m = inventories[0]["metrics"]
        if any(m[k] != 0 for k in METRICS):
            raise TraceError("source inventory contains synthetic BPM evidence")
        expected_counts = {
            "raw_count": len(stage_indices["candidate_create"]),
            "collapsed_count": len(stage_indices["candidate_transform"]),
            "policy_board_count": len(stage_indices["policy_eligibility"]),
            "selector_event_count": sum(len(stage_indices[k]) for k in (
                "candidate_filter", "primary_rank", "primary_compare",
                "primary_terminal")),
            "decode_window_count": len(stage_indices["decode_window"]),
            "supplemental_count": len(stage_indices["supplemental_candidate"]),
        }
        for name, expected_count in expected_counts.items():
            if (m[name] != int(m[name]) or
                    not 0 <= m[name] <= MAX_LINES or m[name] != expected_count):
                raise TraceError("native source inventory/JSONL cardinality mismatch")
        supply_keys = INVENTORY_METRICS - METRICS - set(expected_counts)
        if any(m[k] != int(m[k]) or not 0 <= m[k] <= MAX_LINES * 16
               for k in supply_keys):
            raise TraceError("invalid native MIR source supply counter")
        masks = [RAW_SOURCE_MASKS[row["reason_code"]]
                 for row in stage_indices["candidate_create"]]
        for bit, name in ((1, "topk"), (2, "alias"), (4, "continuous")):
            used = sum(bool(mask & bit) for mask in masks)
            if (used < m[f"mir_{name}_new_raw"] or
                    used > m[f"mir_{name}_add_calls"] * (16 if bit == 1 else 1)):
                raise TraceError("raw source origin contradicts native producer")
        if (m["mir_topk_add_calls"] > m["mir_topk_rows_seen"] or
                m["mir_topk_new_raw"] > m["mir_topk_add_calls"] * 16 or
                m["mir_alias_new_raw"] > m["mir_alias_add_calls"] or
                m["mir_continuous_new_raw"] > m["mir_continuous_add_calls"] or
                sum(m[f"mir_{name}_new_raw"] for name in
                    ("topk", "alias", "continuous")) != m["raw_count"] or
                m["mir_segment_results"] > m["mir_segments_evaluated"] or
                m["mir_family_rank_rows"] > 128 or
                (m["mir_family_rank_rows"] == 0 and
                 (m["mir_alias_add_calls"] or m["mir_continuous_add_calls"])) or
                (m["mir_segments_evaluated"] == 0 and
                 (m["mir_topk_rows_seen"] or m["mir_topk_add_calls"]))):
            raise TraceError("native MIR producer/consumer count conservation failed")
    resources = stage_indices["resource_usage"]
    if len(resources) > 1 or (require_windows and len(resources) != 1):
        raise TraceError("native process resource observation missing or duplicated")
    windows = stage_indices["decode_window"]
    if len(windows) > 128:
        raise TraceError("too many observed windows")
    declared_passes = 0
    intervals: list[tuple[float, float]] = []
    total_decoded = total_requested = 0.0
    for index, row in enumerate(windows):
        m = row["metrics"]
        for key in ("pass_index", "pass_total", "requested_frames",
                    "decoded_frames", "analysis_frames",
                    "source_rate_hz", "analysis_rate_hz"):
            if m[key] != int(m[key]):
                raise TraceError("fractional window counter")
        if index == 0:
            declared_passes = int(m["pass_total"])
        source_rate, analysis_rate = m["source_rate_hz"], m["analysis_rate_hz"]
        if (m["pass_index"] != index or m["pass_total"] != declared_passes or
                not 1 <= declared_passes <= 128 or
                not 8000 <= source_rate <= 384000 or
                not 8000 <= analysis_rate <= min(48000, source_rate) or
                not 0 <= m["start_sec"] <= 86400 or
                not 0 < m["requested_frames"] <= source_rate * 600 or
                not 0 < m["decoded_frames"] <= m["requested_frames"] or
                not 0 < m["analysis_frames"] or
                m["analysis_frames"] / analysis_rate >
                    m["decoded_frames"] / source_rate + .10 or
                any(m[key] != 0 for key in METRICS) or
                (index and m["start_sec"] < windows[index-1]["metrics"]["start_sec"])):
            raise TraceError("invalid actual decode window")
        start = m["start_sec"]
        duration = m["decoded_frames"] / source_rate
        intervals.append((start, start + duration))
        total_decoded += duration
        total_requested += m["requested_frames"] / source_rate
    if require_windows and (not windows or len(windows) != declared_passes):
        raise TraceError("native window capture absent or incomplete")
    if inventories and windows and (
            inventories[0]["metrics"]["mir_segments_evaluated"] > len(windows)):
        raise TraceError("MIR evaluated more segments than decoded windows")
    segments = stage_indices["mir_segment"]
    if segments or require_segments:
        if not inventories:
            raise TraceError("MIR evaluator has no native source inventory")
        native = inventories[0]["metrics"]
        if (len(segments) != native["mir_segments_evaluated"] or
                sum(row["metrics"]["top_k_count"] for row in segments) !=
                    native["mir_topk_rows_seen"] or
                sum(row["metrics"]["aggregated"] for row in segments) !=
                    native["mir_segment_results"]):
            raise TraceError("MIR first-return/TopK/source counts do not conserve")
        previous_index = -1
        for row in segments:
            index = int(row["metrics"]["segment_index"])
            if index <= previous_index or index >= len(windows):
                raise TraceError("MIR evaluator segment order/window identity drift")
            previous_index = index
    covered = 0.0
    end = -1.0
    for start, stop in intervals:
        covered += max(0.0, stop - max(start, end))
        end = max(end, stop)
    raw = stage_indices["candidate_create"]
    collapsed = stage_indices["candidate_transform"]
    primary_pool = stage_indices["primary_eligibility"]
    rail_filters = stage_indices["policy_filter"]
    fullboard = stage_indices["policy_eligibility"]
    supplemental = stage_indices["supplemental_candidate"]
    if len(supplemental) > 1:
        raise TraceError("multiple selected supplemental objects")
    # An actual no-tempo negative can legitimately produce zero raw and
    # collapsed MIR candidates. Prove its ENTIRE no-candidate/no-write
    # terminal chain rather than treating it as an absent/broken trace.
    # This remains a distinct incomplete-causal outcome, never a B PASS.
    if not raw and not collapsed:
        forbidden = ("primary_eligibility", "policy_filter", "policy_eligibility",
                     "supplemental_candidate", "candidate_filter",
                     "primary_rank", "primary_compare", "primary_terminal",
                     "primary_disposition")
        if any(stage_indices[name] for name in forbidden):
            raise TraceError("empty MIR fabricated candidate provenance")
        primary = stage_indices["primary_select"][0]
        resolution = stage_indices["policy_resolution"][0]
        pointer = stage_indices["policy_pointer"][0]
        policy = stage_indices["policy_change"][0]
        final = stage_indices["final_output"][0]
        witness = stage_indices["measured_witness"][0]
        guard = stage_indices["write_guard"][0]
        terminals = (primary, resolution, pointer, policy, final, witness, guard)
        if any(row["candidate_id"] is not None or row["parent_candidate_ids"]
               or any(row["metrics"][key] != 0 for key in METRICS)
               for row in terminals):
            raise TraceError("empty MIR terminal has fabricated candidate or BPM")
        if (primary["outcome"], primary["reason_code"]) not in (
                ("no_candidate", "primary_no_candidate"),
                ("review_hold", "primary_review_hold")):
            raise TraceError("empty MIR primary not held/no-candidate")
        if (resolution["outcome"], resolution["reason_code"]) != (
                "observed", "policy_no_output"):
            raise TraceError("empty MIR fabricated Policy comparison")
        if (pointer["outcome"], pointer["reason_code"]) != (
                "observed", "pointer_no_output"):
            raise TraceError("empty MIR fabricated Policy pointer")
        if (policy["outcome"] not in ("review_hold", "rejected") or
                policy["reason_code"] != "no_policy_write"):
            raise TraceError("empty MIR Policy would write")
        if (final["outcome"], final["reason_code"]) != (
                "review_hold", "final_abstained"):
            raise TraceError("empty MIR fabricated final acceptance")
        if (witness["outcome"], witness["reason_code"]) != (
                "rejected", "not_accepted"):
            raise TraceError("empty MIR fabricated measured witness")
        if (guard["outcome"], guard["reason_code"]) != (
                "write_blocked", "ci_dry_run"):
            raise TraceError("empty MIR media write safety drift")
        if not windows or len(windows) != declared_passes or len(resources) != 1:
            raise TraceError("empty MIR native window/resource evidence incomplete")
        try:
            manual_status = reconcile_manual_export(rows, required=require_windows)
        except ManualTraceError as exc:
            raise TraceError("empty native manual-export chain: " + str(exc)) from exc
        try:
            writer_status = reconcile_writer_resolution(
                rows, required=require_writer_resolution)
        except WriterResolutionError as exc:
            raise TraceError("applied native writer chain: " + str(exc)) from exc
        return {
            **writer_status,
            **manual_status,
            "rows": len(rows), "raw": 0, "collapsed": 0,
            "primary_pool": 0, "policy_board": 0,
            "decoded_windows": len(windows), "declared_windows": declared_passes,
            "windows_complete": 1,
            "requested_window_seconds": round(total_requested, 6),
            "decoded_window_seconds": round(total_decoded, 6),
            "unique_covered_seconds": round(covered, 6),
            "repeat_factor": round(total_decoded / covered, 6) if covered else 0.0,
            "process_wall_seconds": round(resources[0]["metrics"]["wall_seconds"], 6),
            "process_cpu_seconds": round(resources[0]["metrics"]["process_cpu_seconds"], 6),
            "process_peak_rss_bytes": int(
                resources[0]["metrics"]["process_peak_rss_after_bytes"]),
            "native_source_inventory_verified": int(bool(inventories)),
            "native_empty_board_boundary_verified": int(bool(inventories)),
            "mir_segment_reason_counts": ",".join(
                f"{reason}:{sum(x['reason_code'] == reason for x in segments)}"
                for reason in sorted({x["reason_code"] for x in segments})),
            "mir_segment_native_verified": int(bool(segments) and require_segments),
            "negative_no_candidate_trace": 1,
            "primary_auto": 0, "final_accept": 0,
            "write_guard_blocked": 1,
            "phase_b_causal_completeness": "INCOMPLETE_EMPTY_BOARD_CAUSAL",
        }
    if not raw or not collapsed:
        raise TraceError("missing raw/collapsed MIR")
    # Independently replay collapse_mir_candidates' ordered first-match
    # partition. Numeric proximity alone is NOT a valid many-to-many lineage
    # proof: one raw candidate can be close to two distinct cluster centers.
    expected_sources = [[] for _ in collapsed]
    overlapping_raw = 0
    for source in raw:
        bpm = source["metrics"]["bpm"]
        matches = [i for i, cluster in enumerate(collapsed)
                   if abs(cluster["metrics"]["bpm"] - bpm) < .001]
        if not matches:
            raise TraceError("raw candidate orphaned by native collapse")
        overlapping_raw += len(matches) > 1
        expected_sources[matches[0]].append(source["candidate_id"])
    for cluster, parents in zip(collapsed, expected_sources):
        if (not parents or cluster["parent_candidate_ids"] != parents or
                cluster["metrics"]["bpm"] != candidate_bpm[parents[0]]):
            raise TraceError("raw-to-cluster first-match partition drift")
    if len(primary_pool) != len(collapsed) or not all(
            left["candidate_id"] == right["candidate_id"]
            for left, right in zip(primary_pool, collapsed)):
        raise TraceError("primary input pool not fully represented")
    if require_policy_filter and len(rail_filters) != len(collapsed):
        raise TraceError("native Policy rail source inventory missing")
    if rail_filters:
        if len(rail_filters) != len(collapsed):
            raise TraceError("partial Policy rail source inventory")
        for source, gate in zip(collapsed, rail_filters):
            if gate["candidate_id"] != source["candidate_id"]:
                raise TraceError("Policy rail source-index order drift")
    # FullBoard may legitimately exclude out-of-rail clusters, but cannot
    # invent, omit or duplicate a surviving candidate's source identity.
    seen_policy_sources = set()
    for row in fullboard:
        parents = row["parent_candidate_ids"]
        if (row["reason_code"] != "full_board_member" or len(parents) != 1 or
                parents[0] in seen_policy_sources or
                ids[parents[0]] != "candidate_transform"):
            raise TraceError("policy FullBoard source is not unique and indexed")
        seen_policy_sources.add(parents[0])
    if rail_filters:
        by_source = {row["parent_candidate_ids"][0]: row for row in fullboard}
        for gate in rail_filters:
            member = by_source.get(gate["candidate_id"])
            if (member is None) != (gate["outcome"] == "excluded"):
                raise TraceError("Policy rail/FullBoard membership contradiction")
            if member is not None and abs(member["metrics"]["bpm"] -
                                          gate["metrics"]["score"]) > .001:
                raise TraceError("Policy rail/FullBoard resolved BPM mismatch")
    dispositions = stage_indices["primary_disposition"]
    if (require_windows and len(dispositions) != len(collapsed)) or (
            dispositions and len(dispositions) != len(collapsed)):
        raise TraceError("per-candidate disposition incomplete")
    if dispositions:
        ranked_ids = {x["candidate_id"] for x in stage_indices["primary_rank"]}
        checked_ids = {
            x["candidate_id"] for x in
            (stage_indices["candidate_filter"] + stage_indices["primary_terminal"])
            if ids[x["candidate_id"]] == "candidate_transform"
        }
        terminal_ids = [x["candidate_id"] for x in stage_indices["primary_terminal"]
                        if x["reason_code"] == "terminal_final_choice"]
        if len(terminal_ids) > 1:
            raise TraceError("duplicate final selected object")
        for cluster, row in zip(collapsed, dispositions):
            cid = cluster["candidate_id"]
            expected = ("selected_final_object" if cid in terminal_ids else
                        "ranked_not_final" if cid in ranked_ids else
                        "observed_not_ranked" if cid in checked_ids else
                        "no_cluster_observation")
            if (row["candidate_id"] != cid or row["reason_code"] != expected or
                    abs(row["metrics"]["bpm"] - cluster["metrics"]["bpm"]) >= 0.001):
                raise TraceError("native per-candidate executed-path disposition drift")
    primary = stage_indices["primary_select"][0]
    policy_origin = stage_indices["policy_resolution"][0]
    policy_pointer = stage_indices["policy_pointer"][0]
    policy = stage_indices["policy_change"][0]
    final = stage_indices["final_output"][0]
    witness = stage_indices["measured_witness"][0]
    guard = stage_indices["write_guard"][0]
    if (primary["outcome"] == "selected" and
            (not primary["parent_candidate_ids"] or
             any(ids[p] != "candidate_transform" or
                 abs(candidate_bpm[p] - primary["metrics"]["bpm"]) >= .001
                 for p in primary["parent_candidate_ids"]))):
        raise TraceError("selected primary candidate has no matching clustered witness")
    terminal = stage_indices["primary_terminal"]
    # The General-144 alternative is a TWO-stage decision: native baseline
    # comparator, then a bounded 126..141.5 challenger and its 1.20 evidence
    # promotion gate. Source-indexed candidate events make it impossible to
    # treat a ranked challenger as automatically selected.
    general = [r for r in stage_indices["candidate_filter"]
               if r["reason_code"].startswith("general_alt_") and
               not r["reason_code"].startswith("general_alt_window_")]
    if require_windows and any(
            r["reason_code"].startswith("general_alt_window_")
            for r in stage_indices["candidate_filter"]) and not any(
            r["reason_code"] == "general_alt_baseline_source" for r in general):
        raise TraceError("host General alternative window lacks native baseline")
    baseline = [r for r in general
                if r["reason_code"] == "general_alt_baseline_source"]
    resolutions = [r for r in general if r["reason_code"] in (
        "general_alt_bypass_route", "general_alt_bypass_non144",
        "general_alt_no_challenger", "general_alt_challenger_promoted",
        "general_alt_challenger_rejected")]
    if general and (len(baseline) != 1 or len(resolutions) != 1 or not terminal):
        raise TraceError("General alternative source/resolution incomplete")
    if baseline:
        base_id = baseline[0]["candidate_id"]
        verdict = resolutions[0]
        reason = verdict["reason_code"]
        if (baseline[0]["outcome"] != "included" or
                verdict["sequence"] <= baseline[0]["sequence"]):
            raise TraceError("General baseline source chronology invalid")
        alt_ranks = [r for r in stage_indices["primary_rank"]
                     if r["reason_code"] == "general_alt_rank_scored"]
        alt_compares = [r for r in comparisons
                        if r["reason_code"].startswith("general_alt_compare_")]
        if reason in ("general_alt_bypass_route", "general_alt_bypass_non144",
                      "general_alt_no_challenger"):
            if (verdict["candidate_id"] != base_id or
                    verdict["outcome"] != "included" or
                    alt_ranks or alt_compares or
                    any(r["reason_code"] in (
                        "general_alt_baseline_evidence",
                        "general_alt_challenger_evidence") for r in general)):
                raise TraceError("General bypass/no-challenger falsely ranked")
            expected_lane_id = base_id
        else:
            base_ev = [r for r in general
                       if r["reason_code"] == "general_alt_baseline_evidence"]
            alt_ev = [r for r in general
                      if r["reason_code"] == "general_alt_challenger_evidence"]
            if (len(base_ev) != 1 or len(alt_ev) != 1 or
                    base_ev[0]["candidate_id"] != base_id or
                    alt_ev[0]["candidate_id"] != verdict["candidate_id"] or
                    not (base_ev[0]["sequence"] < alt_ev[0]["sequence"] <
                         verdict["sequence"]) or
                    base_ev[0]["outcome"] != "included" or
                    alt_ev[0]["outcome"] != "included" or
                    not alt_ranks or not alt_compares):
                raise TraceError("General promotion has no measured source pair")
            base_score = base_ev[0]["metrics"]["score"]
            alt_score = alt_ev[0]["metrics"]["score"]
            promoted = alt_score >= base_score * 1.20
            if (abs(alt_score - verdict["metrics"]["score"]) > 1e-8 or
                    verdict["outcome"] != ("included" if promoted else "excluded") or
                    reason != ("general_alt_challenger_promoted" if promoted else
                               "general_alt_challenger_rejected") or
                    current_best.get("general_alt", (None, None, None))[2] !=
                        verdict["candidate_id"]):
                raise TraceError("General promotion ratio/native challenger contradicted")
            expected_lane_id = verdict["candidate_id"] if promoted else base_id
        if terminal[0]["candidate_id"] != expected_lane_id:
            raise TraceError("General alternative terminal contradicts native promotion")
    # Replay the actual terminal lane object against the final winner of
    # each executed comparator pool. A ranked loser cannot become the lane
    # terminal by simply retaining a valid candidate ID and matching BPM.
    # Raw lanes select a collapsed object by the raw winner's first-match
    # parent; nested low-pulse bestBy pools each have their own incumbent.
    # The separate low continuous-center rescue is only a source-indexed
    # allowed gate (no comparator tuple), so it is explicitly UNPROVEN.
    lane_comparator_winner_proven = 0
    lane_continuous_rescue_unranked = 0
    if terminal and comparisons:
        lane_id = terminal[0]["candidate_id"]
        winning_clusters = {
            state[2] for family, state in current_best.items()
            if family != "raw" and
            (family[0] if isinstance(family, tuple) else family)
            not in ("residual_high", "residual_harmonic", "harmonic_direct")
        }
        winning_raw = {
            state[2] for family, state in current_best.items()
            if family == "raw"
        }
        lane_cluster = next(
            (cluster for cluster in collapsed
             if cluster["candidate_id"] == lane_id), None)
        raw_parent_won = bool(lane_cluster and any(
            parent in winning_raw
            for parent in lane_cluster["parent_candidate_ids"]))
        continuous_allowed = any(
            row["candidate_id"] == lane_id and
            row["reason_code"] == "low_continuous_allowed" and
            row["outcome"] == "included"
            for row in stage_indices["candidate_filter"])
        continuous_winner = current_best.get("low_continuous")
        if continuous_winner and lane_id != continuous_winner[2]:
            raise TraceError("native low continuous terminal not the executed five-tuple winner")
        if lane_id in winning_clusters or raw_parent_won:
            lane_comparator_winner_proven = 1
        elif continuous_allowed and not require_native_comparator:
            # Legacy unit-only traces remain explicitly unqualified.
            lane_continuous_rescue_unranked = 1
        else:
            raise TraceError("terminal lane candidate was not a native comparator winner")
    if require_native_comparator and terminal and not comparisons:
        raise TraceError("native lane terminal has no executed comparator provenance")
    # Reconstruct the two *later* native terminal transitions independently.
    # A provisional allowed gate alone is not a winner; only an executed,
    # ordered comparator can change the selected source object.
    residual_winner_proven = 0
    direct_winner_proven = 0
    if terminal:
        terminal_positions = [i for i, row in enumerate(rows)
                              if row["stage"] == "primary_terminal"]
        if len(terminal_positions) == 3:
            lane_pos, residual_pos, final_pos = terminal_positions
            residual_keys = ("residual_high", "residual_harmonic")
            residual_winners = [
                current_best[k][2] for k in residual_keys if k in current_best
            ]
            if len(residual_winners) > 1:
                raise TraceError("mutually exclusive residual rescue families combined")
            expected_residual = (residual_winners[0] if residual_winners
                                 else terminal[0]["candidate_id"])
            if terminal[1]["candidate_id"] != expected_residual:
                raise TraceError("residual terminal not backed by executed winner")
            residual_winner_proven = int(bool(residual_winners))
            expected_final = (current_best["harmonic_direct"][2]
                              if "harmonic_direct" in current_best
                              else expected_residual)
            if terminal[2]["candidate_id"] != expected_final:
                raise TraceError("final terminal not backed by executed direct rescue")
            direct_winner_proven = int("harmonic_direct" in current_best)
            for index, row in enumerate(rows):
                if row["stage"] not in ("primary_rank", "primary_compare"):
                    continue
                family = row["reason_code"].split("_rank_", 1)[0] if (
                    row["stage"] == "primary_rank") else (
                    row["reason_code"].split("_compare_", 1)[0])
                if (require_native_comparator and
                        family not in (*residual_keys, "harmonic_direct") and
                        index > lane_pos):
                    raise TraceError("lane comparator after native lane terminal")
                if family in residual_keys and not lane_pos < index < residual_pos:
                    raise TraceError("residual comparator outside native terminal phase")
                if family == "harmonic_direct" and not residual_pos < index < final_pos:
                    raise TraceError("direct comparator outside native terminal phase")
    if terminal:
        actual = [row["reason_code"] for row in terminal]
        allowed = ("terminal_lane_choice", "terminal_residual_choice",
                   "terminal_final_choice")
        if actual != list(allowed):
            raise TraceError("selector terminal order/count incomplete")
        if any(ids[x["candidate_id"]] != "candidate_transform" for x in terminal):
            raise TraceError("selector terminal missing clustered candidate ID")
        if not primary["metrics"]["bpm"] and primary["outcome"] == "selected":
            raise TraceError("selector selected without terminal BPM")
        if (primary["parent_candidate_ids"] != [terminal[-1]["candidate_id"]] or
                abs(terminal[-1]["metrics"]["bpm"] -
                    primary["metrics"]["bpm"]) >= 0.001):
            raise TraceError("primary selected candidate is not exact terminal object")
    expected_primary_reason = {
        "selected": "primary_auto",
        "review_hold": "primary_review_hold",
        "no_candidate": "primary_no_candidate",
    }
    if (primary["outcome"] not in expected_primary_reason or
            primary["reason_code"] != expected_primary_reason[primary["outcome"]] or
            (primary["outcome"] == "selected" and primary["metrics"]["bpm"] <= 0)):
        raise TraceError("primary outcome/reason inconsistent")
    # Native host capture must prove all three executed terminal snapshots.
    # Synthetic minimal unit traces remain permitted without them.
    if require_windows and primary["metrics"]["bpm"] > 0 and not terminal:
        raise TraceError("host selected primary BPM without executed terminal provenance")
    if require_windows and not primary["metrics"]["bpm"] and terminal:
        raise TraceError("host terminal selected without primary BPM")
    if policy["parent_candidate_ids"] != policy_pointer["parent_candidate_ids"]:
        raise TraceError("policy chosen pointer must be independently witnessed")
    if abs(policy_pointer["metrics"]["bpm"] - policy["metrics"]["bpm"]) > 0.001:
        raise TraceError("policy pointer/output BPM mismatch")
    pointer_reason = policy_pointer["reason_code"]
    pointer_native = pointer_reason == "pointer_native_fullboard_consistent"
    pointer_supplemental = pointer_reason == "pointer_native_supplemental_consistent"
    if pointer_supplemental and (not supplemental or
            policy_pointer["parent_candidate_ids"] != [supplemental[0]["candidate_id"]]):
        raise TraceError("supplemental pointer has no native selected source")
    pointer_seen = int(policy_pointer["metrics"]["score"])
    pointer_in_board = int(policy_pointer["metrics"]["support"])
    if (pointer_native and
        (not pointer_seen or not pointer_in_board or
         policy["metrics"]["bpm"] <= 0)):
        raise TraceError("unsupported native policy selected pointer")
    if (pointer_supplemental and
            (not pointer_seen or pointer_in_board or policy["metrics"]["bpm"] <= 0)):
        raise TraceError("supplemental policy pointer flags inconsistent")
    if (pointer_reason == "pointer_not_observed" and pointer_seen):
        raise TraceError("policy pointer marked not observed yet seen")
    if (pointer_reason == "pointer_no_output") != (
            policy["metrics"]["bpm"] == 0):
        raise TraceError("policy pointer output absence inconsistent")
    if pointer_in_board and not pointer_seen:
        raise TraceError("policy pointer cannot be in board if unobserved")
    if abs(policy_origin["metrics"]["bpm"] - policy["metrics"]["bpm"]) > 0.001:
        raise TraceError("policy output/compare drift")
    # Source comparison is not chosen-pointer identity. Recompute the exact
    # matching-BPM multiplicity to catch false uniqueness and hidden ties.
    matching = [x["candidate_id"] for x in fullboard
                if policy["metrics"]["bpm"] > 0 and
                abs(x["metrics"]["bpm"] - policy["metrics"]["bpm"]) <= .001]
    source_reason = policy_origin["reason_code"]
    expected = ("policy_no_output" if policy["metrics"]["bpm"] == 0 else
                "policy_one_same_bpm" if len(matching) == 1 else
                "policy_no_fullboard_match" if not matching else
                "policy_multiple_same_bpm")
    if source_reason != expected or (
            len(matching) == 1 and
            policy_origin["parent_candidate_ids"] != matching):
        raise TraceError("policy fullboard exact-BPM multiplicity drift")
    override = bool(policy_origin["metrics"]["score"])
    release = bool(policy_origin["metrics"]["support"])
    # Orthogonal override/release flags are not disjoint root causes:
    # the native reporter prioritizes writer_override when both are true.
    expected_policy_reason = (
        "writer_override" if override else
        "hold_release" if release else
        "keep_primary" if policy["outcome"] == "selected" else
        "no_policy_write"
    )
    if policy["reason_code"] != expected_policy_reason:
        raise TraceError("policy action/override/release flags inconsistent")
    if final["reason_code"] not in ("final_numeric", "final_abstained"):
        raise TraceError("unapproved final decision reason")
    if (policy["outcome"] == "selected" and
            policy["reason_code"] not in ("writer_override", "hold_release",
                                          "keep_primary")):
        raise TraceError("policy selected without explicit action")
    if final["outcome"] not in ("selected", "review_hold") or (
            final["outcome"] == "selected" and final["metrics"]["bpm"] <= 0) or (
            final["outcome"] == "review_hold" and final["reason_code"] != "final_abstained"):
        raise TraceError("incorrect final candidate/abstention")
    if final["outcome"] == "selected":
        if witness["outcome"] != "included" or abs(
                witness["metrics"]["bpm"] - final["metrics"]["bpm"]) > 0.001:
            raise TraceError("final accepted BPM lacks native measured witness")
        if (pointer_supplemental and
                abs(policy["metrics"]["bpm"] - final["metrics"]["bpm"]) <= 0.001 and
                witness["reason_code"] != "indexed_native_witness"):
            raise TraceError("selected supplemental final requires indexed witness")
        if witness["reason_code"] == "indexed_native_witness":
            if not witness["parent_candidate_ids"] or any(
                    ids[p] not in ("policy_eligibility", "candidate_transform",
                                  "supplemental_candidate")
                    or abs(candidate_bpm[p] - final["metrics"]["bpm"]) > 0.001
                    for p in witness["parent_candidate_ids"]):
                raise TraceError("native indexed witness invalid")
            supplemental_witness_ids = [
                p for p in witness["parent_candidate_ids"]
                if ids[p] == "supplemental_candidate"
            ]
            if supplemental_witness_ids and (
                    not pointer_supplemental or
                    policy_pointer["parent_candidate_ids"] != supplemental_witness_ids):
                raise TraceError("supplemental final witness lacks native selected object")
            if (pointer_supplemental and
                    abs(policy["metrics"]["bpm"] - final["metrics"]["bpm"]) <= 0.001 and
                    policy_pointer["parent_candidate_ids"][0]
                        not in witness["parent_candidate_ids"]):
                raise TraceError("native chosen supplemental source missing in final witness")
        elif witness["reason_code"] != "supplemental_witness_unindexed" or (
                witness["parent_candidate_ids"]):
            raise TraceError("final measured witness origin invalid")
    elif (witness["outcome"] != "rejected" or
          witness["reason_code"] != "not_accepted" or
          witness["parent_candidate_ids"]):
        raise TraceError("withheld BPM must not claim measured acceptance")
    if (guard["outcome"] != "write_blocked" or
            guard["reason_code"] != "ci_dry_run" or
            any(guard["metrics"][name] != 0 for name in METRICS)):
        raise TraceError("media write safety drift")
    # FIRST observed cluster-level exclusions are actual source-indexed gate
    # events, NOT terminal vetoes. Later fallbacks may re-admit a candidate;
    # raw-only lanes need not check every collapsed cluster.
    cluster_gates = [
        row for row in stage_indices["candidate_filter"]
        if ids[row["candidate_id"]] == "candidate_transform"
    ]
    checked_cluster_ids = {row["candidate_id"] for row in cluster_gates}
    first_exclusion = {}
    for row in cluster_gates:
        cid = row["candidate_id"]
        if row["outcome"] == "excluded" and cid not in first_exclusion:
            first_exclusion[cid] = row["reason_code"]
    ranked_cluster_ids = {
        row["candidate_id"] for row in stage_indices["primary_rank"]
        if ids[row["candidate_id"]] == "candidate_transform"
    }
    readmitted_cluster_ids = {
        row["candidate_id"] for row in cluster_gates
        if row["reason_code"].endswith("_fallback_readmitted")
    }
    incomplete = sum(x["reason_code"] == "pool_only_gate_unresolved"
                     for x in primary_pool)
    # Strict original-host raw source-inventory conservation. Legacy small
    # test fixtures are deliberately not claimed as native qualifications.
    raw_fates = {}
    if require_windows:
        try:
            raw_fates = reconcile_native_raw(rows)
        except RawFateError as exc:
            raise TraceError("incomplete or contradictory native raw fate: " +
                             str(exc)) from exc
        try:
            rail_fates = reconcile_native_rail(rows)
        except RailFateError as exc:
            raise TraceError("incomplete or contradictory native rail fate: " +
                             str(exc)) from exc
        try:
            low_upper_fates = reconcile_native_low_upper(rows)
        except LowUpperFateError as exc:
            raise TraceError("incomplete or contradictory native low upper fate: " +
                             str(exc)) from exc
        try:
            continuous_fates = reconcile_native_low_continuous(rows)
        except LowContinuousFateError as exc:
            raise TraceError("incomplete or contradictory native low continuous fate: " +
                             str(exc)) from exc
    else:
        rail_fates = {}
        low_upper_fates = {}
        continuous_fates = {}
    try:
        manual_status = reconcile_manual_export(rows, required=require_windows)
    except ManualTraceError as exc:
        raise TraceError("native manual-export lineage: " + str(exc)) from exc
    try:
        writer_status = reconcile_writer_resolution(
            rows, required=require_writer_resolution)
    except WriterResolutionError as exc:
        raise TraceError("applied native writer chain: " + str(exc)) from exc
    return {
        **writer_status,
        **manual_status,
        **raw_fates,
        **rail_fates,
        **low_upper_fates,
        **continuous_fates,
        "rows": len(rows), "raw": len(raw), "collapsed": len(collapsed),
        "native_source_inventory_verified": int(bool(inventories)),
        "native_empty_board_boundary_verified": 0,
        "mir_segment_reason_counts": ",".join(
            f"{reason}:{sum(x['reason_code'] == reason for x in segments)}"
            for reason in sorted({x["reason_code"] for x in segments})),
        "mir_segment_native_verified": int(bool(segments) and require_segments),
        "raw_cluster_partition_verified": len(raw),
        "raw_lineage_overlap_disambiguated": overlapping_raw,
        "policy_board_unique_cluster_sources": len(seen_policy_sources),
        "policy_board_filtered_clusters": len(collapsed) - len(seen_policy_sources),
        "policy_rail_source_observations": len(rail_filters),
        "policy_rail_decisive_vetos": sum(x["outcome"] == "excluded" for x in rail_filters),
        "policy_rail_native_complete": int(bool(rail_filters) and len(rail_filters) == len(collapsed)),
        "process_wall_seconds": round(resources[0]["metrics"]["wall_seconds"], 6) if resources else None,
        "process_cpu_seconds": round(resources[0]["metrics"]["process_cpu_seconds"], 6) if resources else None,
        "process_peak_rss_bytes": int(resources[0]["metrics"]["process_peak_rss_after_bytes"]) if resources else None,
        "decoded_windows": len(windows), "declared_windows": declared_passes,
        "windows_complete": int(bool(windows) and len(windows) == declared_passes),
        "requested_window_seconds": round(total_requested, 6),
        "decoded_window_seconds": round(total_decoded, 6),
        "unique_covered_seconds": round(covered, 6),
        "repeat_factor": round(total_decoded / covered, 6) if covered else 0.0,
        "primary_pool": len(primary_pool), "policy_board": len(fullboard),
        "primary_first_exclusion_native_complete": int(
            bool(first_rows) and len(first_rows) == len(collapsed)),
        "primary_first_exclusion_sources": sum(
            r["outcome"] == "excluded" for r in first_rows),
        "primary_first_exclusion_no_observation": sum(
            r["outcome"] == "observed" for r in first_rows),
        "primary_veto_reason_unknown": incomplete,
        "selector_cluster_gate_checked": len(checked_cluster_ids),
        "selector_cluster_gate_unvisited": len(collapsed) - len(checked_cluster_ids),
        "selector_cluster_first_exclusion_observed": len(first_exclusion),
        "selector_cluster_excluded_then_ranked": sum(
            cid in ranked_cluster_ids for cid in first_exclusion),
        "selector_cluster_excluded_then_readmitted": sum(
            cid in readmitted_cluster_ids for cid in first_exclusion),
        "selector_cluster_first_exclusion_reasons": ",".join(
            f"{reason}:{sum(r == reason for r in first_exclusion.values())}"
            for reason in sorted(set(first_exclusion.values()))),
        "selector_raw_decisive_alias_vetos": sum(
            r["reason_code"] == "raw_alias_decisive_veto" for r in decisive),
        "selector_raw_decisive_support_vetos": sum(
            r["reason_code"] == "raw_support_decisive_veto" for r in decisive),
        "selector_raw_decisive_source_ids": len(raw_veto_ids),
        "selector_branch_checks": len(stage_indices["candidate_filter"]),
        "selector_fallback_readmissions": sum(
            x["reason_code"].endswith("_fallback_readmitted")
            for x in stage_indices["candidate_filter"]),
        "selector_nested_rescue_checks": sum(
            x["reason_code"].startswith(("residual_high_", "residual_harmonic_",
                                           "harmonic_direct_", "low_mid_",
                                           "low_lower_mid_", "low_midlow_",
                                           "low_deeplow_", "low_upper_direct_",
                                           "low_verylow_", "low_upper_recovery_",
                                           "general_alt_window_"))
            for x in stage_indices["candidate_filter"]),
        "selector_rank_observations": len(stage_indices["primary_rank"]),
        "selector_comparator_observations": len(comparisons),
        "selector_comparator_families": len(set(
            prefix[0] if isinstance(prefix, tuple) else prefix for prefix in current_best)),
        "selector_low_bestby_episodes": low_last_epoch,
        "selector_comparator_wins": sum(x["outcome"] == "included" for x in comparisons),
        "selector_comparator_losses": sum(x["outcome"] == "excluded" for x in comparisons),
        # Per-family actual executed comparator outcomes, never a claim of
        # full selector eligibility or causal first-veto coverage.
        "selector_compare_raw_wins": sum(
            x["reason_code"] == "raw_compare_won" for x in comparisons),
        "selector_compare_raw_losses": sum(
            x["reason_code"] == "raw_compare_lost" for x in comparisons),
        "selector_compare_harmonic_wins": sum(
            x["reason_code"] == "harmonic_compare_won" for x in comparisons),
        "selector_compare_harmonic_losses": sum(
            x["reason_code"] == "harmonic_compare_lost" for x in comparisons),
        "selector_compare_general_baseline_wins": sum(
            x["reason_code"] == "general_baseline_compare_won" for x in comparisons),
        "selector_compare_general_baseline_losses": sum(
            x["reason_code"] == "general_baseline_compare_lost" for x in comparisons),
        "selector_compare_general_alt_wins": sum(
            x["reason_code"] == "general_alt_compare_won" for x in comparisons),
        "selector_compare_general_alt_losses": sum(
            x["reason_code"] == "general_alt_compare_lost" for x in comparisons),
        "selector_compare_low_continuous_wins": sum(
            x["reason_code"] == "low_continuous_compare_won"
            for x in comparisons),
        "selector_compare_low_continuous_losses": sum(
            x["reason_code"] == "low_continuous_compare_lost"
            for x in comparisons),
        "selector_compare_low_wins": sum(
            x["reason_code"] == "low_compare_won" for x in comparisons),
        "selector_compare_low_losses": sum(
            x["reason_code"] == "low_compare_lost" for x in comparisons),
        "selector_terminal_observations": len(stage_indices["primary_terminal"]),
        "selector_lane_comparator_winner_proven": lane_comparator_winner_proven,
        "selector_lane_continuous_rescue_unranked": lane_continuous_rescue_unranked,
        "selector_residual_winner_proven": residual_winner_proven,
        "selector_direct_winner_proven": direct_winner_proven,
        "selector_residual_comparator_observations": sum(
            row["reason_code"].startswith(("residual_high_", "residual_harmonic_"))
            for row in comparisons),
        "selector_direct_comparator_observations": sum(
            row["reason_code"].startswith("harmonic_direct_")
            for row in comparisons),
        "selector_disposition_observations": len(dispositions),
        "disposition_ranked_not_final": sum(
            row["reason_code"] == "ranked_not_final" for row in dispositions),
        "disposition_observed_not_ranked": sum(
            row["reason_code"] == "observed_not_ranked" for row in dispositions),
        "disposition_no_cluster_observation": sum(
            row["reason_code"] == "no_cluster_observation" for row in dispositions),
        "supplemental_selected_sources": len(supplemental),
        "policy_supplemental_pointer_identity_complete": int(pointer_supplemental),
        "policy_origin_unknown": sum(x["reason_code"] == "full_board_origin_unknown"
                                     for x in fullboard),
        "policy_output_source_comparison": policy_origin["reason_code"],
        "policy_override_active": int(override),
        "policy_hold_release_active": int(release),
        "policy_pointer_identity_complete": int(pointer_native),
        "policy_pointer_reason": pointer_reason,
        "primary_auto": int(primary["outcome"] == "selected"),
        "final_accept": int(final["outcome"] == "selected"),
        "indexed_native_final_witness": int(
            final["outcome"] == "selected" and
            witness["reason_code"] == "indexed_native_witness"),
        "indexed_supplemental_final_witness": int(
            final["outcome"] == "selected" and
            witness["reason_code"] == "indexed_native_witness" and
            any(ids[p] == "supplemental_candidate"
                for p in witness["parent_candidate_ids"])),
        "unindexed_supplemental_final_witness": int(
            final["outcome"] == "selected" and
            witness["reason_code"] == "supplemental_witness_unindexed"),
        "write_guard_blocked": 1,
        "phase_b_causal_completeness": "INCOMPLETE_PRIMARY_VETO"
    }


def load(path: Path) -> list[dict]:
    if not path.is_file() or not 0 < path.stat().st_size <= MAX_BYTES:
        raise TraceError("trace file missing/oversized")
    with path.open(encoding="utf-8", newline="") as stream:
        raw = stream.read()
    if not raw.endswith("\n") or "\r" in raw or len(raw.splitlines()) > MAX_LINES:
        raise TraceError("trace framing error")
    def unique_pairs(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise TraceError("duplicate JSON trace key")
            result[key] = value
        return result
    try:
        return [json.loads(line, object_pairs_hook=unique_pairs,
                           parse_constant=lambda x: (_ for _ in ()).throw(
                               TraceError("nonfinite literal")))
                for line in raw.splitlines()]
    except (json.JSONDecodeError, UnicodeError) as exc:
        raise TraceError("invalid JSON trace") from exc


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--trace", required=True, type=Path)
    parser.add_argument("--require-windows", action="store_true")
    a = parser.parse_args()
    result = validate(load(a.trace), require_windows=a.require_windows,
                      require_segments=a.require_windows,
                      require_native_comparator=a.require_windows,
                      require_policy_filter=a.require_windows,
                      require_writer_resolution=a.require_windows)
    print("SMART_TEMPO_CI_PIPELINE_TRACE_SCHEMA=PASS")
    for key, value in result.items():
        print(f"SMART_TEMPO_TRACE_{key.upper()}={value}")


if __name__ == "__main__":
    main()
