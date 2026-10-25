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
WINDOW_METRICS = METRICS | frozenset(("pass_index", "pass_total", "start_sec",
    "requested_frames", "decoded_frames", "analysis_frames",
    "source_rate_hz", "analysis_rate_hz"))
CREATORS = frozenset(("candidate_create", "candidate_transform", "policy_eligibility"))
ENUMS = {
    "decode_window": ("observed", "actual_decoded_extent"),
    "candidate_create": ("observed", "raw_alias"),
    "candidate_transform": ("observed", "cluster_from_raw"),
    "primary_eligibility": ("observed", "pool_only_gate_unresolved"),
    "candidate_filter": (None, None),
    "primary_rank": ("included", None),
    "primary_terminal": ("selected", None),
    "policy_eligibility": ("included", None),
    "primary_select": (None, None),
    "policy_change": (None, None),
    "final_output": (None, None),
    "measured_witness": (None, None),
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
    "low_continuous_allowed", "low_continuous_provisional_exclusion"
))
ALLOWED_RANK_REASONS = frozenset((
    "raw_rank_scored", "harmonic_rank_scored",
    "general_baseline_rank_scored", "general_alt_rank_scored"
))
ALLOWED_TERMINAL_REASONS = frozenset((
    "terminal_lane_choice", "terminal_residual_choice",
    "terminal_final_choice"
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
        "candidate_filter": 3, "primary_rank": 3,
        "primary_terminal": 3, "primary_select": 4,
        "policy_change": 5, "final_output": 6,
        "measured_witness": 7, "write_guard": 8
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
        expected_metrics = WINDOW_METRICS if stage == "decode_window" else METRICS
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
        if stage == "decode_window" and (candidate_id is not None or parents):
            raise TraceError("window must not contain candidate identity")
        if stage in CREATORS:
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
        elif stage in ("candidate_filter", "primary_rank", "primary_terminal"):
            # Actual executed selector probes, with explicit raw/cluster source.
            reason_set = (ALLOWED_FILTER_REASONS if stage == "candidate_filter"
                          else ALLOWED_RANK_REASONS if stage == "primary_rank"
                          else ALLOWED_TERMINAL_REASONS)
            if item["reason_code"] not in reason_set:
                raise TraceError("unknown selector-veto/rank reason")
            expect_raw = item["reason_code"].startswith("raw_")
            if (candidate_id is None or ids.get(candidate_id) !=
                    ("candidate_create" if expect_raw else "candidate_transform")):
                raise TraceError("selector event not linked to correct source board")
            if stage == "candidate_filter" and item["outcome"] not in ("included", "excluded"):
                raise TraceError("invalid selector gate outcome")
        elif candidate_id is not None:
            raise TraceError("non-creator stage unexpectedly creates candidate")
        if stage == "candidate_transform" and not parents:
            raise TraceError("cluster lacks raw-source parents")
        if stage == "candidate_transform" and any(ids[p] != "candidate_create" for p in parents):
            raise TraceError("cluster parent is not raw")
        if stage == "policy_eligibility" and any(ids[p] != "candidate_transform" for p in parents):
            raise TraceError("policy candidate parent not collapsed")
        stage_indices[stage].append(item)
    for single in ("primary_select", "policy_change", "final_output",
                   "measured_witness", "write_guard"):
        if len(stage_indices[single]) != 1:
            raise TraceError("missing or repeated terminal stage " + single)
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
    if not raw or not collapsed:
        raise TraceError("missing raw/collapsed MIR")
    if len(primary_pool) != len(collapsed) or not all(
            left["candidate_id"] == right["candidate_id"]
            for left, right in zip(primary_pool, collapsed)):
        raise TraceError("primary input pool not fully represented")
    if any(row["reason_code"] not in ("full_board_member","full_board_origin_unknown")
           for row in fullboard):
        raise TraceError("policy board source unknown")
    primary = stage_indices["primary_select"][0]
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
    if primary["reason_code"] not in (
            "primary_auto", "primary_review_hold", "primary_no_candidate"):
        raise TraceError("unapproved primary decision reason")
    if policy["reason_code"] not in (
            "writer_override", "hold_release", "keep_primary", "no_policy_write"):
        raise TraceError("unapproved policy decision reason")
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
        if witness["reason_code"] == "indexed_native_witness":
            if not witness["parent_candidate_ids"] or any(
                    ids[p] not in ("policy_eligibility", "candidate_transform")
                    or abs(candidate_bpm[p] - final["metrics"]["bpm"]) > 0.001
                    for p in witness["parent_candidate_ids"]):
                raise TraceError("native indexed witness invalid")
        elif witness["reason_code"] != "supplemental_witness_unindexed" or (
                witness["parent_candidate_ids"]):
            raise TraceError("final measured witness origin invalid")
    elif (witness["outcome"] != "rejected" or
          witness["reason_code"] != "not_accepted" or
          witness["parent_candidate_ids"]):
        raise TraceError("withheld BPM must not claim measured acceptance")
    if (guard["outcome"] != "write_blocked" or
            guard["reason_code"] != "ci_dry_run"):
        raise TraceError("media write safety drift")
    # A pool candidate's presence is NEVER evidence it cleared branch-specific
    # selector vetoes. This remains an explicit *known* gap until instrumented.
    incomplete = sum(x["reason_code"] == "pool_only_gate_unresolved"
                     for x in primary_pool)
    return {
        "rows": len(rows), "raw": len(raw), "collapsed": len(collapsed),
        "decoded_windows": len(windows), "declared_windows": declared_passes,
        "windows_complete": int(bool(windows) and len(windows) == declared_passes),
        "requested_window_seconds": round(total_requested, 6),
        "decoded_window_seconds": round(total_decoded, 6),
        "unique_covered_seconds": round(covered, 6),
        "repeat_factor": round(total_decoded / covered, 6) if covered else 0.0,
        "primary_pool": len(primary_pool), "policy_board": len(fullboard),
        "primary_veto_reason_unknown": incomplete,
        "selector_branch_checks": len(stage_indices["candidate_filter"]),
        "selector_rank_observations": len(stage_indices["primary_rank"]),
        "selector_terminal_observations": len(stage_indices["primary_terminal"]),
        "policy_origin_unknown": sum(x["reason_code"] == "full_board_origin_unknown"
                                     for x in fullboard),
        "primary_auto": int(primary["outcome"] == "selected"),
        "final_accept": int(final["outcome"] == "selected"),
        "indexed_native_final_witness": int(
            final["outcome"] == "selected" and
            witness["reason_code"] == "indexed_native_witness"),
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
