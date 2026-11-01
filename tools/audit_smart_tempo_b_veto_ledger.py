#!/usr/bin/env python3
"""Exact ordered replay of native branch-local selector vetoes; no global eligibility inference."""
from __future__ import annotations
import math

VETO_REASONS = frozenset((
    "raw_alias_decisive_veto", "raw_support_decisive_veto",
    "low_upper_branch_veto", "low_continuous_branch_veto",
    "harmonic_rail_decisive_veto", "general_rail_decisive_veto",
))
RAW_REASONS = frozenset(("raw_alias_decisive_veto", "raw_support_decisive_veto"))
SELECTOR_STAGES = frozenset((
    "candidate_filter", "primary_rank", "primary_compare", "primary_terminal",
))

class VetoLedgerError(ValueError):
    pass

def reconcile_native_veto_ledger(rows: list[dict], *, required: bool = False) -> dict:
    events = [r for r in rows if r["stage"] in SELECTOR_STAGES]
    ledger = [r for r in rows if r["stage"] == "primary_veto_ledger"]
    actual = [(i + 1, r) for i, r in enumerate(events)
              if r["stage"] == "candidate_filter"
              and r["reason_code"] in VETO_REASONS]
    if not required and not ledger:
        return {"native_veto_ledger_complete": 0, "native_veto_ledger_events": 0,
                "native_veto_ledger_raw": 0, "native_veto_ledger_cluster": 0,
                "native_veto_ledger_selected_after_branch_veto": 0}
    if len(ledger) != len(actual):
        raise VetoLedgerError("missing or duplicated native veto event")
    origins = {r["candidate_id"]: r for r in rows if r["stage"] in
               ("candidate_create", "candidate_transform")}
    terminal = [r for r in events if r["stage"] == "primary_terminal"
                and r["reason_code"] == "terminal_final_choice"]
    if len(terminal) > 1:
        raise VetoLedgerError("ambiguous terminal source")
    final_id = terminal[0]["candidate_id"] if terminal else None
    for (position, event), witness in zip(actual, ledger):
        cid = event["candidate_id"]
        expected_origin = ("candidate_create" if event["reason_code"] in RAW_REASONS
                           else "candidate_transform")
        source = origins.get(cid)
        metrics = witness["metrics"]
        if (source is None or source["stage"] != expected_origin or
                event["outcome"] != "excluded" or
                witness["outcome"] != "excluded" or
                witness["reason_code"] != event["reason_code"] or
                witness["candidate_id"] != cid or
                witness["parent_candidate_ids"] != [] or
                metrics["support"] != position or
                not math.isfinite(metrics["score"]) or
                metrics["score"] != event["metrics"]["score"] or
                metrics["bpm"] != event["metrics"]["bpm"] or
                abs(metrics["bpm"] - source["metrics"]["bpm"]) >= .001):
            raise VetoLedgerError("native veto event/source/index/score mismatch")
    return {
        "native_veto_ledger_complete": 1,
        "native_veto_ledger_events": len(actual),
        "native_veto_ledger_raw": sum(r["reason_code"] in RAW_REASONS
                                      for _, r in actual),
        "native_veto_ledger_cluster": sum(r["reason_code"] not in RAW_REASONS
                                          for _, r in actual),
        "native_veto_ledger_selected_after_branch_veto": sum(
            r["candidate_id"] == final_id and r["reason_code"] not in RAW_REASONS
            for _, r in actual),
    }
