#!/usr/bin/env python3
"""Read-only post-host Selector/Policy ablation. No production BPM selection.

Frozen reference is joined ONLY after model decisions have been computed.
This compares competing source-measured decisions, never genre or oracle BPM.
"""
import argparse
import math
from pathlib import Path

MODELS = ("NATIVE_FINAL", "PRIMARY_ONLY", "COMBINED", "AGREEMENT_ONLY")
FIELDS = ("ACCEPTED", "HELD", "CORRECT_005", "CORRECT_020", "WRONG_AUTO", "FALSE_ACCEPT")


class AblationError(ValueError):
    pass


def freeze(rows):
    """Decide using numeric same-run candidates; oracle/track identity unavailable."""
    from audit_smart_tempo_b_shadow_rank import fixed_order
    cluster = [r for r in rows if r["stage"] == "candidate_transform"]
    eligibility = [r for r in rows if r["stage"] == "primary_eligibility"]
    order = fixed_order(cluster, eligibility)["COMBINED"]
    candidates = {r["candidate_id"]: r["metrics"]["bpm"] for r in cluster}
    primary = [r for r in rows if r["stage"] == "primary_select"]
    final = [r for r in rows if r["stage"] == "final_output"]
    if len(primary) != 1 or len(final) != 1:
        raise AblationError("missing or repeated native final/Primary")
    primary, final = primary[0], final[0]
    parents = primary["parent_candidate_ids"]
    outcome = primary["outcome"]
    if outcome not in ("selected", "review_hold", "no_candidate"):
        raise AblationError("unknown Primary outcome")
    if not isinstance(parents, list) or len(parents) > 1:
        raise AblationError("ambiguous Primary source")
    if outcome == "no_candidate":
        if parents or primary["metrics"]["bpm"] != 0:
            raise AblationError("fabricated no-candidate Primary")
        chosen = None
    else:
        if len(parents) != 1 or parents[0] not in candidates:
            raise AblationError("selected Primary without measured source")
        chosen = parents[0]
        if abs(candidates[chosen] - primary["metrics"]["bpm"]) >= .001:
            raise AblationError("Primary BPM-source mismatch")
    fbpm = final["metrics"]["bpm"]
    if final["outcome"] not in ("selected", "review_hold") or (
        (fbpm == 0) != (final["outcome"] == "review_hold")
    ):
        raise AblationError("invalid final disposition")
    if not candidates and (fbpm != 0 or chosen is not None or order):
        raise AblationError("empty board has a candidate")
    combined = order[0] if order else None
    native_primary_accepted = outcome == "selected"
    agreement = native_primary_accepted and chosen is not None and chosen == combined
    return {
        "NATIVE_FINAL": fbpm,
        "PRIMARY_ONLY": candidates[chosen] if native_primary_accepted else 0.0,
        "COMBINED": candidates[combined] if combined else 0.0,
        "AGREEMENT_ONLY": candidates[chosen] if agreement else 0.0,
    }


def score(frozen, reference):
    """Labels score frozen output only. No BPM reference enters freeze()."""
    if set(frozen) != set(MODELS):
        raise AblationError("incomplete/extra model")
    if reference is not None and (
        type(reference) not in (int, float) or not math.isfinite(reference) or reference <= 0
    ):
        raise AblationError("invalid independent ground truth")
    result = {}
    for model in MODELS:
        bpm = frozen[model]
        if type(bpm) not in (int, float) or not math.isfinite(bpm) or bpm < 0:
            raise AblationError("invalid source BPM")
        held = bpm == 0
        result[model] = {
            "ACCEPTED": int(not held),
            "HELD": int(held),
            "CORRECT_005": int(reference is not None and not held and abs(bpm - reference) <= .05),
            "CORRECT_020": int(reference is not None and not held and abs(bpm - reference) <= .20),
            "WRONG_AUTO": int(reference is not None and not held and abs(bpm - reference) > .20),
            "FALSE_ACCEPT": int(reference is None and not held),
        }
    return result


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--case", required=True)
    p.add_argument("--manifest", type=Path, required=True)
    p.add_argument("--observed", type=Path, required=True)
    p.add_argument("--trace", type=Path, required=True)
    args = p.parse_args()
    try:
        import json
        from audit_smart_tempo_b_candidate_recall import CASES, analyze, unique_object
        from audit_smart_tempo_ci_pipeline_trace import load, validate
        from score_synthetic_bpm_reference import load_observed
        if args.case not in CASES or not args.manifest.is_file() or not 0 < args.manifest.stat().st_size < 32768:
            raise AblationError("unknown independent synthetic control")
        rows = load(args.trace)
        validate(rows, require_windows=True)
        manifest = json.loads(args.manifest.read_text(encoding="utf-8"), object_pairs_hook=unique_object)
        observed = load_observed(args.observed)
        analyze(rows, manifest, observed, args.case)
        frozen = freeze(rows)
        results = score(frozen, CASES[args.case][0])
    except (ValueError, KeyError, TypeError, OSError, UnicodeError) as exc:
        p.error(str(exc))
    print("SMART_TEMPO_B_ABLATIONS_VERIFIED=1")
    for model in MODELS:
        for field in FIELDS:
            print("SMART_TEMPO_B_ABLATION_" + model + "_" + field + "=" + str(results[model][field]))


if __name__ == "__main__":
    main()
