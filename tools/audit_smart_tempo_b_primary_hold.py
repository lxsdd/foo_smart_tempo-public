#!/usr/bin/env python3
"""Independent native Primary hold predicate and applied-writer causal oracle.

CI-only numeric source traces. No audio, metadata or private references.
"""
from __future__ import annotations

FLAGS = ("enabled", "candidate", "auto_candidate", "review_hold",
         "route_valid", "sparse_hold", "material_hold",
         "high_family_hold", "soft_center_hold")
HOLD_FLAGS = ("material_hold", "high_family_hold",
              "soft_center_hold", "sparse_hold")
ALLOWED_EMPTY = frozenset(("empty_mir_primary_candidate_board",
                           "selector_returned_no_candidate", "invalid_primary_candidate"))


class PrimaryHoldError(ValueError):
    pass


def reconcile(rows: list[dict], *, required: bool = False) -> dict[str, int]:
    primary = [r for r in rows if r["stage"] == "primary_select"]
    holds = [r for r in rows if r["stage"] == "primary_hold"]
    if not holds and not required:
        return {"primary_hold_native_verified": 0}
    if len(primary) != 1 or len(holds) != 1:
        raise PrimaryHoldError("missing/duplicated native Primary hold decision")
    p, h = primary[0], holds[0]
    m = h["metrics"]
    if (set(m) != set(FLAGS) | {"bpm", "score", "support"} or
            any(type(m[k]) not in (int, float) or m[k] not in (0, 1)
                for k in FLAGS) or
            h["candidate_id"] is not None or
            h["parent_candidate_ids"] != p["parent_candidate_ids"] or
            h["outcome"] != p["outcome"] or
            abs(m["bpm"] - p["metrics"]["bpm"]) > 0.001 or
            abs(m["score"] - p["metrics"]["support"]) > 1e-8 or
            m["support"] < 0 or
            h["sequence"] != p["sequence"] + 1):
        raise PrimaryHoldError("Primary flags, source, or chronology drift")
    candidate = bool(m["candidate"])
    auto = bool(m["auto_candidate"])
    held = bool(m["review_hold"])
    enabled = bool(m["enabled"])
    flags = {k: bool(m[k]) for k in HOLD_FLAGS}
    if (candidate != (m["bpm"] > 0) or
            m["route_valid"] != int(40 <= m["bpm"] <= 220) or
            auto != (candidate and not held) or
            held != any(flags.values()) or
            (not enabled and (candidate or held or auto))):
        raise PrimaryHoldError("Primary native hold OR/auto/route invariant")
    expected_outcome = ("review_hold" if held else "selected" if auto
                        else "no_candidate")
    if p["outcome"] != expected_outcome:
        raise PrimaryHoldError("Primary outcome contradicts native hold flags")
    reason = h["reason_code"]
    if not enabled:
        expected = "disabled"
    elif not candidate:
        if reason not in ALLOWED_EMPTY:
            raise PrimaryHoldError("missing candidate has invented reason")
        expected = reason
    elif flags["material_hold"]:
        expected = "material_risk_review_hold"
    elif flags["high_family_hold"]:
        expected = "high_family_conflict_review_hold"
    elif flags["soft_center_hold"]:
        expected = "soft_center_conflict_review_hold"
    elif flags["sparse_hold"]:
        expected = "sparse_or_acapella_review_hold"
    else:
        expected = "measured_candidate_primary"
    if reason != expected:
        raise PrimaryHoldError("Primary priority reason disagrees with flags")
    writers = [r for r in rows if r["stage"] == "writer_resolution"]
    if len(writers) > 1:
        raise PrimaryHoldError("multiple applied writer decisions")
    if writers and (held or not auto) and writers[0]["reason_code"] == "mir_primary":
        raise PrimaryHoldError("Primary hold was bypassed by native writer")
    return {"primary_hold_native_verified": 1,
            "primary_hold_active": int(held),
            "primary_hold_cause_count": sum(flags.values()),
            "primary_hold_multi_cause": int(sum(flags.values()) > 1),
            "primary_auto_eligible": int(auto)}
