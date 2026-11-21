#!/usr/bin/env python3
"""Oracle-blind native low continuous rescue fate audit (branch-local only)."""


class LowContinuousFateError(ValueError):
    pass


GATE = {"low_continuous_allowed", "low_continuous_provisional_exclusion"}
CLOSURE = {"low_continuous_branch_veto",
           "low_continuous_branch_ranked_out",
           "low_continuous_branch_winner"}
RANK = "low_continuous_rank_scored"
COMPARE = {"low_continuous_compare_won", "low_continuous_compare_lost"}


def reconcile_native_low_continuous(rows: list[dict]) -> dict[str, int]:
    result = {"native_low_continuous_complete_invocations": 0,
              "native_low_continuous_branch_veto_sources": 0,
              "native_low_continuous_ranked_out_sources": 0,
              "native_low_continuous_rescue_winners": 0}
    sources = [r for r in rows if r["stage"] == "candidate_transform"]
    ids = [r["candidate_id"] for r in sources]
    bpms = {r["candidate_id"]: r["metrics"]["bpm"] for r in sources}
    interesting = [(i, r) for i, r in enumerate(rows)
                   if r["reason_code"] in GATE | CLOSURE | COMPARE | {RANK}]
    if not interesting:
        return result
    if not ids:
        raise LowContinuousFateError("continuous rescue without native source board")
    if not any(r["reason_code"] in GATE for _, r in interesting):
        raise LowContinuousFateError("continuous closure/rank without executed gate")
    cursor = 0
    admitted = []
    best = None
    best_tuple = None
    for cid in ids:
        if cursor >= len(interesting):
            raise LowContinuousFateError("truncated continuous source sweep")
        index, gate = interesting[cursor]
        if (gate["reason_code"] not in GATE or gate["candidate_id"] != cid or
                gate["stage"] != "candidate_filter" or
                gate["metrics"]["bpm"] != bpms[cid] or
                gate["outcome"] != ("included" if gate["reason_code"] ==
                                    "low_continuous_allowed" else "excluded")):
            raise LowContinuousFateError("missing/reordered/contradictory continuous gate")
        cursor += 1
        if gate["reason_code"] != "low_continuous_allowed":
            continue
        admitted.append(cid)
        if cursor + 1 >= len(interesting):
            raise LowContinuousFateError("continuous rank/comparison missing")
        rank_i, rank = interesting[cursor]
        compare_i, compare = interesting[cursor + 1]
        if (rank_i != index + 1 or compare_i != rank_i + 1 or
                rank["stage"] != "primary_rank" or rank["reason_code"] != RANK or
                rank["candidate_id"] != cid or rank["outcome"] != "included" or
                compare["stage"] != "primary_compare" or
                compare["reason_code"] not in COMPARE or
                compare["candidate_id"] != cid or
                rank["metrics"]["bpm"] != bpms[cid] or
                compare["metrics"]["bpm"] != bpms[cid] or
                rank["metrics"]["score"] != gate["metrics"]["score"] or
                compare["metrics"]["score"] != gate["metrics"]["score"]):
            raise LowContinuousFateError("rank/compare not adjacent to native gate")
        metrics = compare["metrics"]
        try:
            tie = tuple(metrics[f"tie_{n}"] for n in range(5))
        except KeyError as exc:
            raise LowContinuousFateError("missing five-field native comparator") from exc
        if tie[0] != gate["metrics"]["score"]:
            raise LowContinuousFateError("continuous comparator not tied to gate score")
        won = best_tuple is None or tie > best_tuple
        if ((compare["reason_code"] == "low_continuous_compare_won") != won or
                compare["outcome"] != ("included" if won else "excluded")):
            raise LowContinuousFateError("native five-field winner/loss contradiction")
        if won:
            best, best_tuple = cid, tie
        cursor += 2
    if cursor + len(ids) != len(interesting):
        raise LowContinuousFateError("missing/extra continuous branch closure")
    for cid in ids:
        index, row = interesting[cursor]
        if (row["candidate_id"] != cid or row["stage"] != "candidate_filter" or
                row["metrics"]["bpm"] != bpms[cid] or
                row["reason_code"] not in CLOSURE):
            raise LowContinuousFateError("source identity/ordering in branch closure")
        expected = ("low_continuous_branch_veto" if cid not in admitted else
                    "low_continuous_branch_winner" if cid == best else
                    "low_continuous_branch_ranked_out")
        if (row["reason_code"] != expected or
                row["outcome"] != ("included" if cid == best else "excluded")):
            raise LowContinuousFateError("branch closure disagrees with actual comparator")
        if expected == "low_continuous_branch_veto":
            result["native_low_continuous_branch_veto_sources"] += 1
        elif expected == "low_continuous_branch_ranked_out":
            result["native_low_continuous_ranked_out_sources"] += 1
        else:
            result["native_low_continuous_rescue_winners"] += 1
        cursor += 1
    terminal = [(i, r) for i, r in enumerate(rows)
                if r["reason_code"] == "terminal_lane_choice"]
    if (best is not None and
            (len(terminal) != 1 or terminal[0][0] <= interesting[-1][0] or
             terminal[0][1]["candidate_id"] != best or
             terminal[0][1]["metrics"]["bpm"] != bpms[best])):
        raise LowContinuousFateError("rescue winner not the executed lane terminal")
    result["native_low_continuous_complete_invocations"] = 1
    return result
