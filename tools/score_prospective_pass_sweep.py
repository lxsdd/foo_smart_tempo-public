#!/usr/bin/env python3
"""CI-only, predeclared pass-count sensitivity on public prospective WAVs.

The labels are used after host measurement only. This is a paired diagnostic,
not an optimizer, policy calibration, or real-recording release gate.
"""
from __future__ import annotations

import argparse
import csv
import math
from pathlib import Path

from generate_synthetic_bpm_reference import PROSPECTIVE_CASES
from score_prospective_bpm_reference import (
    PROFILE_HARMONIC_MARGIN, PROFILE_MIN_LOCAL, load_candidates, load_manifest,
)
from score_synthetic_bpm_reference import (
    evaluate, load_observed, shadow_candidate_choice,
)

TARGETS = (
    "prospective_four_floor_97_25",
    "prospective_breakbeat_96_70",
    "prospective_breakbeat_144_20",
    "prospective_breakbeat_189_60",
    "prospective_poly_three_143_60",
    "prospective_shuffle_83_80",
    "prospective_triplet_107_20",
    "prospective_half_pulse_153_60",
    "prospective_sparse_117_80",
    "prospective_tempo_switch_alt",
    "prospective_nonperiodic_alt",
)
PASSES = (1, 5, 20)
OBSERVED_FIELDS = ("id", "passes", "bpm", "held", "effective_seconds",
                   "effective_passes")
CANDIDATE_FIELDS = ("id", "passes", "rank", "bpm", "evidence", "support",
                    "winner", "local_exact")


def keys() -> set[tuple[str, int]]:
    return {(name, passes) for name in TARGETS for passes in PASSES}


def read_observed(path: Path) -> dict[tuple[str, int], dict]:
    out = {}
    with path.open("r", encoding="utf-8-sig", newline="") as f:
        reader = csv.DictReader(f)
        if tuple(reader.fieldnames or ()) != OBSERVED_FIELDS:
            raise ValueError("Invalid public pass-sweep observation schema")
        for row in reader:
            if set(row) != set(OBSERVED_FIELDS):
                raise ValueError("Invalid public pass-sweep observation fields")
            name, passes = row["id"], int(row["passes"])
            key = (name, passes)
            if key not in keys() or key in out:
                raise ValueError("Unknown or repeated public pass-sweep key")
            bpm = float(row["bpm"])
            seconds = float(row["effective_seconds"])
            actual_passes = int(row["effective_passes"])
            if (row["held"] not in ("0", "1") or
                    not all(map(math.isfinite, (bpm, seconds))) or
                    bpm < 0 or not 0 < seconds <= 20 or
                    not 0 < actual_passes <= passes or
                    (row["held"] == "0" and bpm <= 0)):
                raise ValueError("Invalid public pass-sweep host measurement")
            out[key] = {"bpm": bpm, "held": row["held"] == "1",
                        "effective_seconds": seconds,
                        "effective_passes": actual_passes}
    if set(out) != keys():
        raise ValueError("Incomplete public pass-sweep host matrix")
    return out


def read_candidates(path: Path) -> dict[tuple[str, int], list[dict]]:
    out = {key: [] for key in keys()}
    with path.open("r", encoding="utf-8-sig", newline="") as f:
        reader = csv.DictReader(f)
        if tuple(reader.fieldnames or ()) != CANDIDATE_FIELDS:
            raise ValueError("Invalid public pass-sweep candidate schema")
        for row in reader:
            if set(row) != set(CANDIDATE_FIELDS):
                raise ValueError("Invalid public pass-sweep candidate fields")
            key = (row["id"], int(row["passes"]))
            if key not in out or len(out[key]) >= 32:
                raise ValueError("Unknown or unbounded public pass-sweep candidate")
            rank = int(row["rank"])
            numbers = [float(row[field]) for field in
                       ("bpm", "evidence", "support", "winner", "local_exact")]
            if (not 1 <= rank <= 256 or
                    not all(map(math.isfinite, numbers)) or
                    numbers[0] <= 0 or numbers[2] < 0 or numbers[3] < 0):
                raise ValueError(
                    f"Invalid public pass-sweep numeric candidate id={key[0]} "
                    f"passes={key[1]} rank={rank}")
            # Evidence ranks may repeat after nearby hypotheses coalesce and
            # supplemental probes are appended. The row cap bounds the board.
            out[key].append(row)
    return out


def status(row: dict, value: float | None) -> str:
    if value is None or value <= 0:
        return "held"
    if row["ambiguous"]:
        return "ambiguous_accept"
    if row["reference_bpm"] is None:
        return "no_tempo_accept"
    return ("accurate" if abs(value - row["reference_bpm"]) <= 0.20 + 1e-9
            else "wrong")


def report(manifest: dict, baseline: list[dict],
           baseline_candidates: dict[str, list[dict]],
           observed: dict[tuple[str, int], dict],
           candidates: dict[tuple[str, int], list[dict]]) -> list[str]:
    expected = [name for name, _, _ in PROSPECTIVE_CASES]
    if ([row["id"] for row in manifest["cases"]] != expected or
            [row["id"] for row in baseline] != expected or
            set(observed) != keys() or set(candidates) != keys()):
        raise ValueError("Public pass-sweep identity mismatch")
    by_id = {row["id"]: row for row in baseline}
    lines = []
    for axis in (0,) + PASSES:
        counts = dict(clear_accurate=0, clear_wrong=0, clear_held=0,
                      clear_near=0, ambiguous_accept=0, no_tempo_accept=0)
        for name in TARGETS:
            row = by_id[name]
            if axis == 0:
                prod = None if row["held"] else row["reported_bpm"]
                board = baseline_candidates[name]
                geometry = "default"
            else:
                item = observed[(name, axis)]
                prod = None if item["held"] else item["bpm"]
                board = candidates[(name, axis)]
                geometry = (f"{item['effective_seconds']:.3f}s/"
                            f"{item['effective_passes']}p")
            shadow = shadow_candidate_choice(
                board, PROFILE_MIN_LOCAL, PROFILE_HARMONIC_MARGIN)
            near = (row["reference_bpm"] is not None and not row["ambiguous"] and
                    any(abs(float(c["bpm"]) - row["reference_bpm"]) <= .20 + 1e-9
                        for c in board))
            top = (sorted(board, key=lambda item: (
                -float(item["local_exact"]), int(item["rank"])))[0]
                   if board else None)
            closest_error = (min(abs(float(item["bpm"]) - row["reference_bpm"])
                                 for item in board)
                             if board and row["reference_bpm"] is not None and
                             not row["ambiguous"] else None)
            state = status(row, prod)
            if state == "accurate":
                counts["clear_accurate"] += 1
            elif state == "wrong":
                counts["clear_wrong"] += 1
            elif state == "held" and row["reference_bpm"] is not None and not row["ambiguous"]:
                counts["clear_held"] += 1
            elif state in ("ambiguous_accept", "no_tempo_accept"):
                counts[state] += 1
            counts["clear_near"] += bool(near)
            shown = lambda x: "held" if x is None or x <= 0 else f"{x:.6f}"
            top_bpm = f"{float(top['bpm']):.6f}" if top else "none"
            top_score = f"{float(top['local_exact']):.6f}" if top else "na"
            closest_text = (f"{closest_error:.6f}"
                            if closest_error is not None else "na")
            lines.append(
                f"PASS_SWEEP_CASE id={name} passes={axis if axis else 'default'} "
                f"geometry={geometry} production={shown(prod)} "
                f"shadow={shown(shadow)} near_candidate={int(bool(near))} "
                f"board_rows={len(board)} top_local_bpm={top_bpm} "
                f"top_local_score={top_score} "
                f"closest_error_bpm={closest_text} state={state}")
        lines.append(
            "PASS_SWEEP_SUMMARY passes=" + (str(axis) if axis else "default") +
            " " + " ".join(f"{k}={v}" for k, v in counts.items()))
    lines.append("PASS_SWEEP_REFERENCE_USED_AFTER_HOST_ONLY=TRUE")
    lines.append("PASS_SWEEP_PRODUCTION_UNCHANGED=TRUE")
    return lines


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--list-targets", action="store_true")
    parser.add_argument("--manifest", type=Path)
    parser.add_argument("--baseline-observed", type=Path)
    parser.add_argument("--baseline-candidates", type=Path)
    parser.add_argument("--observed", type=Path)
    parser.add_argument("--candidates", type=Path)
    args = parser.parse_args()
    if args.list_targets:
        if any((args.manifest, args.baseline_observed, args.baseline_candidates,
                args.observed, args.candidates)):
            parser.error("List mode accepts no file arguments")
        for name in TARGETS:
            print(name)
        return 0
    if not all((args.manifest, args.baseline_observed, args.baseline_candidates,
                args.observed, args.candidates)):
        parser.error("All five public synthetic inputs are required")
    try:
        manifest = load_manifest(args.manifest)
        baseline = evaluate(manifest, load_observed(args.baseline_observed))["results"]
        base_board = load_candidates(args.baseline_candidates)
        observations = read_observed(args.observed)
        boards = read_candidates(args.candidates)
        lines = report(manifest, baseline, base_board, observations, boards)
    except (OSError, ValueError, KeyError, TypeError, OverflowError) as exc:
        parser.error(str(exc))
    for line in lines:
        print(line)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
