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

SCHEMA = "smart_tempo_source_decision_trace_v1"
MAX_BYTES, MAX_LINES = 2 * 1024 * 1024, 12300
FIELDS = frozenset(("schema", "analysis_id", "sequence", "stage", "outcome",
                    "reason_code", "candidate_id", "parent_candidate_ids", "metrics"))
METRICS = frozenset(("bpm", "score", "support"))
TIE_METRICS = METRICS | frozenset(("tie_0", "tie_1", "tie_2", "tie_3"))
RESOURCE_METRICS = METRICS | frozenset((
    "wall_seconds", "process_cpu_seconds",
    "process_peak_rss_before_bytes", "process_peak_rss_after_bytes",
    "process_working_set_after_bytes"))
WINDOW_METRICS = METRICS | frozenset(("pass_index", "pass_total", "start_sec",
    "requested_frames", "decoded_frames", "analysis_frames",
    "source_rate_hz", "analysis_rate_hz"))
CREATORS = frozenset(("candidate_create", "candidate_transform",
                      "policy_eligibility", "supplemental_candidate"))
ENUMS = {
    "decode_window": ("observed", "actual_decoded_extent"),
    "candidate_create": ("observed", "raw_alias"),
    "candidate_transform": ("observed", "cluster_from_raw"),
    "primary_eligibility": ("observed", "pool_only_gate_unresolved"),
    "candidate_filter": (None, None),
    "primary_rank": ("included", None),
    "primary_compare": (None, None),
    "primary_terminal": ("selected", None),
    "primary_disposition": ("observed", None),
    "policy_eligibility": ("included", None),
    "supplemental_candidate": ("observed", None),
    "primary_select": (None, None),
    "policy_resolution": ("observed", None),
    "policy_pointer": ("observed", None),
    "policy_change": (None, None),
    "final_output": (None, None),
    "measured_witness": (None, None),
    "resource_usage": ("observed", "process_resource_snapshot"),
    "write_guard": ("write_blocked", "ci_dry_run")
}
REASON_PATTERN = re.compile(r"^[a-z][a-z0-9_]{0,95}$")
ID_PATTERN = re.compile(r"^c(?:0|[1-9][0-9]{0,6})$")
ALLOWED_FILTER_REASONS = frozenset((
    "raw_alias_allowed", "raw_alias_provisional_exclusion",
    "raw_support_allowed", "raw_support_provisional_exclusion",
    "low_upper_gate_allowed", "low_upper_gate_provisional_exclusion",
    "low_threshold_allowed", "low_threshold_provisional_exclusion",
    "harmonic_in_rail", "harmonic_out_of_rail",
    "general_in_rail", "general_out_of_rail_provisional",
    "low_continuous_allowed", "low_continuous_provisional_exclusion",
    "low_threshold_fallback_readmitted", "general_rail_fallback_readmitted",
    "residual_high_gate_allowed", "residual_high_gate_provisional_exclusion",
    "residual_harmonic_gate_allowed", "residual_harmonic_gate_provisional_exclusion",
    "harmonic_direct_gate_allowed", "harmonic_direct_gate_provisional_exclusion"
))
ALLOWED_RANK_REASONS = frozenset((
    "raw_rank_scored", "harmonic_rank_scored", "low_rank_scored",
    "general_baseline_rank_scored", "general_alt_rank_scored"
))
COMPARE_TO_RANK = {
    "raw": "raw_rank_scored",
    "harmonic": "harmonic_rank_scored",
    "general_baseline": "general_baseline_rank_scored",
    "general_alt": "general_alt_rank_scored"
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


def validate(rows: list[dict], *, require_windows: bool = False) -> dict:
    if not isinstance(rows, list) or not 8 <= len(rows) <= MAX_LINES:
        raise TraceError("trace missing or out of row bound")
    ids: dict[str, str] = {}
    candidate_bpm: dict[str, float] = {}
    stage_indices: dict[str, list[dict]] = {name: [] for name in ENUMS}
    stage_order = {
        "decode_window": -1,
        "candidate_create": 0, "candidate_transform": 1,
        "primary_eligibility": 1, "policy_eligibility": 2,
        "supplemental_candidate": 2,
        "candidate_filter": 3, "primary_rank": 3, "primary_compare": 3,
        "primary_terminal": 3, "primary_disposition": 3,
        "primary_select": 4,
        "policy_resolution": 5, "policy_pointer": 6,
        "policy_change": 7, "final_output": 8,
        "measured_witness": 9, "resource_usage": 10,
        "write_guard": 11
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
        has_native_tie = (stage == "primary_compare" and
                          item["reason_code"].startswith(("raw_compare_",
                                                          "general_baseline_compare_")))
        expected_metrics = (WINDOW_METRICS if stage == "decode_window" else
                            RESOURCE_METRICS if stage == "resource_usage" else
                            TIE_METRICS if has_native_tie else METRICS)
        if type(item["metrics"]) is not dict or set(item["metrics"]) != expected_metrics:
            raise TraceError("non-numeric-only telemetry")
        for key, value in item["metrics"].items():
            if type(value) not in (float, int) or not math.isfinite(value):
                raise TraceError("nonfinite or nonnumeric metrics")
        bpm = item["metrics"]["bpm"]
        if bpm < 0 or bpm > (220 if stage in ("final_output", "policy_change") else 2000):
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
        if stage in ("decode_window", "resource_usage") and (candidate_id is not None or parents):
            raise TraceError("window/resource must not contain candidate identity")
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
        elif stage == "primary_eligibility":
            if candidate_id is None or ids.get(candidate_id) != "candidate_transform":
                raise TraceError("primary pool includes candidate not transformed")
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
            if item["reason_code"] in ("low_threshold_fallback_readmitted",
                                       "general_rail_fallback_readmitted"):
                predecessor = ("low_threshold_provisional_exclusion" if
                               item["reason_code"].startswith("low_") else
                               "general_out_of_rail_provisional")
                if (item["outcome"] != "included" or
                    not any(previous["candidate_id"] == candidate_id and
                            previous["reason_code"] == predecessor and
                            previous["outcome"] == "excluded"
                            for previous in stage_indices["candidate_filter"])):
                    raise TraceError("readmission without executed exclusion")
        elif candidate_id is not None:
            raise TraceError("non-creator stage unexpectedly creates candidate")
        if stage == "candidate_transform" and not parents:
            raise TraceError("cluster lacks raw-source parents")
        if stage == "candidate_transform" and any(ids[p] != "candidate_create" for p in parents):
            raise TraceError("cluster parent is not raw")
        if stage == "policy_eligibility" and any(ids[p] != "candidate_transform" for p in parents):
            raise TraceError("policy candidate parent not collapsed")
        stage_indices[stage].append(item)
    # Executed rank/comparator pairs prove actual incumbent displacement.
    # A low-pulse rank is not yet a comparator proof.
    comparisons = stage_indices["primary_compare"]
    current_best = {}
    if comparisons or require_windows:
        for index, row in enumerate(rows):
            if row["stage"] == "primary_rank" and row["reason_code"] in COMPARE_TO_RANK.values():
                prefix = next(k for k, v in COMPARE_TO_RANK.items()
                              if v == row["reason_code"])
                if index + 1 >= len(rows):
                    raise TraceError("native rank missing adjacent comparator")
                comp = rows[index + 1]
                if (comp["stage"] != "primary_compare" or
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
            uses_tie = prefix in ("raw", "general_baseline")
            tie = tuple(row["metrics"][f"tie_{i}"] for i in range(4)) if uses_tie else ()
            previous = current_best.get(prefix)
            if previous is None:
                expected_win = True
            else:
                prior_score, prior_tie = previous
                expected_win = (
                    score > prior_score + 1e-9 or
                    (abs(score - prior_score) <= 1e-9 and tie > prior_tie)
                ) if uses_tie else score > prior_score
            if won != expected_win:
                raise TraceError("native comparator score/tie chronology contradicted")
            if won:
                current_best[prefix] = (score, tie)
    for single in ("primary_select", "policy_resolution",
                   "policy_pointer", "policy_change", "final_output",
                   "measured_witness", "write_guard"):
        if len(stage_indices[single]) != 1:
            raise TraceError("missing or repeated terminal stage " + single)
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
    covered = 0.0
    end = -1.0
    for start, stop in intervals:
        covered += max(0.0, stop - max(start, end))
        end = max(end, stop)
    raw = stage_indices["candidate_create"]
    collapsed = stage_indices["candidate_transform"]
    primary_pool = stage_indices["primary_eligibility"]
    fullboard = stage_indices["policy_eligibility"]
    supplemental = stage_indices["supplemental_candidate"]
    if len(supplemental) > 1:
        raise TraceError("multiple selected supplemental objects")
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
    return {
        "rows": len(rows), "raw": len(raw), "collapsed": len(collapsed),
        "raw_cluster_partition_verified": len(raw),
        "raw_lineage_overlap_disambiguated": overlapping_raw,
        "policy_board_unique_cluster_sources": len(seen_policy_sources),
        "policy_board_filtered_clusters": len(collapsed) - len(seen_policy_sources),
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
        "selector_branch_checks": len(stage_indices["candidate_filter"]),
        "selector_fallback_readmissions": sum(
            x["reason_code"].endswith("_fallback_readmitted")
            for x in stage_indices["candidate_filter"]),
        "selector_nested_rescue_checks": sum(
            x["reason_code"].startswith(("residual_high_", "residual_harmonic_", "harmonic_direct_"))
            for x in stage_indices["candidate_filter"]),
        "selector_rank_observations": len(stage_indices["primary_rank"]),
        "selector_comparator_observations": len(comparisons),
        "selector_comparator_families": len(current_best),
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
        "selector_terminal_observations": len(stage_indices["primary_terminal"]),
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
    try:
        return [json.loads(line, parse_constant=lambda x: (_ for _ in ()).throw(
            TraceError("nonfinite literal"))) for line in raw.splitlines()]
    except json.JSONDecodeError as exc:
        raise TraceError("invalid JSON trace") from exc


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--trace", required=True, type=Path)
    parser.add_argument("--require-windows", action="store_true")
    a = parser.parse_args()
    result = validate(load(a.trace), require_windows=a.require_windows)
    print("SMART_TEMPO_CI_PIPELINE_TRACE_SCHEMA=PASS")
    for key, value in result.items():
        print(f"SMART_TEMPO_TRACE_{key.upper()}={value}")


if __name__ == "__main__":
    main()
