#!/usr/bin/env python3
"""Oracle-blind native low-pulse UPPER-branch fate replay.

Only the actually executed branch-local veto is proved. A later independent
continuous-center rescue can still select a previously upper-vetoed source.
No final-primary first-veto or musical accuracy is inferred from this gate.
"""


class LowUpperFateError(ValueError):
    pass


def reconcile_native_low_upper(rows: list[dict]) -> dict[str, int]:
    sources = [r for r in rows if r["stage"] == "candidate_transform"]
    ids = [r["candidate_id"] for r in sources]
    bpms = {r["candidate_id"]: r["metrics"]["bpm"] for r in sources}
    reasons = {"low_upper_gate_allowed",
               "low_upper_gate_provisional_exclusion",
               "low_upper_branch_veto"}
    observed = [(i, r) for i, r in enumerate(rows)
                if r["stage"] == "candidate_filter" and
                r["reason_code"] in reasons]
    result = {
        "native_low_upper_complete_invocations": 0,
        "native_low_upper_branch_veto_sources": 0,
        "native_low_upper_continued_to_threshold": 0,
        "native_low_upper_ranked_sources": 0,
    }
    if not observed:
        if any(r["reason_code"].startswith(("low_threshold_", "low_rank_",
                                           "low_compare_", "low_continuous_"))
               for r in rows):
            raise LowUpperFateError("low selector without initial upper sweep")
        return result
    if not ids or len(ids) != len(set(ids)):
        raise LowUpperFateError("invalid low selector cluster inventory")
    cursor = 0

    def take(cid: str, reason: str, included: bool):
        nonlocal cursor
        if cursor >= len(observed):
            raise LowUpperFateError("truncated native low upper sequence")
        index, row = observed[cursor]
        cursor += 1
        if (row["candidate_id"] != cid or row["reason_code"] != reason or
                row["outcome"] != ("included" if included else "excluded") or
                row["metrics"]["bpm"] != bpms[cid]):
            raise LowUpperFateError("native low upper source/order/polarity drift")
        return index

    accepted, excluded = [], []
    for cid in ids:
        if cursor >= len(observed):
            raise LowUpperFateError("low upper initial sweep incomplete")
        reason = observed[cursor][1]["reason_code"]
        if reason == "low_upper_gate_allowed":
            take(cid, reason, True)
            accepted.append(cid)
        elif reason == "low_upper_gate_provisional_exclusion":
            take(cid, reason, False)
            excluded.append(cid)
        else:
            raise LowUpperFateError("low upper gate missing or reordered")
    initial_end = observed[cursor-1][0]
    if accepted:
        for cid in excluded:
            take(cid, "low_upper_branch_veto", False)
        if cursor != len(observed):
            raise LowUpperFateError("extra/duplicate low upper closure")
        last_veto = observed[-1][0] if excluded else initial_end
        if any(r["stage"] == "candidate_filter" and
               r["reason_code"].startswith("low_threshold_") for r in rows):
            raise LowUpperFateError("upper branch returned yet threshold executed")
        ranks = [(i, r) for i, r in enumerate(rows) if
                 r["stage"] == "primary_rank" and
                 r["reason_code"] == "low_rank_scored"]
        comps = [(i, r) for i, r in enumerate(rows) if
                 r["stage"] == "primary_compare" and
                 r["reason_code"] in ("low_compare_won", "low_compare_lost")]
        if ([r["candidate_id"] for _, r in ranks] != accepted or
                [r["candidate_id"] for _, r in comps] != accepted):
            raise LowUpperFateError("upper branch survivor rank/comparator drift")
        winner = None
        for (rank_i, rank), (comp_i, comp) in zip(ranks, comps):
            if (rank_i <= last_veto or comp_i != rank_i+1 or
                    comp["metrics"]["low_pool_epoch"] !=
                    rank["metrics"]["low_pool_epoch"]):
                raise LowUpperFateError("upper branch comparator chronology drift")
            if comp["outcome"] == "included":
                winner = comp["candidate_id"]
            elif comp["outcome"] != "excluded" or winner is None:
                raise LowUpperFateError("upper comparator lost without incumbent")
        if winner is None:
            raise LowUpperFateError("upper branch has no winner")
        terminal = [r for r in rows if r["stage"] == "primary_terminal"
                    and r["reason_code"] == "terminal_lane_choice"]
        if len(terminal) != 1:
            raise LowUpperFateError("upper branch lane terminal missing")
        # A subsequent native continuous-center rescue may replace this
        # branch's winner; the main parser independently verifies its tuple.
        continuous = any(r["stage"] == "primary_compare" and
                         r["reason_code"].startswith("low_continuous_compare_")
                         for r in rows)
        if not continuous and terminal[0]["candidate_id"] != winner:
            raise LowUpperFateError("upper winner not at lane terminal")
        result["native_low_upper_branch_veto_sources"] = len(excluded)
        result["native_low_upper_ranked_sources"] = len(accepted)
    else:
        if cursor != len(observed):
            raise LowUpperFateError("empty upper pool fabricated branch veto")
        threshold = [(i, r) for i, r in enumerate(rows) if
                     r["stage"] == "candidate_filter" and
                     r["reason_code"] in ("low_threshold_allowed",
                                         "low_threshold_provisional_exclusion")]
        if (len(threshold) != len(ids) or
                any(i <= initial_end or r["candidate_id"] != cid or
                    r["metrics"]["bpm"] != bpms[cid] or
                    r["outcome"] != ("included" if r["reason_code"] ==
                                      "low_threshold_allowed" else "excluded")
                    for (i, r), cid in zip(threshold, ids))):
            raise LowUpperFateError("empty upper pool did not continue to threshold")
        result["native_low_upper_continued_to_threshold"] = 1
    result["native_low_upper_complete_invocations"] = 1
    return result
