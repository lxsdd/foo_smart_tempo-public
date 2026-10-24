#!/usr/bin/env python3
"""Compare measured BPM with declared, synthetic-only pulse references.

This scorer never claims that a synthetic beat is equivalent to human tempo
perception. Silence, deliberate halftime patterns, and swing are reported
separately. No user audio, reference recordings or personal paths.
"""
import argparse
import csv
import json
import math
from pathlib import Path


def evaluate(reference, observed):
    rows = reference["cases"]
    if len(rows) != len(observed):
        raise ValueError("observation count mismatched")
    if len({c["id"] for c in rows}) != len(rows):
        raise ValueError("duplicate synthetic case identity")
    scored = []
    for expected, got in zip(rows, observed):
        value = float(got["bpm"])
        held = str(got["held"]) == "1"
        if not math.isfinite(value) or value < 0:
            raise ValueError("non-finite/negative BPM")
        bpm = expected["reference_bpm"]
        if held and value > 0:
            # A held candidate is not an accepted BPM even if it is measured.
            accepted = False
        else:
            accepted = value > 0 and not held
        if bpm is None:
            category = "correct_abstention" if not accepted else "false_positive_silence"
            relative_error = None
        elif not accepted:
            category = "abstained"
            relative_error = None
        else:
            relative_error = abs(value - bpm) / bpm
            if relative_error <= 0.02:
                category = "within_two_percent"
            elif min(abs(value / bpm - 2), abs(value / bpm - .5)) <= 0.04:
                category = "half_double_alias"
            else:
                category = "other_error"
        scored.append({"id": expected["id"], "style": expected["style"],
                       "reference_bpm": bpm, "reported_bpm": value,
                       "held": held, "category": category,
                       "relative_error": relative_error})
    counts = {kind: sum(row["category"] == kind for row in scored)
              for kind in ("within_two_percent", "half_double_alias",
                           "other_error", "abstained",
                           "correct_abstention", "false_positive_silence")}
    return {"schema": "synthetic_bpm_score_v1", "n": len(rows),
            "counts": counts, "results": scored}


def load_observed(path):
    with path.open(newline="", encoding="utf-8") as handle:
        entries = list(csv.DictReader(handle))
    if any(set(entry) != {"index", "bpm", "held"} for entry in entries):
        raise ValueError("observation header mismatch")
    if [int(entry["index"]) for entry in entries] != list(range(len(entries))):
        raise ValueError("observation ordinal mismatch")
    if any(entry["held"] not in ("0", "1") for entry in entries):
        raise ValueError("bad held flag")
    return entries


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--observed", required=True, type=Path)
    parser.add_argument("--json-report", type=Path)
    args = parser.parse_args()
    reference = json.loads(args.manifest.read_text(encoding="utf-8"))
    if reference.get("schema") != "synthetic_bpm_reference_v1":
        parser.error("invalid synthetic reference schema")
    scored = evaluate(reference, load_observed(args.observed))
    if args.json_report:
        if args.json_report.exists():
            parser.error("refuse to overwrite existing report")
        args.json_report.write_text(json.dumps(scored, indent=2) + "\n", encoding="utf-8")
    # Stable summaries only; no disk paths or source data.
    print("SYNTHETIC_BPM_OBSERVATION=PASS")
    for category, count in scored["counts"].items():
        print(f"SYNTHETIC_BPM_{category.upper()}={count}")
    print(f"SYNTHETIC_BPM_EVALUATED={scored['n']}")


if __name__ == "__main__":
    main()
