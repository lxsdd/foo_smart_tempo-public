#!/usr/bin/env python3
"""Independent synthetic-only MIR candidate recall: WHY did a BPM miss?

Ground truth is loaded only here, AFTER the native foobar host has completed.
Never pass it into MIR, Policy, foobar, tags, or a component.
"""
import argparse
import json
import math
from pathlib import Path
import sys

from audit_smart_tempo_ci_pipeline_trace import load, validate, TraceError
from score_synthetic_bpm_reference import load_observed

# Frozen original four-case Phase-B pilot plus separate prospective six-case
# synthetic selector generalization set. Neither is private music or Phase C.
CASES = {
    "pulse_60": (60.0, "regular"),
    "pulse_128": (128.0, "regular"),
    "pulse_120_sparse": (120.0, "sparse"),
    "probe_72_regular": (72.0, "regular"),
    "probe_104_offbeat": (104.0, "offbeat"),
    "probe_132_swing": (132.0, "swing"),
    "probe_156_half_pulse": (156.0, "half_pulse"),
    "probe_90_polyrhythm": (90.0, "polyrhythm"),
    "probe_150_sparse": (150.0, "sparse"),
    "silence": (None, "silence"),
}
STAGES = {
    "raw": "candidate_create",
    "cluster": "candidate_transform",
    "policy_board": "policy_eligibility",
}
REASONS = frozenset((
    "reference_in_selected_output", "missing_raw_reference",
    "missing_cluster_reference", "missing_policy_board_reference",
    "primary_selected_other", "policy_selected_other",
    "final_selected_other", "reference_present_held",
    "negative_abstained", "negative_falseaccept",
))


class RecallError(ValueError):
    pass


def unique_object(pairs):
    result = {}
    for key, val in pairs:
        if key in result:
            raise RecallError("duplicate reference field")
        result[key] = val
    return result


def analyze(rows: list[dict], manifest: dict, observed: list[dict],
            case_id: str) -> dict:
    if case_id not in CASES:
        raise RecallError("unapproved synthetic case")
    expected_bpm, expected_style = CASES[case_id]
    if (type(manifest) is not dict or
            set(manifest) != {"schema", "cases"} or
            manifest["schema"] != "synthetic_bpm_reference_v1" or
            type(manifest["cases"]) is not list or
            len(manifest["cases"]) != 1):
        raise RecallError("not a one-case synthetic oracle")
    case = manifest["cases"][0]
    if (type(case) is not dict or
            set(case) != {"id", "reference_bpm", "style", "duration_sec",
                          "sample_rate", "event_sample_positions", "wav_sha256"} or
            case["id"] != case_id or case["style"] != expected_style or
            type(case["duration_sec"]) is not int or
            case["duration_sec"] != 20 or
            type(case["sample_rate"]) is not int or
            case["sample_rate"] != 16000 or
            type(case["wav_sha256"]) is not str or
            len(case["wav_sha256"]) != 64 or
            any(c not in "0123456789abcdef" for c in case["wav_sha256"]) or
            type(case["event_sample_positions"]) is not list or
            any(type(v) is not int or not 0 <= v < 320000
                for v in case["event_sample_positions"])):
        raise RecallError("synthetic oracle manifest drift")
    supplied = case["reference_bpm"]
    if (expected_bpm is None and supplied is not None or
            expected_bpm is not None and
            (type(supplied) not in (float, int) or supplied != expected_bpm)):
        raise RecallError("reference BPM has been changed")
    if len(observed) != 1 or type(observed[0]) is not dict or (
            set(observed[0]) != {"index", "bpm", "held"} or
            observed[0]["index"] != "0" or
            observed[0]["held"] not in ("0", "1")):
        raise RecallError("invalid host BPM observation")
    try:
        observed_bpm = float(observed[0]["bpm"])
    except (ValueError, TypeError):
        raise RecallError("invalid host BPM number") from None
    if not math.isfinite(observed_bpm) or observed_bpm < 0:
        raise RecallError("nonfinite/negative host BPM")
    stages = {}
    for label, stage in STAGES.items():
        points = [r["metrics"]["bpm"] for r in rows if r["stage"] == stage]
        if any(type(n) not in (int, float) or not math.isfinite(n) or n <= 0
               for n in points):
            raise RecallError("invalid source candidate BPM")
        stages[label] = points
    def single(stage):
        entries = [r for r in rows if r["stage"] == stage]
        if len(entries) != 1:
            raise RecallError("missing or duplicate native terminal stage")
        return entries[0]
    primary = single("primary_select")
    policy = single("policy_change")
    final = single("final_output")
    final_bpm = final["metrics"]["bpm"]
    if abs(observed_bpm - final_bpm) > 0.001:
        raise RecallError("host final BPM diverges from native final witness")
    accepted = observed_bpm > 0 and observed[0]["held"] == "0"
    if accepted != (final["outcome"] == "selected"):
        raise RecallError("host accepted/held conflicts with native final state")
    if expected_bpm is None:
        if stages["raw"] or stages["cluster"] or stages["policy_board"]:
            raise RecallError("silence falsely contains native candidate sources")
        reason = "negative_falseaccept" if accepted else "negative_abstained"
        near = {label: 0 for label in stages}
    else:
        near = {label: sum(abs(n - expected_bpm) <= 0.20
                           for n in values)
                for label, values in stages.items()}
        close = lambda bpm: abs(bpm - expected_bpm) <= 0.20
        # A final audio-measured local-exact refinement can be correct even
        # when no upstream cluster lies within the strict oracle tolerance.
        # Final success takes precedence; stage availability is reported
        # independently through the near-source counters.
        if accepted and close(final_bpm):
            reason = "reference_in_selected_output"
        elif not near["raw"]:
            reason = "missing_raw_reference"
        elif not near["cluster"]:
            reason = "missing_cluster_reference"
        elif not near["policy_board"]:
            reason = "missing_policy_board_reference"
        elif not close(primary["metrics"]["bpm"]):
            reason = "primary_selected_other"
        elif not close(policy["metrics"]["bpm"]):
            reason = "policy_selected_other"
        elif not close(final_bpm):
            reason = "final_selected_other"
        elif not accepted:
            reason = "reference_present_held"
        else:
            reason = "reference_in_selected_output"
    if reason not in REASONS:
        raise RecallError("unclassified candidate outcome")
    return {
        "CASE": case_id,
        "RAW_TOTAL": len(stages["raw"]),
        "RAW_NEAR_020": near["raw"],
        "CLUSTER_TOTAL": len(stages["cluster"]),
        "CLUSTER_NEAR_020": near["cluster"],
        "POLICY_BOARD_TOTAL": len(stages["policy_board"]),
        "POLICY_BOARD_NEAR_020": near["policy_board"],
        "PRIMARY_NEAR_020": int(expected_bpm is not None and
                                abs(primary["metrics"]["bpm"] - expected_bpm) <= .20),
        "POLICY_NEAR_020": int(expected_bpm is not None and
                               abs(policy["metrics"]["bpm"] - expected_bpm) <= .20),
        "FINAL_ACCEPTED_NEAR_020": int(expected_bpm is not None and
                                        accepted and
                                        abs(final_bpm - expected_bpm) <= .20),
        "ROOT_CAUSE_STAGE": reason,
    }



def diagnose_primary_comparator(rows: list[dict], case_id: str) -> dict:
    """Post-host, reference-conditioned source-object audit; never a first-veto claim.

    The caller MUST validate the native trace with require_windows=True before
    joining the frozen synthetic reference. Raw aliases are matched only through
    the independently validated raw->collapsed parent IDs, not BPM proximity.
    A comparator winner in an unchosen family is NOT a Primary lane winner.
    """
    if case_id not in CASES:
        raise RecallError("unapproved synthetic comparator case")
    reference, _ = CASES[case_id]
    clusters = [r for r in rows if r["stage"] == "candidate_transform"]
    primary = [r for r in rows if r["stage"] == "primary_select"]
    if len(primary) != 1:
        raise RecallError("missing native primary decision")
    if reference is None:
        if clusters or any(r["stage"] in ("primary_rank", "primary_compare")
                           for r in rows):
            raise RecallError("negative comparator fabricated sources")
        return {"PRIMARY_COMPARATOR_PROVENANCE": "PASS",
                "PRIMARY_NEAR_RANKED_SOURCES": 0,
                "PRIMARY_NEAR_COMPARATOR_WINS": 0,
                "PRIMARY_NEAR_FAMILY_FINAL_WINNERS": 0,
                "PRIMARY_FIRST_OBSERVED_EXCLUSION": "none",
                "PRIMARY_COMPARATOR_DIAGNOSIS": "negative_no_candidates"}
    near = [r for r in clusters
            if abs(r["metrics"]["bpm"] - reference) <= 0.20]
    near_clusters = {r["candidate_id"] for r in near}
    if any(type(r["candidate_id"]) is not str or
           type(r["parent_candidate_ids"]) is not list or
           any(type(parent) is not str for parent in r["parent_candidate_ids"])
           for r in near):
        raise RecallError("unindexed native reference cluster")
    near_sources = near_clusters | {
        parent for cluster in near for parent in cluster["parent_candidate_ids"]
    }
    ranks = [r for r in rows if r["stage"] == "primary_rank" and
             r["candidate_id"] in near_sources]
    comparisons = [r for r in rows if r["stage"] == "primary_compare"]
    # The independent native validator already replays adjacent rank/compare
    # scores and wins. Here we retain the LAST winner in each executed family
    # (and each low-pulse bestBy epoch) without synthesizing an unrun branch.
    winners = {}
    near_wins = 0
    for row in comparisons:
        family = row["reason_code"].rsplit("_compare_", 1)[0]
        if family not in ("raw", "harmonic", "low", "general_baseline",
                          "general_alt", "residual_high", "residual_harmonic",
                          "harmonic_direct"):
            raise RecallError("unrecognized native comparator family")
        key = (family, row["metrics"].get("low_pool_epoch", 0))
        if row["outcome"] == "included":
            winners[key] = row["candidate_id"]
            near_wins += row["candidate_id"] in near_sources
        elif row["outcome"] != "excluded":
            raise RecallError("unrecognized comparator result")
    near_final_winners = sum(source in near_sources for source in winners.values())
    excluded = next((
        r["reason_code"] for r in rows
        if r["stage"] == "candidate_filter" and
        r["candidate_id"] in near_sources and r["outcome"] == "excluded"
    ), "none")
    # This is the FIRST OBSERVED provisional exclusion, not necessarily a
    # decisive veto: fallback and nested rescues may later re-admit sources.
    from audit_smart_tempo_ci_pipeline_trace import ALLOWED_FILTER_REASONS
    if excluded != "none" and excluded not in ALLOWED_FILTER_REASONS:
        raise RecallError("unrecognized native exclusion reason")
    chosen = primary[0]["parent_candidate_ids"]
    if any(source in near_clusters for source in chosen):
        diagnosis = "reference_primary_selected"
    elif not near_clusters:
        diagnosis = "reference_missing_cluster"
    elif not ranks:
        diagnosis = "reference_not_ranked"
    elif not near_final_winners:
        diagnosis = "reference_ranked_lost"
    else:
        diagnosis = "reference_won_nonterminal_family"
    return {
        "PRIMARY_COMPARATOR_PROVENANCE": "PASS",
        "PRIMARY_NEAR_RANKED_SOURCES": len({r["candidate_id"] for r in ranks}),
        "PRIMARY_NEAR_COMPARATOR_WINS": near_wins,
        "PRIMARY_NEAR_FAMILY_FINAL_WINNERS": near_final_winners,
        "PRIMARY_FIRST_OBSERVED_EXCLUSION": excluded,
        "PRIMARY_COMPARATOR_DIAGNOSIS": diagnosis,
    }

def main():
    p = argparse.ArgumentParser()
    p.add_argument("--case", choices=sorted(CASES), required=True)
    p.add_argument("--manifest", type=Path, required=True)
    p.add_argument("--observed", type=Path, required=True)
    p.add_argument("--trace", type=Path, required=True)
    args = p.parse_args()
    try:
        if not args.manifest.is_file() or not 0 < args.manifest.stat().st_size < 32768:
            raise RecallError("missing/oversized independent oracle")
        reference = json.loads(args.manifest.read_text(encoding="utf-8"),
                               object_pairs_hook=unique_object)
        rows = load(args.trace)
        validate(rows, require_windows=True)
        result = analyze(rows, reference, load_observed(args.observed), args.case)
        result.update(diagnose_primary_comparator(rows, args.case))
    except (RecallError, TraceError, ValueError, UnicodeError, KeyError) as exc:
        p.error(str(exc))
    # Fixed, predeclared numeric-only source comparison. Never print filenames,
    # private identifiers, metadata, or dynamically chosen reference BPM.
    print("SMART_TEMPO_B_RECALL_VERIFIED=1")
    for name, value in result.items():
        print("SMART_TEMPO_B_RECALL_" + name + "=" + str(value))


if __name__ == "__main__":
    main()
