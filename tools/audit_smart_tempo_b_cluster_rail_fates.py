#!/usr/bin/env python3
"""Independent, oracle-blind original General/Harmonic rail fate replay.

The source-indexed pass is proof of only the executed selector's rail stage,
not every future rescue/Policy decision or actual musical tempo quality.
"""


class RailFateError(ValueError):
    pass


def reconcile_native_rail(rows: list[dict]) -> dict[str, int]:
    sources = [r for r in rows if r["stage"] == "candidate_transform"]
    ids = [r["candidate_id"] for r in sources]
    bpm = {r["candidate_id"]: r["metrics"]["bpm"] for r in sources}
    rails = {
        "general_in_rail", "general_out_of_rail_provisional",
        "general_rail_fallback_readmitted", "general_rail_decisive_veto",
        "harmonic_in_rail", "harmonic_out_of_rail",
        "harmonic_rail_decisive_veto",
    }
    observed = [r for r in rows if r["stage"] == "candidate_filter"
                and r["reason_code"] in rails]
    result = {
        "native_rail_complete_invocations": 0,
        "native_general_first_decisive_rail": 0,
        "native_harmonic_first_decisive_rail": 0,
        "native_general_rail_fallback_sources": 0,
        "native_rail_compared_sources": 0,
    }
    if not observed:
        return result  # the original audio may never visit either rail
    if not ids or len(ids) != len(set(ids)):
        raise RailFateError("invalid native cluster inventory")
    general = observed[0]["reason_code"].startswith("general_")
    prefix = "general" if general else "harmonic"
    if any(not r["reason_code"].startswith(prefix+"_") for r in observed):
        raise RailFateError("multiple native rail families in one selector")
    cursor = 0

    def take(cid, reason, included):
        nonlocal cursor
        if cursor >= len(observed):
            raise RailFateError("truncated native rail event sequence")
        row = observed[cursor]
        cursor += 1
        if (row["candidate_id"] != cid or row["reason_code"] != reason or
                row["outcome"] != ("included" if included else "excluded") or
                row["metrics"]["bpm"] != bpm[cid]):
            raise RailFateError("native rail source/reason/BPM/order contradiction")

    accepted, excluded = [], []
    for cid in ids:
        if cursor >= len(observed):
            raise RailFateError("native rail initial sweep truncated")
        why = observed[cursor]["reason_code"]
        if why == prefix+"_in_rail":
            take(cid, why, True)
            accepted.append(cid)
        elif why == ("general_out_of_rail_provisional" if general else
                     "harmonic_out_of_rail"):
            take(cid, why, False)
            excluded.append(cid)
        else:
            raise RailFateError("native rail skipped candidate or changed source order")
    if general and not accepted:
        for cid in ids:
            take(cid, "general_rail_fallback_readmitted", True)
        finalists = ids
        result["native_general_rail_fallback_sources"] = len(ids)
    else:
        for cid in excluded:
            take(cid, prefix+"_rail_decisive_veto", False)
        finalists = accepted
        result["native_general_first_decisive_rail" if general else
               "native_harmonic_first_decisive_rail"] = len(excluded)
    if cursor != len(observed):
        raise RailFateError("unmatched native rail veto/readmission/duplicate")

    rank_reason = ("general_baseline_rank_scored" if general else
                   "harmonic_rank_scored")
    comp_prefix = "general_baseline" if general else "harmonic"
    ranks = [r for r in rows if r["stage"] == "primary_rank"
             and r["reason_code"] == rank_reason]
    comparisons = [r for r in rows if r["stage"] == "primary_compare"
                   and r["reason_code"] in (
                       comp_prefix+"_compare_won", comp_prefix+"_compare_lost")]
    if [r["candidate_id"] for r in ranks] != finalists or (
            [r["candidate_id"] for r in comparisons] != finalists):
        raise RailFateError("native rail survivor not ranked and compared exactly once")
    winner = None
    for cid, comparison in zip(finalists, comparisons):
        if comparison["outcome"] == "included":
            winner = cid
        elif comparison["outcome"] != "excluded" or winner is None:
            raise RailFateError("rail comparator lacks incumbent")
    # The baseline may later be replaced by the General 144 alternative.
    # Harmonic's original lane has no such alternative; residual rescue is
    # after lane_terminal and therefore does not change this check.
    terminal = [r for r in rows if r["stage"] == "primary_terminal"
                and r["reason_code"] == "terminal_lane_choice"]
    alt = any(r["stage"] == "primary_compare" and
              r["reason_code"].startswith("general_alt_compare_")
              for r in rows)
    if not finalists:
        if terminal:
            raise RailFateError("empty harmonic rail fabricated lane selection")
    elif not alt and (len(terminal) != 1 or
                      terminal[0]["candidate_id"] != winner):
        raise RailFateError("native rail winner/terminal source-ID mismatch")
    result["native_rail_complete_invocations"] = 1
    result["native_rail_compared_sources"] = len(finalists)
    return result
