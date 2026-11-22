#!/usr/bin/env python3
"""Independent CI-only replay of the actual manual-review merge, never Primary eligibility.

Only bounded synthetic numeric evidence and native source IDs. A manual source
may be supplemental-only, so its presence is NOT a selector veto override.
"""
from __future__ import annotations
from collections import Counter

KINDS = {
    "manual_full_board": 1,
    "manual_primary_selector": 2,
    "manual_material_risk": 4,
    "manual_partial_aggregate": 8,
    "manual_partial_topk": 16,
}
ORDER = tuple(KINDS)
FIELDS = ("bpm", "score", "support", "winner_support",
          "local_exact_score", "evidence_rank", "origin_mask")


class ManualTraceError(ValueError):
    pass


def _close(a, b):
    return abs(a - b) <= 0.00002


def reconcile(rows: list[dict], *, required: bool = False) -> dict:
    states = [r for r in rows if r["stage"] == "manual_state"]
    sources = [r for r in rows if r["stage"] == "manual_source"]
    outputs = [r for r in rows if r["stage"] == "manual_export"]
    if not states:
        if required:
            raise ManualTraceError("native manual export state absent")
        if sources or outputs:
            raise ManualTraceError("orphan manual source/export")
        return {"manual_native_state": "LEGACY_NOT_CAPTURED",
                "manual_source_count": 0, "manual_export_count": 0,
                "manual_merged_sources": 0, "manual_supplemental_only": 0}
    if len(states) != 1:
        raise ManualTraceError("duplicate native manual state")
    state = states[0]
    if (state["candidate_id"] is not None or state["parent_candidate_ids"] or
            state["outcome"] != "observed" or
            state["reason_code"] not in ("manual_requested", "manual_not_requested") or
            state["metrics"]["bpm"] != 0):
        raise ManualTraceError("malformed native manual state")
    if (state["metrics"]["score"] != len(sources) or
            state["metrics"]["support"] != len(outputs) or
            len(sources) > 8192 or len(outputs) > 8192):
        raise ManualTraceError("native manual source/output count mismatch")
    if state["reason_code"] == "manual_not_requested":
        if sources or outputs:
            raise ManualTraceError("manual export without native capture request")
        return {"manual_native_state": "NOT_REQUESTED",
                "manual_source_count": 0, "manual_export_count": 0,
                "manual_merged_sources": 0, "manual_supplemental_only": 0}

    # Native source ordering is fixed: FullBoard, Primary, material risk,
    # partial aggregate, TopK. The exact numeric first-match grouping is
    # replayed independently of the C++ recorder and of reference BPM.
    seen_ids = set()
    kind_order = -1
    groups = []
    fullboard = [r for r in rows if r["stage"] == "policy_eligibility"]
    primary = [r for r in rows if r["stage"] == "primary_select"]
    expected_fullboard = [
        r for r in fullboard if 40 <= r["metrics"]["bpm"] <= 220
    ]
    native_fullboard = 0
    native_primary = 0
    for src in sources:
        kind = src["reason_code"]
        if kind not in KINDS or src["outcome"] != "observed":
            raise ManualTraceError("unknown native manual input origin")
        order = ORDER.index(kind)
        if order < kind_order or (order in (1, 2, 3) and order == kind_order):
            raise ManualTraceError("manual input source ordering drift")
        kind_order = order
        cid = src["candidate_id"]
        m = src["metrics"]
        if cid in seen_ids or not 40 <= m["bpm"] <= 220 or (
                m["origin_mask"] != KINDS[kind] or
                m["evidence_rank"] != int(m["evidence_rank"]) or
                m["evidence_rank"] < 0):
            raise ManualTraceError("malformed native manual input identity")
        seen_ids.add(cid)
        parents = src["parent_candidate_ids"]
        if kind == "manual_full_board":
            if native_fullboard >= len(expected_fullboard):
                raise ManualTraceError("extra FullBoard manual source")
            original = expected_fullboard[native_fullboard]
            if (parents != [original["candidate_id"]] or
                    not _close(m["bpm"], original["metrics"]["bpm"]) or
                    m["evidence_rank"] != native_fullboard + 1):
                raise ManualTraceError("manual FullBoard index/parent drift")
            native_fullboard += 1
        elif kind == "manual_primary_selector":
            if (native_primary or len(primary) != 1 or
                    parents != primary[0]["parent_candidate_ids"] or
                    len(parents) != 1 or m["evidence_rank"] != 0):
                raise ManualTraceError("manual Primary pointer not indexed")
            native_primary += 1
        elif parents or m["evidence_rank"] != 0:
            raise ManualTraceError("supplemental source falsely indexed as Primary")
        group = next((g for g in groups if abs(g["bpm"] - m["bpm"]) <= .001), None)
        if group is None:
            group = {"bpm": m["bpm"], "ids": [], "score": m["score"],
                     "support": m["support"], "winner": m["winner_support"],
                     "exact": m["local_exact_score"], "rank": 0, "mask": 0}
            groups.append(group)
        group["ids"].append(cid)
        group["score"] = max(group["score"], m["score"])
        group["support"] = max(group["support"], m["support"])
        group["winner"] = max(group["winner"], m["winner_support"])
        group["exact"] = max(group["exact"], m["local_exact_score"])
        group["mask"] |= KINDS[kind]
        if m["evidence_rank"] > 0:
            if group["rank"] == 0 or m["evidence_rank"] < group["rank"]:
                group["rank"] = int(m["evidence_rank"])
    if native_fullboard != len(expected_fullboard):
        raise ManualTraceError("missing FullBoard manual source")
    groups.sort(key=lambda g: (g["rank"] == 0, -g["score"],
                               -g["support"], -g["exact"], g["bpm"]))
    for i, group in enumerate(groups):
        if not group["rank"]:
            group["rank"] = i + 1
    if len(groups) != len(outputs):
        raise ManualTraceError("missing/extra merged manual export")
    for group, out in zip(groups, outputs):
        m = out["metrics"]
        if (out["outcome"] != "observed" or
                out["reason_code"] != "manual_merged_export" or
                out["candidate_id"] in seen_ids or
                out["parent_candidate_ids"] != group["ids"]):
            raise ManualTraceError("merged manual source ID partition drift")
        seen_ids.add(out["candidate_id"])
        expected = {"bpm": group["bpm"], "score": group["score"],
                    "support": group["support"], "winner_support": group["winner"],
                    "local_exact_score": group["exact"],
                    "evidence_rank": group["rank"], "origin_mask": group["mask"]}
        if any(not _close(m[key], expected[key]) for key in FIELDS):
            raise ManualTraceError("manual merge value/rank/origin drift")
    counts = Counter(kind for src in sources for kind, bit in KINDS.items()
                     if src["metrics"]["origin_mask"] == bit)
    return {"manual_native_state": "REQUESTED_VERIFIED",
            "manual_source_count": len(sources),
            "manual_export_count": len(outputs),
            "manual_merged_sources": sum(len(g["ids"]) > 1 for g in groups),
            "manual_supplemental_only": sum(g["mask"] & 3 == 0 for g in groups),
            "manual_primary_sources": counts["manual_primary_selector"],
            "manual_fullboard_sources": counts["manual_full_board"]}
