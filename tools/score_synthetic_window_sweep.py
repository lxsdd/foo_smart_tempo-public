#!/usr/bin/env python3
"""Score real foobar x64 BPM observations across public 120s window sweeps.

Exploratory quality evidence only: a synthetic pass is not owner-music
acceptance or proof of superiority over the previous production engine.
No private material, downloads, metadata/tag writes, or trained fixtures.
"""
from __future__ import annotations

import argparse
import csv
import math
import statistics
from pathlib import Path

REFERENCE = {
    "window_128_four_floor": (128.0, "drum_four_floor", "none"),
    "window_175_breakbeat": (175.0, "drum_breakbeat", "none"),
    "window_110_shuffle": (110.0, "drum_shuffle", "none"),
    "window_100_odd_five": (100.0, "drum_odd_five", "none"),
    "window_90_128_switch": (None, "tempo_switch", "tempo_nonstationary"),
    "window_nonperiodic_percussion": (None, "nonperiodic_percussion", "none"),
}
WINDOW_SECONDS = (5, 10, 15, 20, 30, 45, 60, 90)
PASS_COUNTS = (1, 3, 5, 10, 20, 50)
AXES = tuple((s, 5) for s in WINDOW_SECONDS) + tuple(
    (20, p) for p in PASS_COUNTS if p != 5
)
REQUIRED_COLUMNS = (
    "id", "style", "reference_bpm", "ambiguity", "seconds", "passes",
    "effective_seconds", "effective_passes", "bpm", "held", "analysis_ms",
)


def classify(bpm: float, held: int, expected: float | None) -> str:
    if expected is None:
        return "safe_hold" if held == 1 or bpm <= 0.0 else "false_accept"
    if held == 1:
        return "held"
    if bpm <= 0.0:
        return "missing"
    if abs(bpm - expected) <= 0.20:
        return "exact"
    ratio = bpm / expected
    if any(abs(ratio - alias) <= 0.02 * alias
           for alias in (0.25, 0.5, 2.0, 4.0)):
        return "tempo_alias"
    return "wrong"


def parse_rows(path: Path) -> dict[tuple[str, int, int], dict]:
    with path.open("r", newline="", encoding="utf-8-sig") as stream:
        reader = csv.DictReader(stream)
        if not reader.fieldnames or not all(
                field in reader.fieldnames for field in REQUIRED_COLUMNS):
            raise ValueError("Missing public real-host sweep fields")
        records = list(reader)
    expected_keys = {(name, seconds, passes)
                     for name in REFERENCE for seconds, passes in AXES}
    result: dict[tuple[str, int, int], dict] = {}
    for row in records:
        name = row["id"]
        seconds, passes = int(row["seconds"]), int(row["passes"])
        key = (name, seconds, passes)
        if key not in expected_keys or key in result:
            raise ValueError("Unknown or duplicate synthetic matrix cell")
        expected, style, ambiguity = REFERENCE[name]
        if row["style"] != style or row["ambiguity"] != ambiguity:
            raise ValueError("Synthetic reference style mismatch")
        if expected is None:
            if row["reference_bpm"].strip():
                raise ValueError("Nonstationary/nonrhythmic case has BPM reference")
        elif (not row["reference_bpm"].strip() or
              abs(float(row["reference_bpm"]) - expected) > 1e-6):
            raise ValueError("Synthetic BPM reference mismatch")
        bpm = float(row["bpm"])
        held = int(row["held"])
        effective_seconds = float(row["effective_seconds"])
        effective_passes = int(row["effective_passes"])
        milliseconds = float(row["analysis_ms"])
        if (not math.isfinite(bpm) or bpm < 0.0 or bpm > 1000.0 or
                held not in (0, 1) or
                not math.isfinite(effective_seconds) or
                not 0.0 < effective_seconds <= seconds + 1e-6 or
                not 0 < effective_passes <= passes or
                not math.isfinite(milliseconds) or milliseconds <= 0.0):
            raise ValueError("Invalid or non-real measured geometry/BPM/runtime")
        result[key] = {
            "status": classify(bpm, held, expected),
            "bpm": bpm, "held": held, "ms": milliseconds,
            "effective_seconds": effective_seconds,
            "effective_passes": effective_passes,
        }
    if set(result) != expected_keys:
        raise ValueError(f"Incomplete window sweep: {len(result)}/{len(expected_keys)}")
    return result


def summary(rows: dict[tuple[str, int, int], dict],
            seconds: int, passes: int) -> str:
    cells = [rows[(name, seconds, passes)] for name in REFERENCE]
    positives = cells[:4]
    negatives = cells[4:]
    exact = sum(x["status"] == "exact" for x in positives)
    wrong = sum(x["status"] in ("wrong", "tempo_alias") for x in positives)
    hold = sum(x["status"] in ("held", "missing") for x in positives)
    safe = sum(x["status"] == "safe_hold" for x in negatives)
    unsafe = sum(x["status"] == "false_accept" for x in negatives)
    median_ms = int(statistics.median(x["ms"] for x in cells))
    return (f"exact={exact}/4 wrong={wrong} held={hold} "
            f"safe_ambiguous={safe}/2 false_accept={unsafe} "
            f"median_analysis_ms={median_ms}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--observed", type=Path, required=True)
    args = parser.parse_args()
    try:
        rows = parse_rows(args.observed)
    except (OSError, TypeError, ValueError, OverflowError) as exc:
        parser.error(str(exc))
    print(f"SYNTHETIC_WINDOW_REAL_HOST_OBSERVATIONS={len(rows)}")
    for seconds in WINDOW_SECONDS:
        print(f"WINDOW_SWEEP_SECONDS seconds={seconds} passes=5 "
              + summary(rows, seconds, 5))
    for passes in PASS_COUNTS:
        print(f"WINDOW_SWEEP_PASSES seconds=20 passes={passes} "
              + summary(rows, 20, passes))
    print("WINDOW_SWEEP_DEFAULT_20S_50P "
          + summary(rows, 20, 50))
    print("WINDOW_SWEEP_EXPLORATORY_ONLY=TRUE")
    for name in REFERENCE:
        for seconds, passes in AXES:
            cell = rows[(name, seconds, passes)]
            print(f"WINDOW_SWEEP_CASE id={name} seconds={seconds} "
                  f"passes={passes} actual_seconds={cell['effective_seconds']:.3f} "
                  f"actual_passes={cell['effective_passes']} "
                  f"bpm={cell['bpm']:.5f} held={cell['held']} "
                  f"status={cell['status']} analysis_ms={cell['ms']:.0f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
