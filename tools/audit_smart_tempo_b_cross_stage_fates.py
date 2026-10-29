#!/usr/bin/env python3
"""Independent source-ID replay of native cross-stage candidate fates.

Bits are witnessed facts, never global selector eligibility. Synthetic CI
numeric IDs only; no audio, private labels, tags, or filenames.
"""
from __future__ import annotations
from collections import Counter

class CrossStageFateError(ValueError):
    pass

def reconcile(rows: list[dict], *, required: bool = False) -> dict[str, int]:
    by_stage = lambda stage: [r for r in rows if r["stage"] == stage]
    clusters = by_stage("candidate_transform")
    fates = by_stage("candidate_fate")
    empty = {"cross_stage_fate_rows": 0, "cross_stage_fate_veto_sources": 0,
             "cross_stage_fate_writer_sources": 0}
    if not fates and not required:
        return empty
    if len(fates) != len(clusters):
        raise CrossStageFateError("missing/duplicate native cross-stage source fates")
    if not clusters:
        return empty
    rail = by_stage("policy_filter")
    board = by_stage("policy_eligibility")
    pointer = by_stage("policy_pointer")
    writer = by_stage("writer_resolution")
    terminals = [r for r in by_stage("primary_terminal")
                 if r["reason_code"] == "terminal_final_choice"]
    if len(pointer) != 1 or len(writer) != 1 or len(terminals) > 1:
        raise CrossStageFateError("terminal native object references incomplete")
    if len(rail) != len(clusters):
        raise CrossStageFateError("native Policy rail cardinality incomplete")
    rail_by_id = {r["candidate_id"]: r for r in rail}
    if len(rail_by_id) != len(rail):
        raise CrossStageFateError("duplicate native Policy rail source")
    board_parents = Counter(p for r in board for p in r["parent_candidate_ids"])
    board_parent_by_id = {r["candidate_id"]: r["parent_candidate_ids"]
                          for r in board}
    primary = terminals[0]["candidate_id"] if terminals else None
    pointer_parent = pointer[0]["parent_candidate_ids"]
    writer_parent = writer[0]["parent_candidate_ids"]
    vetoes = by_stage("primary_veto_ledger")
    selectors = [r for r in rows if r["stage"] in
                 ("candidate_filter", "primary_rank", "primary_compare",
                  "primary_terminal")]
    ranks = {r["candidate_id"] for r in by_stage("primary_rank")}
    observed = {r["candidate_id"] for r in selectors
                if r["reason_code"] != "terminal_final_choice"}
    counted = Counter()
    for cluster, native in zip(clusters, fates):
        cid = cluster["candidate_id"]
        if (native["candidate_id"] != cid or native["parent_candidate_ids"] or
                native["reason_code"] != "cross_stage_observation_not_global_eligibility"
                or native["outcome"] != "observed"):
            raise CrossStageFateError("source fate order, ID or reason altered")
        if cid not in rail_by_id:
            raise CrossStageFateError("source fate missing Policy rail")
        raw_parents = set(cluster["parent_candidate_ids"])
        raw_vetos = [r for r in vetoes if r["candidate_id"] in raw_parents]
        cluster_vetos = [r for r in vetoes if r["candidate_id"] == cid]
        all_vetos = raw_vetos + cluster_vetos
        first = min((int(r["metrics"]["support"]) for r in all_vetos), default=0)
        last = max((int(r["metrics"]["support"]) for r in all_vetos), default=0)
        raw_positions = [j for j, r in enumerate(selectors, 1)
                         if r["candidate_id"] in raw_parents]
        cluster_positions = [j for j, r in enumerate(selectors, 1)
                             if r["candidate_id"] == cid]
        positions = raw_positions + cluster_positions
        first_selector = min(positions, default=0)
        last_selector = max(positions, default=0)
        source_board_ids = [bid for bid, parents in board_parent_by_id.items()
                            if parents == [cid]]
        if len(source_board_ids) > 1 or board_parents[cid] > 1:
            raise CrossStageFateError("source has duplicate Policy board membership")
        pointer_selected = bool(pointer_parent and
                                pointer_parent[0] in source_board_ids)
        writer_primary = (writer[0]["reason_code"] == "mir_primary" and
                          writer_parent == [cid])
        writer_policy = (writer[0]["reason_code"] in
                         ("mir_writer_override", "mir_review_hold_release",
                          "mir_local_exact_upgrade") and
                         len(writer_parent) == 1 and
                         writer_parent[0] in source_board_ids)
        flags = (
            (1 if cid == primary else 0) |
            (2 if rail_by_id[cid]["outcome"] == "included" else 0) |
            (4 if source_board_ids else 0) |
            (8 if pointer_selected else 0) |
            (16 if writer_primary else 0) |
            (32 if writer_policy else 0) |
            (64 if cid in ranks else 0) |
            (128 if cid in observed else 0) |
            (256 if cluster_vetos else 0) |
            (512 if raw_vetos else 0)
        )
        m = native["metrics"]
        if (abs(m["bpm"] - cluster["metrics"]["bpm"]) >= .001 or
                m["score"] != flags or
                m["support"] != len(all_vetos) or
                m["raw_veto_count"] != len(raw_vetos) or
                m["cluster_veto_count"] != len(cluster_vetos) or
                m["first_veto_event_index"] != first or
                m["last_veto_event_index"] != last or
                m["raw_selector_event_count"] != len(raw_positions) or
                m["cluster_selector_event_count"] != len(cluster_positions) or
                m["first_selector_event_index"] != first_selector or
                m["last_selector_event_index"] != last_selector):
            raise CrossStageFateError("native cross-stage bits/counts/chronology contradict source")
        counted["cross_stage_fate_rows"] += 1
        counted["cross_stage_fate_veto_sources"] += int(bool(all_vetos))
        counted["cross_stage_fate_writer_sources"] += int(writer_primary or writer_policy)
    if counted["cross_stage_fate_writer_sources"] > 1:
        raise CrossStageFateError("multiple applied native writer source identities")
    return {key: counted[key] for key in (
        "cross_stage_fate_rows", "cross_stage_fate_veto_sources",
        "cross_stage_fate_writer_sources")}
