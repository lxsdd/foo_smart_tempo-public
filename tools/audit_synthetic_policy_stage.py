#!/usr/bin/env python3
"""Read-only final-path attribution for the fixed public complex WAV cohort.

Final diagnostic labels are NOT disjoint internal root causes: in particular,
review-hold-release can also have a writer-override candidate underneath.
Exported boards do NOT establish primary-selector reachability.
"""
from __future__ import annotations

import csv
import math
import re
from pathlib import Path

POLICY_FIELDS = ("id", "policy_reason", "decision_class", "measured_policy_reason")
# Source: modern_bpm_analyzer.cpp, final diagnostic precedence (not root causes).
FINAL_PATHS = {
    "mir_primary": ("mir-primary", "primary", True),
    "mir_writer_override": ("mir-writer-override", "writer_override", True),
    "mir_review_hold_release": ("mir-review-hold-release", "review_hold_release", True),
    "mir_local_exact_upgrade": ("mir-local-exact-upgrade", "local_exact_upgrade", True),
    "mir_review_hold": ("mir-review-hold", "review_hold", False),
    "mir_no_candidate": ("mir-no-candidate", "no_candidate", False),
    "mir_writer_invariant_hold": ("mir-review-hold", "witness_hold", False),
    "none": ("none", "unknown", None),
}
STAGES = tuple(v[1] for v in FINAL_PATHS.values())
TOKEN = re.compile(r"[A-Za-z0-9_-]{1,96}\Z", re.ASCII)
SOURCE_LITERAL = re.compile(r'"([a-z][a-z0-9_]{3,95})"', re.ASCII)
RISK_STYLES = frozenset(("halftrap", "doubletrap", "switch", "nonperiodic", "tone"))
FIELDS = ("total", "clear", "clear_005", "clear_020", "clear_wrong", "clear_held",
          "clear_alias", "risk", "risk_accepted", "risk_held", "wrong_precise_export",
          "wrong_missing_precise_export", "wrong_output_matches_export",
          "wrong_output_missing_export")


def source_measured_reason_allowlist() -> frozenset[str]:
    """Whitelist literal public policy tokens from checked-in C++ source, not CSV."""
    path = Path(__file__).resolve().parents[1] / "src/foo_smart_tempo/mir_candidate_pipeline.cpp"
    literals = frozenset(SOURCE_LITERAL.findall(path.read_text(encoding="utf-8")))
    if "measured_half_pulse_recovery" not in literals or "frozen_writer_override_gate" not in literals:
        raise ValueError("public measured-policy source allowlist missing")
    # The source also contains the sentinel "invalid"; never whitelist it.
    return (literals - {"invalid"}) | {"none"}


def load_policy(path: Path, expected_ids: list[str],
                allowed_reasons: frozenset[str] | None = None) -> dict[str, dict[str, str]]:
    """Require one row for each public fixture, exact order, safe tokens and enums."""
    if len(expected_ids) != 16 or len(set(expected_ids)) != 16 or any(
            not re.fullmatch(r"challenge_[a-z0-9_]+", name, re.ASCII)
            for name in expected_ids):
        raise ValueError("invalid public complex inventory")
    allowed = source_measured_reason_allowlist() if allowed_reasons is None else allowed_reasons
    with path.open(encoding="utf-8-sig", newline="") as f:
        reader = csv.DictReader(f)
        if tuple(reader.fieldnames or ()) != POLICY_FIELDS:
            raise ValueError("complex policy CSV schema mismatch")
        rows = list(reader)
    if len(rows) != len(expected_ids):
        raise ValueError("complex policy row count mismatch")
    result = {}
    for expected, row in zip(expected_ids, rows):
        if set(row) != set(POLICY_FIELDS) or row["id"] != expected:
            raise ValueError("complex policy identity/order mismatch")
        if any(not isinstance(value, str) or not TOKEN.fullmatch(value) or
               value == "invalid" for value in row.values()):
            raise ValueError("unsafe complex policy token")
        reason = row["policy_reason"]
        if reason not in FINAL_PATHS or row["decision_class"] != FINAL_PATHS[reason][0]:
            raise ValueError("unknown or inconsistent final diagnostic path")
        if row["measured_policy_reason"] not in allowed:
            raise ValueError("unapproved measured policy source token")
        result[expected] = row
    return result


def audit_policy_stages(scored: dict, signals: dict[str, list[dict]],
                        policies: dict[str, dict[str, str]],
                        cases: tuple) -> dict:
    """Classify observed final path AFTER real-host analysis; never select BPM."""
    expected = [name for name, _, _ in cases]
    if len(expected) != 16 or len(set(expected)) != 16 or (
            set(policies) != set(expected) or set(signals) != set(expected) or
            [r["id"] for r in scored["results"]] != expected):
        raise ValueError("policy stage join identity mismatch")
    stages = {name: dict.fromkeys(FIELDS, 0) for name in STAGES}
    details = []
    for (name, reference, style), row in zip(cases, scored["results"]):
        policy = policies[name]
        reason = policy["policy_reason"]
        if reason not in FINAL_PATHS or policy["decision_class"] != FINAL_PATHS[reason][0]:
            raise ValueError("policy stage path drift")
        _, stage, must_accept = FINAL_PATHS[reason]
        held = bool(row["held"])
        bpm = row["reported_bpm"]
        if not isinstance(bpm, (int, float)) or not math.isfinite(bpm):
            raise ValueError("invalid observed BPM in stage join")
        accepted = not held and bpm > 0
        if held == accepted or (must_accept is not None and must_accept != accepted):
            raise ValueError("final policy path contradicts host observation")
        stat = stages[stage]
        stat["total"] += 1
        risk = style in RISK_STYLES
        if risk:
            stat["risk"] += 1
            stat["risk_accepted" if accepted else "risk_held"] += 1
            details.append((name, stage, "risk_accept" if accepted else "risk_hold", "na", "na"))
            continue
        if reference is None or not math.isfinite(reference) or reference <= 0:
            raise ValueError("missing clear tempo reference")
        stat["clear"] += 1
        if not accepted:
            stat["clear_held"] += 1
            details.append((name, stage, "held", "na", "na"))
            continue
        err = abs(bpm - reference)
        stat["clear_005"] += int(err <= .05 + 1e-9)
        stat["clear_020"] += int(err <= .20 + 1e-9)
        if err <= .20 + 1e-9:
            details.append((name, stage, "correct", "na", "na"))
            continue
        stat["clear_wrong"] += 1
        ratio = bpm / reference
        stat["clear_alias"] += int(min(abs(ratio - 2), abs(ratio - .5)) <= .04)
        board = []
        for signal in signals[name]:
            value = float(signal["bpm"])
            if not math.isfinite(value) or value <= 0:
                raise ValueError("invalid public measured candidate in stage join")
            board.append(value)
        precise = any(abs(v - reference) <= .20 + 1e-9 for v in board)
        matches = any(abs(v - bpm) <= .20 + 1e-9 for v in board)
        stat["wrong_precise_export" if precise else "wrong_missing_precise_export"] += 1
        stat["wrong_output_matches_export" if matches else "wrong_output_missing_export"] += 1
        details.append((name, stage, "wrong", str(int(precise)), str(int(matches))))
    totals = {field: sum(s[field] for s in stages.values()) for field in FIELDS}
    if (totals["total"] != 16 or totals["clear"] != 11 or totals["risk"] != 5 or
            totals["clear_020"] + totals["clear_wrong"] + totals["clear_held"] != 11 or
            totals["risk_accepted"] + totals["risk_held"] != 5 or
            totals["wrong_precise_export"] + totals["wrong_missing_precise_export"] != totals["clear_wrong"] or
            totals["wrong_output_matches_export"] + totals["wrong_output_missing_export"] != totals["clear_wrong"]):
        raise ValueError("stage attribution denominator mismatch")
    return {"stages": stages, "totals": totals, "details": details}
