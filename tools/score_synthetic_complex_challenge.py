#!/usr/bin/env python3
"""Read-only exact BPM and host-response scoring for public challenge fixtures."""
from __future__ import annotations

import argparse
import csv
import json
import math
from pathlib import Path
import statistics

from generate_synthetic_complex_challenge import CASES
from score_synthetic_bpm_reference import evaluate, load_observed



CANDIDATE_FIELDS = (
    "id", "rank", "bpm", "evidence", "support", "winner", "local_exact",
)


def load_candidates(path: Path) -> dict[str, list[float]]:
    """Validate bounded public-only measured board; duplicate rank is legal."""
    known = {name for name, _, _ in CASES}
    grouped: dict[str, list[float]] = {name: [] for name in known}
    with path.open(encoding="utf-8-sig", newline="") as f:
        reader = csv.DictReader(f)
        if tuple(reader.fieldnames or ()) != CANDIDATE_FIELDS:
            raise ValueError("challenge candidate schema mismatch")
        for row in reader:
            if set(row) != set(CANDIDATE_FIELDS) or row["id"] not in known:
                raise ValueError("non-public challenge candidate")
            rank = int(row["rank"])
            values = [float(row[k]) for k in CANDIDATE_FIELDS[2:]]
            if (not 1 <= rank <= 256 or len(grouped[row["id"]]) >= 32 or
                    not all(math.isfinite(v) for v in values) or
                    values[0] <= 0 or values[2] < 0 or values[3] < 0):
                raise ValueError("invalid or unbounded challenge candidate")
            grouped[row["id"]].append(values[0])
    return grouped


def audit_candidates(scored: dict, grouped: dict[str, list[float]]) -> dict:
    """Post-host labels diagnose board availability, NEVER select a BPM."""
    known = {name for name, _, _ in CASES}
    if set(grouped) != known:
        raise ValueError("challenge candidate identity mismatch")
    counts = dict(clear_with_precise=0, clear_without_precise=0,
                  wrong_with_precise=0, wrong_without_precise=0)
    wrong_details = []
    for row in scored["results"]:
        if row["ambiguous"] or row["reference_bpm"] is None:
            continue
        values = grouped[row["id"]]
        ref = row["reference_bpm"]
        closest = min((abs(v - ref) for v in values), default=None)
        near = closest is not None and closest <= .20 + 1e-9
        counts["clear_with_precise" if near else "clear_without_precise"] += 1
        error = row["absolute_error_bpm"]
        if error is not None and error > .20 + 1e-9:
            counts["wrong_with_precise" if near else "wrong_without_precise"] += 1
            wrong_details.append((row["id"], int(near), closest))
    counts["wrong_details"] = wrong_details
    return counts


def classify_challenge(scored: dict) -> dict:
    """Keep tempo-switch, metrical ambiguity and nonrhythmic negatives apart."""
    counts = dict(clear_correct=0, clear_wrong=0, clear_held=0,
                  clear_half_double=0, metrical_accepted=0,
                  nonstationary_accepted=0, nonrhythmic_accepted=0)
    for (name, _, style), row in zip(CASES, scored["results"]):
        if row["id"] != name:
            raise ValueError("challenge result identity mismatch")
        accepted = not row["held"] and row["reported_bpm"] > 0
        if style in ("halftrap", "doubletrap"):
            counts["metrical_accepted"] += int(accepted)
        elif style == "switch":
            counts["nonstationary_accepted"] += int(accepted)
        elif style in ("nonperiodic", "tone"):
            counts["nonrhythmic_accepted"] += int(accepted)
        elif not accepted:
            counts["clear_held"] += 1
        elif row["absolute_error_bpm"] <= .20 + 1e-9:
            counts["clear_correct"] += 1
        else:
            counts["clear_wrong"] += 1
            ratio = row["reported_bpm"] / row["reference_bpm"]
            counts["clear_half_double"] += int(
                min(abs(ratio - 2.0), abs(ratio - 0.5)) <= .04)
    if sum(counts[k] for k in ("clear_correct", "clear_wrong", "clear_held")) != 11:
        raise ValueError("challenge clear denominator mismatch")
    return counts


def score(manifest: dict, observed: list[dict], timing: list[dict],
          candidates: dict[str, list[float]] | None = None) -> dict:
    expected = list(CASES)
    rows = manifest.get("cases")
    if (manifest.get("schema") != "synthetic_bpm_reference_v1" or
            not isinstance(rows, list) or len(rows) != len(expected)):
        raise ValueError("challenge manifest schema/count mismatch")
    if len(timing) != len(expected):
        raise ValueError("host timing count mismatch")
    for i, ((name, bpm, style), row, elapsed) in enumerate(
            zip(expected, rows, timing)):
        if (row.get("id") != name or
                row.get("style") != "stress_" + style or
                row.get("reference_bpm") != bpm):
            raise ValueError("challenge reference mismatch")
        ambiguity = ("meter_underdetermined" if style in
                     ("halftrap", "doubletrap") else
                     "tempo_nonstationary" if style == "switch" else "none")
        if row.get("ambiguity") != ambiguity:
            raise ValueError("unexpected metrical ambiguity label")
        if (elapsed.get("id") != name or
                elapsed.get("index") != str(i)):
            raise ValueError("timing identity mismatch")
        millis = float(elapsed["analysis_ms"])
        if not math.isfinite(millis) or millis <= 0:
            raise ValueError("invalid host response time")
    result = evaluate(manifest, observed)
    result["challenge_classes"] = classify_challenge(result)
    if candidates is not None:
        result["candidate_audit"] = audit_candidates(result, candidates)
    millis = [float(row["analysis_ms"]) for row in timing]
    result["host_response_ms_median"] = statistics.median(millis)
    result["host_response_ms_p95"] = sorted(millis)[
        math.ceil(0.95 * len(millis)) - 1]
    return result


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--observed", type=Path, required=True)
    parser.add_argument("--timing", type=Path, required=True)
    parser.add_argument("--candidates", type=Path, required=True)
    args = parser.parse_args()
    with args.manifest.open(encoding="utf-8-sig") as f:
        manifest = json.load(f)
    observed = load_observed(args.observed)
    with args.timing.open(encoding="utf-8-sig", newline="") as f:
        reader = csv.DictReader(f)
        if tuple(reader.fieldnames or ()) != ("index", "id", "analysis_ms"):
            raise ValueError("unexpected timing CSV header")
        timing = list(reader)
    candidates = load_candidates(args.candidates)
    result = score(manifest, observed, timing, candidates)
    print(f"CHALLENGE_REAL_CASES={result['n']}")
    print(f"CHALLENGE_CLEAR_CASES={result['precision']['clear_reference_n']}")
    print(f"CHALLENGE_ACCEPTED_CLEAR={result['precision']['accepted_reference_n']}")
    print(f"CHALLENGE_PRECISE_0_05={result['precision']['within_0_05_bpm']}")
    print(f"CHALLENGE_PRECISE_0_20={result['precision']['within_0_20_bpm']}")
    print(f"CHALLENGE_UNSAFE_ACCEPTS={result['unsafe_count']}")
    print(f"CHALLENGE_AMBIGUOUS_ACCEPTS={result['counts']['ambiguous_accept']}")
    classes = result["challenge_classes"]
    audit = result["candidate_audit"]
    for key in ("clear_correct", "clear_wrong", "clear_held",
                "clear_half_double", "metrical_accepted",
                "nonstationary_accepted", "nonrhythmic_accepted"):
        print(f"CHALLENGE_{key.upper()}={classes[key]}")
    for key in ("clear_with_precise", "clear_without_precise",
                "wrong_with_precise", "wrong_without_precise"):
        print(f"CHALLENGE_BOARD_{key.upper()}={audit[key]}")
    for name, near, closest in audit["wrong_details"]:
        value = "none" if closest is None else f"{closest:.6f}"
        print(f"CHALLENGE_WRONG_CASE id={name} precise_board={near} nearest_error_bpm={value}")
    print(f"CHALLENGE_MEDIAN_HOST_RESPONSE_MS={result['host_response_ms_median']:.0f}")
    print(f"CHALLENGE_P95_HOST_RESPONSE_MS={result['host_response_ms_p95']:.0f}")
    print(f"CHALLENGE_QUALITY_VERDICT={result['quality_verdict']}")
    # Never use this research verdict as a technical CI release gate.


if __name__ == "__main__":
    main()
