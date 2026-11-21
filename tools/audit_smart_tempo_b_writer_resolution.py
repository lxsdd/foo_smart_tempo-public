#!/usr/bin/env python3
"""Independent CI-only replay of the *applied* MIR writer resolution.

Policy proposals and equal-BPM candidates are not proof of a selected writer
source. The native analyzer supplies its final bounded reason after the
measured-witness invariant; this checker conserves that reason against actual
Primary/Policy/final events and never uses oracle BPM or media metadata.
"""
from __future__ import annotations

import math

ALLOWED = frozenset((
    "mir_primary", "mir_review_hold", "mir_no_candidate",
    "mir_writer_override", "mir_review_hold_release",
    "mir_local_exact_upgrade", "mir_writer_invariant_hold",
))
POLICY_APPLIED = frozenset((
    "mir_writer_override", "mir_review_hold_release",
    "mir_local_exact_upgrade",
))
HELD = frozenset((
    "mir_review_hold", "mir_no_candidate", "mir_writer_invariant_hold",
))
NATIVE_POINTER = frozenset((
    "pointer_native_fullboard_consistent",
    "pointer_native_supplemental_consistent",
))


class WriterResolutionError(ValueError):
    pass


def reconcile(rows: list[dict], *, required: bool = False) -> dict:
    def single(stage: str):
        matches = [r for r in rows if r["stage"] == stage]
        if len(matches) != 1:
            raise WriterResolutionError("missing/duplicate writer dependency: " + stage)
        return matches[0]

    observations = [r for r in rows if r["stage"] == "writer_resolution"]
    if not observations and not required:
        return dict(writer_resolution_native=0, writer_source_indexed=0,
                    writer_source_unindexed=0, writer_policy_applied=0,
                    writer_reason="not_observed")
    if len(observations) != 1:
        raise WriterResolutionError("native applied writer reason missing or duplicated")
    writer = observations[0]
    primary = single("primary_select")
    pointer = single("policy_pointer")
    policy = single("policy_change")
    final = single("final_output")
    witness = single("measured_witness")
    reason = writer["reason_code"]
    m = writer["metrics"]
    parents = writer["parent_candidate_ids"]
    if (reason not in ALLOWED or writer["candidate_id"] is not None or
            writer["outcome"] != final["outcome"] or
            abs(m["bpm"] - final["metrics"]["bpm"]) > .001 or
            m["score"] not in (0, 1) or m["support"] not in (0, 1) or
            m["score"] != int(m["score"]) or
            m["support"] != int(m["support"]) or
            m["support"] != int(policy["outcome"] == "selected")):
        raise WriterResolutionError("applied writer/Policy flags or final BPM contradicted")
    accepted = final["outcome"] == "selected"
    indexed = False
    if reason == "mir_primary":
        if (not accepted or primary["outcome"] != "selected" or
                len(primary["parent_candidate_ids"]) != 1 or
                parents != primary["parent_candidate_ids"] or
                abs(primary["metrics"]["bpm"] - m["bpm"]) > .001):
            raise WriterResolutionError("primary writer did not select actual terminal object")
        indexed = True
    elif reason in POLICY_APPLIED:
        if not accepted or m["score"] != 1 or m["support"] != 1:
            raise WriterResolutionError("unapplied Policy proposal impersonates writer")
        if reason == "mir_writer_override" and policy["reason_code"] != "writer_override":
            raise WriterResolutionError("writer override was not proposed")
        if reason == "mir_review_hold_release" and policy["reason_code"] not in (
                "writer_override", "hold_release"):
            raise WriterResolutionError("review-hold release was not proposed")
        if reason == "mir_local_exact_upgrade" and policy["reason_code"] != "keep_primary":
            raise WriterResolutionError("micro-upgrade was not proposed")
        # A changed effective Policy BPM can legitimately lack the observed
        # chosen pointer. Do not invent a source by nearest-BPM matching.
        expected = (pointer["parent_candidate_ids"]
                    if pointer["reason_code"] in NATIVE_POINTER and
                    abs(pointer["metrics"]["bpm"] - m["bpm"]) <= .001 else [])
        if parents != expected or len(parents) > 1:
            raise WriterResolutionError("writer source is not the native Policy pointer")
        indexed = len(parents) == 1
    elif reason in HELD:
        if accepted or parents or m["bpm"] != 0:
            raise WriterResolutionError("held writer fabricated accepted candidate")
        if reason == "mir_no_candidate" and primary["outcome"] != "no_candidate":
            raise WriterResolutionError("no-candidate reason contradicts Primary")
        if reason == "mir_review_hold" and not (
                primary["outcome"] in ("review_hold", "no_candidate") or
                policy["outcome"] == "review_hold"):
            raise WriterResolutionError("review-hold reason has no native hold")
        if reason == "mir_writer_invariant_hold" and witness["outcome"] != "rejected":
            raise WriterResolutionError("invariant hold falsely claims measured acceptance")
    else:
        raise WriterResolutionError("unhandled applied writer branch")
    if accepted and witness["outcome"] != "included":
        raise WriterResolutionError("applied writer lacks accepted measured witness")
    # Bind the measured source BPM to its exact original native creator ID.
    # Local-exact refinements are recorded as deltas, not re-parented by BPM.
    source_kind = m.get("source_kind")
    source_bpm = m.get("source_bpm")
    source_delta = m.get("source_delta")
    if (type(source_kind) not in (int, float) or
            source_kind not in (0, 1, 2, 3) or
            type(source_bpm) not in (int, float) or
            type(source_delta) not in (int, float) or
            not math.isfinite(source_bpm) or not math.isfinite(source_delta)):
        raise WriterResolutionError("native writer source evidence malformed")
    expected_kind = (1 if reason == "mir_primary" else
                     2 if indexed and pointer["reason_code"] ==
                          "pointer_native_fullboard_consistent" else
                     3 if indexed and pointer["reason_code"] ==
                          "pointer_native_supplemental_consistent" else 0)
    if source_kind != expected_kind or indexed != bool(expected_kind):
        raise WriterResolutionError("native writer source kind/pointer mismatch")
    if indexed:
        source_id = parents[0]
        creators = [r for r in rows
                    if r["stage"] in ("candidate_transform",
                                    "policy_eligibility", "supplemental_candidate")
                    and r.get("candidate_id") == source_id]
        expected_stage = {1: "candidate_transform", 2: "policy_eligibility",
                          3: "supplemental_candidate"}[expected_kind]
        if (len(creators) != 1 or creators[0]["stage"] != expected_stage or
                type(creators[0].get("metrics", {}).get("bpm")) not in (int, float) or
                not math.isfinite(creators[0]["metrics"]["bpm"]) or
                abs(source_bpm - creators[0]["metrics"]["bpm"]) > 0.0000011 or
                source_bpm <= 0 or
                abs(source_delta - (m["bpm"] - source_bpm)) > 0.0000021):
            raise WriterResolutionError("native writer exact indexed source BPM drift")
    elif source_bpm != 0 or source_delta != 0:
        raise WriterResolutionError("unindexed writer invented source measurement")
    refined_primary = indexed and expected_kind == 1 and abs(source_delta) > 0.001
    if refined_primary:
        if (abs(source_delta) > 0.800002 or
                witness["reason_code"] != "indexed_native_refined_witness" or
                source_id not in witness["parent_candidate_ids"]):
            raise WriterResolutionError("refined Primary writer lacks exact selected witness")
    elif witness["reason_code"] == "indexed_native_refined_witness":
        raise WriterResolutionError("non-refined writer forged a native refined witness")
    if indexed and not refined_primary and (
            witness["reason_code"] != "indexed_native_witness" or
            source_id not in witness["parent_candidate_ids"]):
        raise WriterResolutionError("indexed writer source missing from measured witness")
    return dict(writer_resolution_native=1,
                writer_source_indexed=int(accepted and indexed),
                writer_source_unindexed=int(accepted and not indexed),
                writer_policy_applied=int(reason in POLICY_APPLIED),
                writer_reason=reason,
                writer_source_kind=int(source_kind),
                writer_source_delta=source_delta)
