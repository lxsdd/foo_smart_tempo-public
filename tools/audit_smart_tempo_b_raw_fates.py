#!/usr/bin/env python3
"""Independent, oracle-blind complete native RAW-selector fate automaton.

CI-only numeric source IDs. This proves an executed RAW selector invocation
under strict host mode, never a collapsed-cluster veto or musical BPM truth.
No path, track identifiers, tags, external or user-supplied data are read.
"""
from __future__ import annotations


class RawFateError(ValueError):
    pass


def reconcile_native_raw(rows: list[dict]) -> dict[str, int]:
    raw = [row for row in rows if row["stage"] == "candidate_create"]
    ids = [row["candidate_id"] for row in raw]
    bpm = {row["candidate_id"]: row["metrics"]["bpm"] for row in raw}
    if len(ids) != len(set(ids)):
        raise RawFateError("duplicate raw source IDs")
    native = [row for row in rows if row["stage"] in (
        "candidate_filter", "primary_rank", "primary_compare") and
        row["reason_code"].startswith("raw_")]
    result = {
        "native_raw_complete_invocations": 0,
        "native_raw_first_decisive_alias": 0,
        "native_raw_first_decisive_support": 0,
        "native_raw_alias_fallback_sources": 0,
        "native_raw_support_fallback_sources": 0,
        "native_raw_compared_sources": 0,
    }
    if not native:
        # A different MIR lane may never invoke the raw selector.
        return result
    if not ids:
        raise RawFateError("raw native events without input candidates")
    cursor = 0

    def take(cid: str, reason: str, included: bool, stage="candidate_filter"):
        nonlocal cursor
        if cursor >= len(native):
            raise RawFateError("truncated raw selector chronology")
        row = native[cursor]
        cursor += 1
        if (row["stage"] != stage or row["candidate_id"] != cid or
                row["reason_code"] != reason or
                row["outcome"] != ("included" if included else "excluded") or
                row["metrics"]["bpm"] != bpm[cid]):
            raise RawFateError("wrong raw source, stage, BPM, order or polarity")

    # The first alias evaluation MUST cover every native raw input, exactly
    # once and in its source order. An absent veto cannot be waved away.
    alias_winner = []
    alias_excluded = []
    for cid in ids:
        if cursor >= len(native):
            raise RawFateError("raw alias sweep incomplete")
        reason = native[cursor]["reason_code"]
        if reason == "raw_alias_allowed":
            take(cid, reason, True)
            alias_winner.append(cid)
        elif reason == "raw_alias_provisional_exclusion":
            take(cid, reason, False)
            alias_excluded.append(cid)
        else:
            raise RawFateError("raw alias sweep skipped or reordered")
    if alias_winner:
        for cid in alias_excluded:
            take(cid, "raw_alias_decisive_veto", False)
        active = alias_winner
        result["native_raw_first_decisive_alias"] = len(alias_excluded)
    else:
        for cid in ids:
            take(cid, "raw_alias_fallback_readmitted", True)
        active = ids
        result["native_raw_alias_fallback_sources"] = len(ids)

    support_winner, support_excluded = [], []
    for cid in active:
        if cursor >= len(native):
            raise RawFateError("raw support sweep incomplete")
        reason = native[cursor]["reason_code"]
        if reason == "raw_support_allowed":
            take(cid, reason, True)
            support_winner.append(cid)
        elif reason == "raw_support_provisional_exclusion":
            take(cid, reason, False)
            support_excluded.append(cid)
        else:
            raise RawFateError("raw support sweep skipped or reordered")
    if support_winner:
        for cid in support_excluded:
            take(cid, "raw_support_decisive_veto", False)
        finalists = support_winner
        result["native_raw_first_decisive_support"] = len(support_excluded)
    else:
        for cid in active:
            take(cid, "raw_support_fallback_readmitted", True)
        finalists = active
        result["native_raw_support_fallback_sources"] = len(active)

    last_winner = None
    for cid in finalists:
        take(cid, "raw_rank_scored", True, stage="primary_rank")
        if cursor >= len(native):
            raise RawFateError("raw rank missing native comparator")
        reason = native[cursor]["reason_code"]
        if reason == "raw_compare_won":
            take(cid, reason, True, stage="primary_compare")
            last_winner = cid
        elif reason == "raw_compare_lost":
            take(cid, reason, False, stage="primary_compare")
            if last_winner is None:
                raise RawFateError("raw comparator lost without incumbent")
        else:
            raise RawFateError("raw native comparator missing")
        result["native_raw_compared_sources"] += 1
    if cursor != len(native):
        raise RawFateError("extra or duplicate raw native event")
    if last_winner is None:
        raise RawFateError("nonempty native raw selector lacks winner")

    # The productive selector resolves the raw winning BPM to the *first*
    # collapsed candidate matching within 0.001. A later residual rescue
    # can change the final choice, so compare ONLY the lane terminal.
    collapsed = [row for row in rows if row["stage"] == "candidate_transform"]
    matching = [row["candidate_id"] for row in collapsed if
                abs(row["metrics"]["bpm"] - bpm[last_winner]) < .001]
    terminal = [row for row in rows if row["stage"] == "primary_terminal" and
                row["reason_code"] == "terminal_lane_choice"]
    if len(terminal) != 1 or not matching or (
            terminal[0]["candidate_id"] != matching[0]):
        raise RawFateError("raw native winning object did not reach lane terminal")
    result["native_raw_complete_invocations"] = 1
    return result
