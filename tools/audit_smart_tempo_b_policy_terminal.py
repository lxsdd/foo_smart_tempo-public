#!/usr/bin/env python3
"""Independent CI-only witness for the actual final native Policy reason/action.

A reason is a native terminal branch label, NOT a complete proof of every
nested candidate eligibility predicate. No labels, tracks or ground truth.
"""
from __future__ import annotations

REASONS = frozenset((
    "center_anchored_writer_override_gate",
    "disabled",
    "empty_measured_fullboard",
    "frozen_soft_center_review_promotion_gate",
    "frozen_writer_family_conflict_gate",
    "frozen_writer_override_gate",
    "generic_three_halves_family_conflict",
    "measured_alternative_family_conflict_release_gate",
    "measured_candidate_family_conflict_release_gate",
    "measured_center_challenger_family_conflict_release_gate",
    "measured_center_continuous_recovery",
    "measured_center_exact_family_conflict_release_gate",
    "measured_centered_continuous_harmonic_release",
    "measured_current_family_conflict_retention_gate",
    "measured_direct_high_support_alternative_family_conflict_release_gate",
    "measured_dominant_direct_fullboard_release",
    # These exact source-emitted Policy terminal reasons are independently
    # distinguished from the action/outcome safety gate below.
    "dominant_measured_family_conflict_release_gate",
    "measured_residual_downward_family_recovery_gate",
    "measured_strict_direct_downward_family_recovery_gate",
    "measured_winner_half_family_recovery_gate",
    "measured_independent_high_pulse_retention_gate",
    "measured_exact_half_family_recovery_gate",
    "measured_broad_route_cross_view_family_recovery_gate",
    "measured_dominant_segment_family_recovery_gate",
    "measured_downward_family_conflict_release_gate",
    "measured_evidence_dominates_center_conflict_gate",
    "measured_family_conflict_evidence_release_gate",
    "measured_half_pulse_recovery",
    "measured_high_family_conflict_fullboard_release",
    "measured_low_pulse_four_thirds_fullboard_release",
    "measured_low_pulse_hold_recovery",
    "measured_lower_harmonic_family_conflict_release_gate",
    "measured_material_risk_local_exact_release",
    "measured_material_risk_low_pulse_release",
    "measured_material_risk_same_pulse_release",
    "measured_normalized_winner_family_conflict_release_gate",
    "measured_out_of_rail_primary_witness_release",
    "measured_partial_bar_cross_view_bridge_release",
    "measured_partial_bar_dominant_octave_release",
    "measured_partial_bar_exact_half_recovery_gate",
    "measured_partial_bar_family_conflict_release",
    "measured_partial_bar_topk_exact_center_release",
    "measured_partial_bar_topk_exact_rational_release",
    "measured_protected_current_release_gate",
    "measured_protected_direct_release_gate",
    "measured_protected_half_alias_current_release_gate",
    "measured_protected_near_release_gate",
    "measured_protected_strong_direct_release_gate",
    "measured_release_ready_anchor_family_conflict_gate",
    "measured_release_ready_center_family_conflict_gate",
    "measured_release_ready_full_pulse",
    "measured_release_ready_high_family_winner",
    "measured_release_ready_low_anchor",
    "measured_release_ready_low_family_plateau",
    "measured_release_ready_material_risk_winner",
    "measured_release_ready_sparse_family",
    "measured_residual_margin_family_conflict_release_gate",
    "measured_residual_two_thirds_family_conflict_release_gate",
    "measured_sparse_current_direct_release_gate",
    "measured_sparse_low_pulse_current_release",
    "measured_sparse_pulse_hold_recovery",
    "measured_sparse_top_family_half_alias_release",
    "measured_strong_centered_winner_family_conflict_release_gate",
    "measured_strong_continuous_alternative_family_conflict_release_gate",
    "measured_strong_pulse_alternative_family_conflict_release_gate",
    "outside_frozen_policy_surfaces",
    "protected_review_hold_reason",
    "review_hold_promotion_gate_veto",
    "unmeasured_policy_output_veto",
    "weak_current_strong_remote_family_conflict",
    "writer_override_gate_veto",
))
ACTIONS = {
    0: "none",
    1: "keep_current",
    2: "keep_review_hold",
    3: "writer_override",
    4: "review_hold_promotion",
    5: "family_conflict_review_hold",
    6: "broad_route_conflict_review_hold",
    7: "invariant_review_hold",
}


class PolicyTerminalError(ValueError):
    pass


def reconcile(rows: list[dict], *, required: bool = False) -> dict:
    terminals = [r for r in rows if r["stage"] == "policy_terminal"]
    result = {"native_policy_terminal_verified": 0,
              "native_policy_terminal_reason": "not_observed",
              "native_policy_terminal_action": "not_observed"}
    if not terminals and not required:
        return result
    pointers = [r for r in rows if r["stage"] == "policy_pointer"]
    changes = [r for r in rows if r["stage"] == "policy_change"]
    comparisons = [r for r in rows if r["stage"] == "policy_resolution"]
    if any(len(x) != 1 for x in (terminals, pointers, changes, comparisons)):
        raise PolicyTerminalError("missing/duplicate native Policy terminal chain")
    t, pointer, change, comparison = (
        terminals[0], pointers[0], changes[0], comparisons[0])
    reason = t["reason_code"]
    m = t["metrics"]
    code = m.get("action_code")
    if reason not in REASONS or type(code) is not int or code not in ACTIONS:
        raise PolicyTerminalError("unknown native reason/action")
    if (t["candidate_id"] is not None or
            t["parent_candidate_ids"] != pointer["parent_candidate_ids"] or
            t["parent_candidate_ids"] != change["parent_candidate_ids"] or
            not pointer["sequence"] < t["sequence"] < change["sequence"] or
            t["outcome"] != change["outcome"] or
            any(abs(m[key] - other["metrics"][key]) > 0.001
                for key, other in (("bpm", change), ("bpm", pointer),
                                   ("bpm", comparison))) or
            m["score"] != comparison["metrics"]["score"] or
            m["support"] != comparison["metrics"]["support"]):
        raise PolicyTerminalError("terminal source/BPM/flags contradict applied Policy")
    policy_reason = change["reason_code"]
    expected = ("writer_override" if code == 3 else
                "hold_release" if code == 4 else
                "keep_primary" if code == 1 and t["outcome"] == "selected" else
                "no_policy_write")
    if policy_reason != expected:
        raise PolicyTerminalError("native action contradicts Policy decision")
    # Source-emitted recovery/release labels cannot authorize a different
    # native action; do not weaken unrecognized-reason rejection.
    override_reasons = frozenset((
        "dominant_measured_family_conflict_release_gate",
        "measured_residual_downward_family_recovery_gate",
        "measured_strict_direct_downward_family_recovery_gate",
        "measured_winner_half_family_recovery_gate",
        "measured_exact_half_family_recovery_gate",
        "measured_broad_route_cross_view_family_recovery_gate",
    ))
    if ((reason in override_reasons and code != 3) or
            (reason == "measured_independent_high_pulse_retention_gate" and
             code != 1)):
        raise PolicyTerminalError("native reason/action mismatch")
    if ((code == 3 and m["score"] != 1) or
            (code == 4 and m["support"] != 1) or
            (code not in (3, 4) and (m["score"] or m["support"])) or
            (code in (0, 2, 5, 6, 7) and t["outcome"] == "selected") or
            (reason in ("disabled", "empty_measured_fullboard") and code != 0) or
            (reason == "unmeasured_policy_output_veto" and code != 7) or
            (reason == "frozen_soft_center_review_promotion_gate" and code != 4) or
            (reason == "review_hold_promotion_gate_veto" and code != 2)):
        raise PolicyTerminalError("reason/action/outcome safety contradiction")
    return {"native_policy_terminal_verified": 1,
            "native_policy_terminal_reason": reason,
            "native_policy_terminal_action": ACTIONS[code]}
