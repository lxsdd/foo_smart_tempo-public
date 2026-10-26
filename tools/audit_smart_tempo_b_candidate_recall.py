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

# Immutable, public, predeclared four-case PHASE-B pilot. Not the Phase-C set.
CASES = {
    "pulse_60": (60.0, "regular"),
    "pulse_128": (128.0, "regular"),
    "pulse_120_sparse": (120.0, "sparse"),
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
        if not near["raw"]:
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
    except (RecallError, TraceError, ValueError, UnicodeError, KeyError) as exc:
        p.error(str(exc))
    # Fixed, predeclared numeric-only source comparison. Never print filenames,
    # private identifiers, metadata, or dynamically chosen reference BPM.
    print("SMART_TEMPO_B_RECALL_VERIFIED=1")
    for name, value in result.items():
        print("SMART_TEMPO_B_RECALL_" + name + "=" + str(value))


if __name__ == "__main__":
    main()
