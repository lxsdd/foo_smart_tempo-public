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


def score(manifest: dict, observed: list[dict], timing: list[dict]) -> dict:
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
    args = parser.parse_args()
    with args.manifest.open(encoding="utf-8-sig") as f:
        manifest = json.load(f)
    observed = load_observed(args.observed)
    with args.timing.open(encoding="utf-8-sig", newline="") as f:
        reader = csv.DictReader(f)
        if tuple(reader.fieldnames or ()) != ("index", "id", "analysis_ms"):
            raise ValueError("unexpected timing CSV header")
        timing = list(reader)
    result = score(manifest, observed, timing)
    print(f"CHALLENGE_REAL_CASES={result['n']}")
    print(f"CHALLENGE_CLEAR_CASES={result['precision']['clear_reference_n']}")
    print(f"CHALLENGE_ACCEPTED_CLEAR={result['precision']['accepted_reference_n']}")
    print(f"CHALLENGE_PRECISE_0_05={result['precision']['within_0_05_bpm']}")
    print(f"CHALLENGE_PRECISE_0_20={result['precision']['within_0_20_bpm']}")
    print(f"CHALLENGE_UNSAFE_ACCEPTS={result['unsafe_count']}")
    print(f"CHALLENGE_AMBIGUOUS_ACCEPTS={result['counts']['ambiguous_accept']}")
    print(f"CHALLENGE_FALSE_TEMPO={result['counts']['false_positive_nonrhythmic']}")
    print(f"CHALLENGE_MEDIAN_HOST_RESPONSE_MS={result['host_response_ms_median']:.0f}")
    print(f"CHALLENGE_P95_HOST_RESPONSE_MS={result['host_response_ms_p95']:.0f}")
    print(f"CHALLENGE_QUALITY_VERDICT={result['quality_verdict']}")
    # Never use this research verdict as a technical CI release gate.


if __name__ == "__main__":
    main()
