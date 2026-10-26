#!/usr/bin/env python3
"""Fixed, post-host, source-ID-preserving shadow rank comparison for Phase B.

This NEVER selects a BPM in foobar. The frozen reference is read ONLY after
the actual Win32/x64 host has written its independently validated trace.
These four simple unconstrained baselines are diagnostic hypotheses, NOT
production recommendations, genre-aware policies or a tuned quality claim.
"""
from __future__ import annotations

import argparse
import math
from pathlib import Path

from audit_smart_tempo_b_candidate_recall import (
    CASES, RecallError, analyze as recall_analyze, unique_object,
)
from audit_smart_tempo_ci_pipeline_trace import load, validate, TraceError
from score_synthetic_bpm_reference import load_observed

# Predeclared selection rules. No branch-specific tuning, manual reference
# injection, alias bonus, learned thresholds, or adaptive case parameters.
MODELS = ("SCORE_SUM", "SUPPORT", "COMBINED", "SCORE_TIMES_SUPPORT")
RESULT_KEYS = (
    "CANDIDATES", "REFERENCE_PRESENT", "NATIVE_PRIMARY_CORRECT",
    "NATIVE_FINAL_CORRECT", "NATIVE_PRIMARY_SOURCE_INDEXED",
    "REFERENCE_LOST_AT_PRIMARY",
) + tuple("MODEL_" + name + "_CORRECT" for name in MODELS) + tuple(
    "MODEL_" + name + "_MATCHES_PRIMARY" for name in MODELS
) + tuple("MODEL_" + name + "_REFERENCE_RANK" for name in MODELS) + tuple(
    "MODEL_" + name + "_TOP3_REFERENCE" for name in MODELS
)


def fixed_order(cluster_rows: list[dict], eligible_rows: list[dict]) -> dict:
    """Order all same-run cluster IDs using source metrics, WITHOUT truth BPM.

    Equal scores retain first native collapse vector order. FullBoard/Policy
    must not be substituted for the exact Primary source snapshot.
    """
    if not cluster_rows:
        if eligible_rows:
            raise RecallError("empty cluster with invented eligibility")
        return {name: [] for name in MODELS}
    if not 1 <= len(cluster_rows) <= 4096:
        raise RecallError("invalid native Primary candidate count")
    ids = [row["candidate_id"] for row in cluster_rows]
    if any(type(cid) is not str or not cid.startswith("c") for cid in ids):
        raise RecallError("bad native source ID")
    if len(set(ids)) != len(ids):
        raise RecallError("duplicate native cluster ID")
    lookup = {}
    for row in eligible_rows:
        cid = row["candidate_id"]
        if cid in lookup:
            raise RecallError("duplicate Primary eligibility source")
        lookup[cid] = row
    if set(lookup) != set(ids):
        raise RecallError("Primary eligibility diverges from source objects")

    scores = []
    for cluster in cluster_rows:
        cid = cluster["candidate_id"]
        source_bpm = cluster["metrics"]["bpm"]
        eligible = lookup[cid]
        if abs(eligible["metrics"]["bpm"] - source_bpm) >= .001:
            raise RecallError("Primary eligibility BPM mismatches cluster ID")
        s, u, b = (cluster["metrics"]["score"],
                   cluster["metrics"]["support"],
                   eligible["metrics"]["score"])
        if any(type(v) not in (float, int) or not math.isfinite(v) or
               abs(v) > 1e12 for v in (s, u, b)):
            raise RecallError("invalid source-only ranking metric")
        scores.append((s, u, b, s * u))
    # Stable sort preserves native collapse-vector order on exact ties.
    # Source ranking does not inspect oracle BPM or a ground-truth label.
    return {model: [ids[i] for i in sorted(
                range(len(ids)), key=lambda i: -scores[i][j])]
            for j, model in enumerate(MODELS)}


def fixed_rank(cluster_rows: list[dict], eligible_rows: list[dict]) -> dict:
    return {name: (ordered[0] if ordered else None)
            for name, ordered in fixed_order(
                cluster_rows, eligible_rows).items()}


def shadow_result(rows: list[dict], manifest: dict, observed: list[dict],
                  case_id: str) -> dict[str, int]:
    # This independent existing source-to-writer scorer validates the frozen
    # manifest, complete host BPM witness and native-versus-final decision.
    recall = recall_analyze(rows, manifest, observed, case_id)
    if case_id not in CASES:
        raise RecallError("unknown synthetic oracle case")
    expected = CASES[case_id][0]
    cluster = [row for row in rows if row["stage"] == "candidate_transform"]
    eligibility = [row for row in rows if row["stage"] == "primary_eligibility"]
    orders = fixed_order(cluster, eligibility)
    ranks = {name: (ids[0] if ids else None) for name, ids in orders.items()}
    by_id = {row["candidate_id"]: row["metrics"]["bpm"] for row in cluster}
    selected = [row for row in rows if row["stage"] == "primary_select"]
    if len(selected) != 1:
        raise RecallError("Primary selected source missing or duplicate")
    item = selected[0]
    parents = item["parent_candidate_ids"]
    if expected is None:
        if cluster or eligibility or parents or item["outcome"] != "no_candidate":
            raise RecallError("silence acquired a false Primary source")
        chosen = None
    else:
        # A real Primary can select an exact measured source AND put it on
        # Review Hold. The downstream Policy may still release a different
        # measured FullBoard object. Neither outcome invalidates the
        # original source-indexed Primary comparator provenance.
        if len(parents) == 1 and item["outcome"] in ("selected", "review_hold"):
            if parents[0] not in by_id:
                raise RecallError("rhythmic Primary source unindexed")
            chosen = parents[0]
            if abs(by_id[chosen] - item["metrics"]["bpm"]) >= .001:
                raise RecallError("Primary selected-object BPM mismatch")
        elif not parents and item["outcome"] == "no_candidate" and (
                item["metrics"]["bpm"] == 0):
            chosen = None
        else:
            raise RecallError("rhythmic Primary source/outcome inconsistent")
    correct = lambda cid: int(
        cid is not None and expected is not None and
        abs(by_id[cid] - expected) <= .20
    )
    if any(v is not None and v not in by_id for v in ranks.values()):
        raise RecallError("shadow rank invented a source ID")
    result = {
        "CANDIDATES": len(by_id),
        "REFERENCE_PRESENT": int(expected is not None and any(
            abs(bpm - expected) <= .20 for bpm in by_id.values())),
        "NATIVE_PRIMARY_CORRECT": correct(chosen),
        "NATIVE_FINAL_CORRECT": recall["FINAL_ACCEPTED_NEAR_020"],
        "NATIVE_PRIMARY_SOURCE_INDEXED": int(chosen is not None),
        "REFERENCE_LOST_AT_PRIMARY": int(
            recall["ROOT_CAUSE_STAGE"] == "primary_selected_other"
        ),
    }
    for name in MODELS:
        cid = ranks[name]
        result["MODEL_" + name + "_CORRECT"] = correct(cid)
        result["MODEL_" + name + "_MATCHES_PRIMARY"] = int(
            cid is not None and chosen is not None and cid == chosen
        )
        # Join frozen reference ONLY AFTER source-only order is finalized.
        # Report 0 if absent; 1-based position otherwise.
        near_rank = next((
            i for i, source_id in enumerate(orders[name], 1)
            if expected is not None and abs(by_id[source_id] - expected) <= .20
        ), 0)
        result["MODEL_" + name + "_REFERENCE_RANK"] = near_rank
        result["MODEL_" + name + "_TOP3_REFERENCE"] = int(
            1 <= near_rank <= 3)
    if set(result) != set(RESULT_KEYS):
        raise RecallError("shadow report field mismatch")
    return result


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--case", choices=sorted(CASES), required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--observed", type=Path, required=True)
    parser.add_argument("--trace", type=Path, required=True)
    args = parser.parse_args()
    try:
        if not args.manifest.is_file() or not 0 < args.manifest.stat().st_size < 32768:
            raise RecallError("missing or oversized independent oracle")
        import json
        manifest = json.loads(args.manifest.read_text(encoding="utf-8"),
                              object_pairs_hook=unique_object)
        rows = load(args.trace)
        validate(rows, require_windows=True)
        result = shadow_result(rows, manifest, load_observed(args.observed),
                               args.case)
    except (RecallError, TraceError, ValueError, UnicodeError,
            KeyError, TypeError) as exc:
        parser.error(str(exc))
    print("SMART_TEMPO_B_SHADOW_VERIFIED=1")
    # Only bounded numeric observations, never source ID, song metadata,
    # filename, oracle BPM, owner input, or generated audio.
    for key in RESULT_KEYS:
        print("SMART_TEMPO_B_SHADOW_" + key + "=" + str(result[key]))


if __name__ == "__main__":
    main()
