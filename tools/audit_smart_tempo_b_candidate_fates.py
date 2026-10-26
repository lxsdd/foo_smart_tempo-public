#!/usr/bin/env python3
"""Independent, label-blind executed-selector fate ledger for synthetic CI traces.

This is NOT a first-decisive-veto proof. A provisional exclusion can be
rescued, and an unvisited candidate has no observed rejecting predicate.
No audio, paths, tags, oracle BPM, or private identifiers are consumed.
"""
from __future__ import annotations

import argparse
from collections import Counter
from pathlib import Path

from audit_smart_tempo_ci_pipeline_trace import TraceError, load, validate

COMPARATOR_FAMILIES = frozenset((
    "raw", "harmonic", "low", "low_continuous", "general_baseline",
    "general_alt", "residual_high", "residual_harmonic", "harmonic_direct",
))
READMISSIONS = {
    "raw_alias_fallback_readmitted": "raw_alias_provisional_exclusion",
    "raw_support_fallback_readmitted": "raw_support_provisional_exclusion",
    "low_threshold_fallback_readmitted": "low_threshold_provisional_exclusion",
    "general_rail_fallback_readmitted": "general_out_of_rail_provisional",
}
FATES = (
    "selected", "unselected_pool_winner", "displaced_winner",
    "comparator_loser", "gate_only", "unvisited",
)


def reconcile(rows: list[dict]) -> dict[str, int]:
    """Reconcile original indexed source objects, never BPM-nearest joins.

    Call validate(...require_windows=True...) before this function on host
    input. This separate replay additionally checks its own rank adjacency,
    incumbent chronology, readmissions, and native disposition conservation.
    """
    clusters = [r for r in rows if r["stage"] == "candidate_transform"]
    sources = [r["candidate_id"] for r in clusters]
    if len(sources) != len(set(sources)):
        raise TraceError("duplicate collapsed source ID")
    source_set = set(sources)
    observations = {cid: set() for cid in sources}
    ranked = Counter()
    ever_won = set()
    displaced = set()
    best = {}
    first_excluded = {}
    readmitted = set()
    comparator_count = 0
    for index, row in enumerate(rows):
        stage = row["stage"]
        cid = row["candidate_id"]
        if stage == "primary_terminal" and cid in source_set:
            observations[cid].add("terminal")
        if stage == "candidate_filter" and cid in source_set:
            observations[cid].add("gate")
            reason = row["reason_code"]
            if row["outcome"] == "excluded":
                first_excluded.setdefault(cid, reason)
            if reason in READMISSIONS:
                if (cid not in first_excluded or
                        first_excluded[cid] != READMISSIONS[reason] and
                        not any(previous["stage"] == "candidate_filter" and
                                previous["candidate_id"] == cid and
                                previous["reason_code"] == READMISSIONS[reason] and
                                previous["outcome"] == "excluded"
                                for previous in rows[:index])):
                    raise TraceError("readmission has no earlier exact-source veto")
                readmitted.add(cid)
        if stage != "primary_rank":
            continue
        if index + 1 >= len(rows):
            raise TraceError("rank lacks comparator")
        compare = rows[index + 1]
        prefix = row["reason_code"].removesuffix("_rank_scored")
        if prefix not in COMPARATOR_FAMILIES or (
                compare["stage"] != "primary_compare" or
                compare["candidate_id"] != cid or
                compare["reason_code"] not in (
                    prefix + "_compare_won", prefix + "_compare_lost")):
            raise TraceError("rank/comparator source or family mismatch")
        if cid in source_set:
            ranked[cid] += 1
        elif prefix != "raw":
            raise TraceError("non-raw comparator points outside collapsed pool")
        if prefix == "raw" and cid in source_set:
            raise TraceError("raw comparator points into collapsed pool")
        epoch = row["metrics"].get("low_pool_epoch") if prefix == "low" else None
        if prefix == "low" and (
                not isinstance(epoch, int) or epoch < 1 or
                compare["metrics"].get("low_pool_epoch") != epoch):
            raise TraceError("invalid low comparator episode")
        key = (prefix, epoch)
        previous = best.get(key)
        score = row["metrics"]["score"]
        if score != compare["metrics"]["score"]:
            raise TraceError("rank/comparator score drift")
        ties = tuple(compare["metrics"].get("tie_" + str(i), 0.0)
                     for i in range(5 if prefix == "low_continuous" else 4))
        if previous is None:
            should_win = True
        else:
            old_score, old_ties, old_cid = previous
            if prefix in ("residual_harmonic", "harmonic_direct", "low_continuous"):
                should_win = ties > old_ties
            elif prefix in ("raw", "general_baseline"):
                should_win = (score > old_score + 1e-9 or
                              abs(score - old_score) <= 1e-9 and ties > old_ties)
            else:
                should_win = score > old_score
        won = compare["reason_code"].endswith("_won")
        if won != should_win or (
                compare["outcome"] != ("included" if won else "excluded")):
            raise TraceError("executed comparator verdict drift")
        if won:
            if previous is not None:
                displaced.add(previous[2])
            best[key] = (score, ties, cid)
            ever_won.add(cid)
        comparator_count += 1

    final_rows = [r for r in rows if r["stage"] == "primary_terminal" and
                  r["reason_code"] == "terminal_final_choice"]
    if len(final_rows) > 1:
        raise TraceError("multiple final source objects")
    selected = final_rows[0]["candidate_id"] if final_rows else None
    if selected is not None and selected not in source_set:
        raise TraceError("final source outside collapsed pool")
    live_winners = {value[2] for value in best.values()}
    dispositions = [r for r in rows if r["stage"] == "primary_disposition"]
    if len(dispositions) != len(sources):
        raise TraceError("native per-source disposition missing")
    counts = Counter()
    for cid, native in zip(sources, dispositions):
        if native["candidate_id"] != cid:
            raise TraceError("native disposition order/source mismatch")
        expected_native = (
            "selected_final_object" if cid == selected else
            "ranked_not_final" if ranked[cid] else
            "observed_not_ranked" if observations[cid] else
            "no_cluster_observation"
        )
        if native["reason_code"] != expected_native:
            raise TraceError("native disposition contradicts executed path")
        fate = (
            "selected" if cid == selected else
            "unselected_pool_winner" if cid in live_winners else
            "displaced_winner" if cid in ever_won else
            "comparator_loser" if ranked[cid] else
            "gate_only" if observations[cid] else "unvisited"
        )
        counts[fate] += 1
    if sum(counts[f] for f in FATES) != len(sources):
        raise TraceError("source fate partition incomplete")
    if selected is not None and counts["selected"] != 1:
        raise TraceError("selected source not conserved")
    return {
        "total": len(sources),
        **{f: counts[f] for f in FATES},
        "comparator_rows": comparator_count,
        "gate_excluded_sources": len(first_excluded),
        "readmitted_sources": len(readmitted),
        "first_decisive_veto_unproven": len(sources),
    }


def main() -> None:
    p = argparse.ArgumentParser()
    p.add_argument("--trace", required=True, type=Path)
    p.add_argument("--require-windows", action="store_true")
    args = p.parse_args()
    if not args.require_windows:
        raise TraceError("fate ledger requires strict original-host provenance")
    rows = load(args.trace)
    validate(rows, require_windows=True, require_segments=True,
             require_native_comparator=True)
    result = reconcile(rows)
    print("SMART_TEMPO_B_FATES_LEDGER=PASS")
    for key, value in result.items():
        print("SMART_TEMPO_B_FATES_" + key.upper() + "=" + str(value))
    print("SMART_TEMPO_B_FATES_CAUSAL_STATUS=EXECUTED_COMPARATOR_ONLY")


if __name__ == "__main__":
    main()
