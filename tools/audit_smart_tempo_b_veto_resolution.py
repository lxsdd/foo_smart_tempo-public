#!/usr/bin/env python3
"""Independent native cluster branch-veto/final-source reconciliation.

CI-only: branch-local decisive gates are NOT globally final exclusions.
A later residual rescue can select the same object. No music or labels read.
"""
from __future__ import annotations

VETO_REASONS = frozenset(("harmonic_rail_decisive_veto",
                          "general_rail_decisive_veto"))
PRECURSOR = {
    "harmonic_rail_decisive_veto": "harmonic_out_of_rail",
    "general_rail_decisive_veto": "general_out_of_rail_provisional",
}


class VetoResolutionError(ValueError):
    pass


def reconcile_native_veto_resolution(rows: list[dict], *,
                                     required: bool = False) -> dict:
    clusters = [r for r in rows if r["stage"] == "candidate_transform"]
    summaries = [r for r in rows if r["stage"] == "primary_veto_resolution"]
    result = {
        "primary_veto_resolution_native_complete": 0,
        "primary_veto_resolution_sources": 0,
        "primary_veto_resolution_branch_vetos": 0,
        "primary_veto_resolution_selected_after_branch_veto": 0,
        "primary_veto_resolution_selected_without_branch_veto": 0,
        "primary_veto_resolution_no_veto_observed": 0,
    }
    if not summaries and not required:
        return result
    if len(summaries) != len(clusters):
        raise VetoResolutionError("cluster branch-veto inventory missing or duplicated")
    events = [r for r in rows if r["stage"] in (
        "candidate_filter", "primary_rank", "primary_compare", "primary_terminal")]
    terminal = [r for r in events if r["stage"] == "primary_terminal"
                and r["reason_code"] == "terminal_final_choice"]
    if len(terminal) > 1:
        raise VetoResolutionError("multiple native final choices")
    final_id = terminal[0]["candidate_id"] if terminal else None
    for cluster, summary in zip(clusters, summaries):
        cid = cluster["candidate_id"]
        if (summary["candidate_id"] != cid or
                summary["parent_candidate_ids"] or
                abs(summary["metrics"]["bpm"] - cluster["metrics"]["bpm"]) >= .001):
            raise VetoResolutionError("branch-veto source/order/BPM mismatch")
        vetoes = [(index + 1, row) for index, row in enumerate(events)
                  if row["stage"] == "candidate_filter" and
                  row["candidate_id"] == cid and
                  row["reason_code"] in VETO_REASONS]
        if len(vetoes) > 1:
            raise VetoResolutionError("duplicate native branch veto")
        first = vetoes[0] if vetoes else None
        selected = cid == final_id
        if first:
            position, source = first
            if source["outcome"] != "excluded":
                raise VetoResolutionError("native branch veto not excluded")
            predecessor = PRECURSOR[source["reason_code"]]
            earlier = [r for r in events if r["stage"] == "candidate_filter"
                       and r["candidate_id"] == cid and
                       r["reason_code"] == predecessor and
                       r["sequence"] < source["sequence"]]
            if len(earlier) != 1:
                raise VetoResolutionError("native branch veto lacks unique earlier gate")
            if (source["reason_code"] == "general_rail_decisive_veto" and
                    any(r["candidate_id"] == cid and
                        r["reason_code"] == "general_rail_fallback_readmitted"
                        for r in events)):
                raise VetoResolutionError("general branch veto also readmitted")
            expected_reason = source["reason_code"]
            expected_score = source["metrics"]["score"]
            expected_position = position
        else:
            expected_reason = ("selected_without_native_decisive_veto" if selected
                               else "no_native_decisive_veto_observed")
            expected_score = 0
            expected_position = 0
        expected_outcome = ("selected" if selected else
                            "excluded" if first else "observed")
        if (summary["reason_code"] != expected_reason or
                summary["outcome"] != expected_outcome or
                summary["metrics"]["score"] != expected_score or
                summary["metrics"]["support"] != expected_position):
            raise VetoResolutionError("native branch-veto/final-choice chronology drift")
        result["primary_veto_resolution_sources"] += 1
        result["primary_veto_resolution_branch_vetos"] += int(bool(first))
        result["primary_veto_resolution_selected_after_branch_veto"] += int(
            selected and bool(first))
        result["primary_veto_resolution_selected_without_branch_veto"] += int(
            selected and not first)
        result["primary_veto_resolution_no_veto_observed"] += int(
            not selected and not first)
    result["primary_veto_resolution_native_complete"] = int(
        len(summaries) == len(clusters))
    return result
