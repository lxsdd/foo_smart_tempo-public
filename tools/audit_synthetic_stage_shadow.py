#!/usr/bin/env python3
"""Read-only cross-tab of final host paths versus a pre-frozen audio-only shadow.

Both host decisions and shadow proposals MUST already exist before BPM labels
are consulted here. This is observational attribution, not a selector policy.
"""
from __future__ import annotations

import math

RISK = frozenset(("halftrap", "doubletrap", "switch", "nonperiodic", "tone"))
TRANSITIONS = (
    "wrong_to_correct", "wrong_to_held", "wrong_to_wrong",
    "correct_to_correct", "correct_to_held", "correct_to_wrong",
    "held_to_correct", "held_to_held", "held_to_wrong",
    "risk_accept_to_hold", "risk_accept_to_accept",
    "risk_hold_to_hold", "risk_hold_to_accept",
)
METRICS = ("total", "clear", "risk", "prod_correct", "prod_wrong",
           "prod_held", "shadow_correct", "shadow_wrong", "shadow_held",
           "risk_prod_accepted", "risk_prod_held", "risk_shadow_accepted",
           "risk_shadow_held") + TRANSITIONS


def audit_stage_shadow(scored: dict, stages: dict, frozen: dict,
                       cases: tuple, signals: dict) -> dict:
    """Classify stage-by-stage *after* the existing strict host and policy join.

    Never treat a final-path label as a unique internal cause; never promote
    exported board membership to primary-selector eligibility.
    """
    names = [case[0] for case in cases]
    if (not names or len(names) != len(set(names)) or
            set(signals) != set(names) or
            [row["id"] for row in scored["results"]] != names or
            [row[0] for row in stages["details"]] != names or
            [row[0] for row in frozen["cases"]] != names):
        raise ValueError("stage-shadow identity/order mismatch")
    stage_names = set(stages["stages"])
    if not stage_names or any(row[1] not in stage_names for row in stages["details"]):
        raise ValueError("stage-shadow invalid final path")
    stats = {stage: dict.fromkeys(METRICS, 0) for stage in stages["stages"]}
    details = []
    for (name, ref, style), observed, stage_row, frozen_row in zip(
            cases, scored["results"], stages["details"], frozen["cases"]):
        _, stage, reported_outcome, _, _ = stage_row
        proposed = frozen_row[1]
        bpm = observed["reported_bpm"]
        if not isinstance(bpm, (int, float)) or not math.isfinite(bpm):
            raise ValueError("invalid host BPM in stage-shadow join")
        accepted = not observed["held"] and bpm > 0
        if bool(observed["held"]) == accepted:
            raise ValueError("contradictory host hold in stage-shadow join")
        if proposed is not None:
            if (not isinstance(proposed, (int, float)) or
                    not math.isfinite(proposed) or proposed <= 0):
                raise ValueError("unsafe shadow proposal")
            if not any(abs(float(c["bpm"]) - proposed) <= 1e-6
                       for c in signals[name]):
                raise ValueError("shadow proposed an unmeasured BPM")
        risk = style in RISK
        if risk:
            if reported_outcome != ("risk_accept" if accepted else "risk_hold"):
                raise ValueError("stage-shadow risk outcome mismatch")
            prod = "accept" if accepted else "hold"
            shadow = "accept" if proposed is not None else "hold"
        else:
            if ref is None or not math.isfinite(ref) or ref <= 0:
                raise ValueError("missing clear BPM oracle")
            prod = ("held" if not accepted else
                    "correct" if abs(bpm - ref) <= .20 + 1e-9 else "wrong")
            shadow = ("held" if proposed is None else
                      "correct" if abs(proposed - ref) <= .20 + 1e-9 else "wrong")
            if reported_outcome != prod:
                raise ValueError("stage-shadow clear outcome mismatch")
        row = stats[stage]
        row["total"] += 1
        row["risk" if risk else "clear"] += 1
        if risk:
            row["risk_prod_accepted" if accepted else "risk_prod_held"] += 1
            row["risk_shadow_accepted" if proposed is not None else "risk_shadow_held"] += 1
            row[f"risk_{prod}_to_{shadow}"] += 1
        else:
            row[f"prod_{prod}"] += 1
            row[f"shadow_{shadow}"] += 1
            row[f"{prod}_to_{shadow}"] += 1
        details.append((name, stage, prod, shadow))
    for stage, row in stats.items():
        original = stages["stages"][stage]
        if (row["total"] != original["total"] or
                row["clear"] != original["clear"] or
                row["risk"] != original["risk"] or
                row["prod_correct"] != original["clear_020"] or
                row["prod_wrong"] != original["clear_wrong"] or
                row["prod_held"] != original["clear_held"] or
                row["risk_prod_accepted"] != original["risk_accepted"] or
                row["risk_prod_held"] != original["risk_held"] or
                sum(row[k] for k in TRANSITIONS[:9]) != row["clear"] or
                sum(row[k] for k in TRANSITIONS[9:]) != row["risk"] or
                row["shadow_correct"] + row["shadow_wrong"] + row["shadow_held"] != row["clear"] or
                row["risk_shadow_accepted"] + row["risk_shadow_held"] != row["risk"]):
            raise ValueError("stage-shadow denominator mismatch")
    totals = {key: sum(s[key] for s in stats.values()) for key in METRICS}
    shadow = frozen["counts"]
    if (totals["clear"] != 11 or totals["risk"] != 5 or
            totals["shadow_correct"] != shadow["clear_correct"] or
            totals["shadow_wrong"] != shadow["clear_wrong"] or
            totals["shadow_held"] != shadow["clear_held"] or
            totals["risk_shadow_accepted"] != sum(shadow[k] for k in (
                "metrical_accepted", "nonstationary_accepted", "nonrhythmic_accepted"))):
        raise ValueError("stage-shadow frozen cohort denominator mismatch")
    return {"stages": stats, "totals": totals, "details": details}
