#!/usr/bin/env python3
"""Read-only exported-board rank audit for the fixed 16-case public challenge.

An exported evidence rank is NOT a primary-selector path or provenance tag.
Neither rank 1 nor a high local-exact score proves selector reachability.
All BPM-oracle comparisons occur after host analysis and board export.
"""
from __future__ import annotations

import math

RISK = frozenset(("halftrap", "doubletrap", "switch", "nonperiodic", "tone"))
FIELDS = (
    "total", "clear", "risk", "correct", "wrong", "held", "risk_accept", "risk_hold",
    "wrong_precise_rank1", "wrong_precise_nonrank1_only", "wrong_no_precise",
    "wrong_output_rank1", "wrong_output_nonrank1_only", "wrong_output_missing",
    "wrong_precise_top_local", "wrong_precise_not_top_local",
)


def audit_export_rank(scored: dict, signals: dict, stages: dict, cases: tuple) -> dict:
    """Partition observed outcomes by exported rank, NOT hidden MIR origin."""
    names = [name for name, _, _ in cases]
    if (len(names) != 16 or len(set(names)) != 16 or
            set(signals) != set(names) or
            [row["id"] for row in scored["results"]] != names or
            [row[0] for row in stages["details"]] != names):
        raise ValueError("export-rank identity/order mismatch")
    result = {stage: dict.fromkeys(FIELDS, 0) for stage in stages["stages"]}
    details = []
    for (name, ref, style), row, stage_row in zip(
            cases, scored["results"], stages["details"]):
        _, stage, outcome, _, _ = stage_row
        if stage not in result:
            raise ValueError("unknown final-path stage")
        bpm, held = row["reported_bpm"], row["held"]
        if (not isinstance(bpm, (int, float)) or not math.isfinite(bpm) or
                not isinstance(held, bool) or (held and bpm > 0) or
                (not held and bpm <= 0)):
            raise ValueError("contradictory exported host outcome")
        board = []
        if len(signals[name]) > 32:
            raise ValueError("export board exceeds public cap")
        for candidate in signals[name]:
            try:
                rank = int(candidate["rank"])
                measured = float(candidate["bpm"])
                local = float(candidate["local_exact"])
            except (KeyError, TypeError, ValueError, OverflowError) as exc:
                raise ValueError("invalid exported candidate") from exc
            if (not 1 <= rank <= 256 or not math.isfinite(measured) or
                    measured <= 0 or not math.isfinite(local) or
                    not 0 <= local <= 1):
                raise ValueError("unbounded exported candidate")
            board.append((rank, measured, local))
        stat = result[stage]
        stat["total"] += 1
        risk = style in RISK
        stat["risk" if risk else "clear"] += 1
        if risk:
            if outcome != ("risk_hold" if held else "risk_accept"):
                raise ValueError("export-rank risk stage mismatch")
            stat["risk_hold" if held else "risk_accept"] += 1
            details.append((name, stage, "risk", "na", "na"))
            continue
        if not isinstance(ref, (int, float)) or not math.isfinite(ref) or ref <= 0:
            raise ValueError("invalid clear BPM oracle")
        label = ("held" if held else "correct"
                 if abs(bpm - ref) <= .20 + 1e-9 else "wrong")
        if outcome != label:
            raise ValueError("export-rank final stage outcome mismatch")
        stat[label] += 1
        if label != "wrong":
            details.append((name, stage, label, "na", "na"))
            continue
        precise = [c for c in board if abs(c[1] - ref) <= .20 + 1e-9]
        output = [c for c in board if abs(c[1] - bpm) <= .20 + 1e-9]
        precise_label = ("rank1" if any(c[0] == 1 for c in precise) else
                         "nonrank1_only" if precise else "missing")
        output_label = ("rank1" if any(c[0] == 1 for c in output) else
                        "nonrank1_only" if output else "missing")
        if precise_label == "missing":
            stat["wrong_no_precise"] += 1
        else:
            stat["wrong_precise_" + precise_label] += 1
        stat["wrong_output_" + output_label] += 1
        if precise:
            top_local = max(c[2] for c in board)
            key = ("wrong_precise_top_local" if any(
                c[2] >= top_local - 1e-12 for c in precise)
                else "wrong_precise_not_top_local")
            stat[key] += 1
        details.append((name, stage, "wrong", precise_label, output_label))
    for stage, stat in result.items():
        baseline = stages["stages"][stage]
        if (stat["total"] != baseline["total"] or
                stat["clear"] != baseline["clear"] or
                stat["risk"] != baseline["risk"] or
                stat["correct"] != baseline["clear_020"] or
                stat["wrong"] != baseline["clear_wrong"] or
                stat["held"] != baseline["clear_held"] or
                stat["risk_accept"] != baseline["risk_accepted"] or
                stat["risk_hold"] != baseline["risk_held"] or
                sum(stat[k] for k in ("wrong_precise_rank1",
                    "wrong_precise_nonrank1_only", "wrong_no_precise")) != stat["wrong"] or
                sum(stat[k] for k in ("wrong_output_rank1",
                    "wrong_output_nonrank1_only", "wrong_output_missing")) != stat["wrong"] or
                stat["wrong_precise_top_local"] +
                stat["wrong_precise_not_top_local"] !=
                stat["wrong_precise_rank1"] + stat["wrong_precise_nonrank1_only"]):
            raise ValueError("export-rank denominator mismatch")
    totals = {key: sum(stat[key] for stat in result.values()) for key in FIELDS}
    if totals["total"] != 16 or totals["clear"] != 11 or totals["risk"] != 5:
        raise ValueError("export-rank cohort mismatch")
    return {"stages": result, "totals": totals, "details": details,
            "interpretation": "exported_rank_only_not_primary_selector_provenance"}
