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
CREATORS = frozenset(("candidate_create", "candidate_transform", "policy_eligibility"))
ENUMS = {
    "candidate_create": ("observed", "raw_alias"),
    "candidate_transform": ("observed", "cluster_from_raw"),
    "primary_eligibility": ("observed", "pool_only_gate_unresolved"),
    "policy_eligibility": ("included", None),
    "primary_select": (None, None),
    "policy_change": (None, None),
    "final_output": (None, None),
    "write_guard": ("write_blocked", "ci_dry_run")
}
REASON_PATTERN = re.compile(r"^[a-z][a-z0-9_]{0,95}$")
ID_PATTERN = re.compile(r"^c(?:0|[1-9][0-9]{0,6})$")


class TraceError(ValueError):
    pass


def validate(rows: list[dict]) -> dict:
    if not isinstance(rows, list) or not 8 <= len(rows) <= MAX_LINES:
        raise TraceError("trace missing or out of row bound")
    ids: dict[str, str] = {}
    stage_indices: dict[str, list[dict]] = {name: [] for name in ENUMS}
    for index, item in enumerate(rows):
        if type(item) is not dict or item.keys() != FIELDS and set(item) != FIELDS:
            raise TraceError("trace fields not exact")
        if item["schema"] != SCHEMA or item["analysis_id"] != "cafefeed01234567":
            raise TraceError("foreign trace source/session")
        if type(item["sequence"]) is not int or item["sequence"] != index:
            raise TraceError("trace sequence gap/reordering")
        stage = item["stage"]
        if stage not in ENUMS:
            raise TraceError("unknown stage")
        expected_outcome, expected_reason = ENUMS[stage]
        if (expected_outcome is not None and item["outcome"] != expected_outcome or
            expected_reason is not None and item["reason_code"] != expected_reason or
            item["outcome"] not in ("observed","included","selected","rejected",
                                    "review_hold","no_candidate","write_blocked")):
            raise TraceError("unapproved outcome/reason")
        if (not isinstance(item["reason_code"], str) or
                not REASON_PATTERN.fullmatch(item["reason_code"])):
            raise TraceError("unbounded reason")
        if type(item["metrics"]) is not dict or set(item["metrics"]) != METRICS:
            raise TraceError("non-numeric-only telemetry")
        for key, value in item["metrics"].items():
            if type(value) not in (float, int) or not math.isfinite(value):
                raise TraceError("nonfinite or nonnumeric metrics")
        bpm = item["metrics"]["bpm"]
        if bpm < 0 or bpm > 220:
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
        if stage in CREATORS:
            if candidate_id is None or candidate_id in ids:
                raise TraceError("duplicated or missing created candidate ID")
            if int(candidate_id[1:]) != len(ids):
                raise TraceError("noncontiguous candidate lineage IDs")
            ids[candidate_id] = stage
        elif stage == "primary_eligibility":
            if candidate_id is None or ids.get(candidate_id) != "candidate_transform":
                raise TraceError("primary pool includes candidate not transformed")
        elif candidate_id is not None:
            raise TraceError("non-creator stage unexpectedly creates candidate")
        if stage == "candidate_transform" and not parents:
            raise TraceError("cluster lacks raw-source parents")
        if stage == "candidate_transform" and any(ids[p] != "candidate_create" for p in parents):
            raise TraceError("cluster parent is not raw")
        if stage == "policy_eligibility" and any(ids[p] != "candidate_transform" for p in parents):
            raise TraceError("policy candidate parent not collapsed")
        stage_indices[stage].append(item)
    for single in ("primary_select", "policy_change", "final_output", "write_guard"):
        if len(stage_indices[single]) != 1:
            raise TraceError("missing or repeated terminal stage " + single)
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
    guard = stage_indices["write_guard"][0]
    if (primary["outcome"] == "selected" and
            (not primary["parent_candidate_ids"] or
             any(ids[p] != "candidate_transform" for p in primary["parent_candidate_ids"]))):
        raise TraceError("selected primary candidate has no clustered witness")
    if (policy["outcome"] == "selected" and
            policy["reason_code"] not in ("writer_override", "hold_release",
                                          "keep_primary")):
        raise TraceError("policy selected without explicit action")
    if final["outcome"] not in ("selected", "review_hold") or (
            final["outcome"] == "selected" and final["metrics"]["bpm"] <= 0) or (
            final["outcome"] == "review_hold" and final["reason_code"] != "final_abstained"):
        raise TraceError("incorrect final candidate/abstention")
    if (guard["outcome"] != "write_blocked" or
            guard["reason_code"] != "ci_dry_run"):
        raise TraceError("media write safety drift")
    # A pool candidate's presence is NEVER evidence it cleared branch-specific
    # selector vetoes. This remains an explicit *known* gap until instrumented.
    incomplete = sum(x["reason_code"] == "pool_only_gate_unresolved"
                     for x in primary_pool)
    return {
        "rows": len(rows), "raw": len(raw), "collapsed": len(collapsed),
        "primary_pool": len(primary_pool), "policy_board": len(fullboard),
        "primary_veto_reason_unknown": incomplete,
        "policy_origin_unknown": sum(x["reason_code"] == "full_board_origin_unknown"
                                     for x in fullboard),
        "primary_auto": int(primary["outcome"] == "selected"),
        "final_accept": int(final["outcome"] == "selected"),
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
    a = parser.parse_args()
    result = validate(load(a.trace))
    print("SMART_TEMPO_CI_PIPELINE_TRACE_SCHEMA=PASS")
    for key, value in result.items():
        print(f"SMART_TEMPO_TRACE_{key.upper()}={value}")


if __name__ == "__main__":
    main()
