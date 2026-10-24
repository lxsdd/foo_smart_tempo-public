#!/usr/bin/env python3
"""Read-only paired real-host early/late BPM diagnostic on six public 120s WAVs."""
from __future__ import annotations

import argparse
import csv
import math
from pathlib import Path

# The phase references are fixed by the generator, not inferred by MIR. A
# nonstationary song has no single global BPM even if both parts are precise.
PHASE_REFERENCES = {
    "window_128_four_floor": (128.0, 128.0, "drum_four_floor", "none"),
    "window_175_breakbeat": (175.0, 175.0, "drum_breakbeat", "none"),
    "window_110_shuffle": (110.0, 110.0, "drum_shuffle", "none"),
    "window_100_odd_five": (100.0, 100.0, "drum_odd_five", "none"),
    "window_90_128_switch": (90.0, 128.0, "tempo_switch", "tempo_nonstationary"),
    "window_nonperiodic_percussion": (None, None, "nonperiodic_percussion", "none"),
}
FIELDS = (
    "id", "style", "reference_bpm", "ambiguity", "phase", "offset_pct",
    "seconds", "passes", "effective_seconds", "effective_passes",
    "bpm", "held", "analysis_ms",
)


def parse(path: Path) -> dict[tuple[str, str], dict]:
    with path.open("r", encoding="utf-8-sig", newline="") as file:
        reader = csv.DictReader(file)
        if not reader.fieldnames or not all(x in reader.fieldnames for x in FIELDS):
            raise ValueError("Invalid public temporal observation schema")
        raw = list(reader)
    out = {}
    for row in raw:
        name = row["id"]
        phase = row["phase"]
        key = name, phase
        if name not in PHASE_REFERENCES or phase not in ("early", "late") or key in out:
            raise ValueError("Unknown or duplicate public temporal pair")
        early, late, style, ambiguity = PHASE_REFERENCES[name]
        if row["style"] != style or row["ambiguity"] != ambiguity:
            raise ValueError("Temporal public reference identity mismatch")
        expected_global = early if early == late else None
        if expected_global is None:
            if row["reference_bpm"].strip():
                raise ValueError("Nonstationary/nonrhythmic fixture has no global BPM")
        elif not row["reference_bpm"].strip() or abs(
                float(row["reference_bpm"]) - expected_global) > 1e-6:
            raise ValueError("Fixed global BPM reference mismatch")
        offset = 20 if phase == "early" else 80
        bpm = float(row["bpm"])
        held = int(row["held"])
        seconds = int(row["seconds"])
        passes = int(row["passes"])
        effective_seconds = float(row["effective_seconds"])
        effective_passes = int(row["effective_passes"])
        milliseconds = float(row["analysis_ms"])
        if (int(row["offset_pct"]) != offset or seconds != 20 or passes != 1 or
                not math.isfinite(bpm) or not 0 <= bpm <= 1000 or
                held not in (0, 1) or
                not math.isfinite(effective_seconds) or
                not 0 < effective_seconds <= 20.000001 or
                effective_passes != 1 or not math.isfinite(milliseconds) or
                milliseconds <= 0):
            raise ValueError("Measured host phase/geometry invalid")
        out[key] = {"bpm": bpm, "held": held, "expected": early if phase == "early" else late,
                    "elapsed_ms": milliseconds}
    want = {(name, phase) for name in PHASE_REFERENCES for phase in ("early", "late")}
    if set(out) != want:
        raise ValueError(f"Incomplete real-host temporal sweep: {len(out)}/12")
    return out


CANDIDATE_FIELDS = (
    "id", "phase", "rank", "bpm", "evidence", "support", "winner",
    "local_exact",
)


def parse_candidates(path: Path) -> dict[tuple[str, str], list[dict]]:
    """Load bounded actual-host candidates; never consult expected BPM."""
    keys = {(name, phase) for name in PHASE_REFERENCES
            for phase in ("early", "late")}
    out: dict[tuple[str, str], list[dict]] = {key: [] for key in keys}
    with path.open("r", encoding="utf-8-sig", newline="") as f:
        reader = csv.DictReader(f)
        if tuple(reader.fieldnames or ()) != CANDIDATE_FIELDS:
            raise ValueError("Invalid exact temporal candidate schema")
        for raw in reader:
            if set(raw) != set(CANDIDATE_FIELDS):
                raise ValueError("Unexpected temporal candidate field")
            key = (raw["id"], raw["phase"])
            if key not in keys:
                raise ValueError("Candidate references an unknown phase/fixture")
            rank = int(raw["rank"])
            bpm, evidence, support, winner, local = (
                float(raw[k]) for k in
                ("bpm", "evidence", "support", "winner", "local_exact")
            )
            if (not 1 <= rank <= 32 or
                    not all(math.isfinite(x) for x in
                            (bpm, evidence, support, winner, local)) or
                    bpm <= 0 or support < 0 or winner < 0 or
                    len(out[key]) >= 32):
                raise ValueError("Invalid or unbounded real-host candidate")
            out[key].append({"rank": rank, "bpm": bpm, "local": local})
    return out


def status(row: dict) -> str:
    if row["held"] == 1 or row["bpm"] == 0:
        return "safe_hold" if row["expected"] is None else "held"
    if row["expected"] is None:
        return "unsafe_accept"
    if abs(row["bpm"] - row["expected"]) <= 0.20:
        return "exact"
    return "wrong_or_alias"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--observed", type=Path, required=True)
    parser.add_argument("--candidates", type=Path, required=True)
    args = parser.parse_args()
    try:
        rows = parse(args.observed)
        candidates = parse_candidates(args.candidates)
    except (OSError, ValueError, OverflowError, TypeError) as exc:
        parser.error(str(exc))
    print("TEMPORAL_REAL_HOST_OBSERVATIONS=12")
    for name, (e, l, _, _) in PHASE_REFERENCES.items():
        early, late = rows[name, "early"], rows[name, "late"]
        for phase, row in (("early", early), ("late", late)):
            reference = "none" if row["expected"] is None else f"{row['expected']:.3f}"
            print(f"TEMPORAL_PHASE id={name} phase={phase} expected={reference} "
                  f"bpm={row['bpm']:.5f} held={row['held']} "
                  f"category={status(row)} elapsed_ms={row['elapsed_ms']:.0f}")
        both = (not early["held"] and not late["held"] and
                early["bpm"] > 0 and late["bpm"] > 0)
        delta = abs(early["bpm"] - late["bpm"]) if both else 0.0
        matching = bool(both and delta <= 0.20)
        alias = bool(both and (abs(early["bpm"] / late["bpm"] - .5) <= .02 or
                               abs(early["bpm"] / late["bpm"] - 2.) <= .04))
        print(f"TEMPORAL_PAIR id={name} consistent_exact={int(matching)} "
              f"alias_conflict={int(alias)} different_phases={int(both and not matching)} "
              f"global_reference={'none' if e != l else e}")
    # References only appear in this offline scorer; the analyzer and
    # candidate inventory were already measured without them.
    correct_measured = 0
    wrong_with_measured = 0
    accepted_with_witness = 0
    for name, (early_ref, late_ref, _, _) in PHASE_REFERENCES.items():
        for phase, expected in (("early", early_ref), ("late", late_ref)):
            actual = rows[name, phase]
            inventory = candidates[name, phase]
            exact = [c for c in inventory if expected is not None and
                     abs(c["bpm"] - expected) <= 0.20]
            witness = (actual["bpm"] > 0 and not actual["held"] and
                       any(abs(c["bpm"] - actual["bpm"]) <= 0.20
                           for c in inventory))
            correct_measured += int(bool(exact))
            accepted_with_witness += int(bool(witness))
            wrong_with_measured += int(bool(exact) and
                                       status(actual) == "wrong_or_alias")
            best_rank = min((c["rank"] for c in exact), default=0)
            print(f"TEMPORAL_CANDIDATE_EVIDENCE id={name} phase={phase} "
                  f"candidates={len(inventory)} "
                  f"reference_candidate={int(bool(exact))} "
                  f"best_reference_rank={best_rank} "
                  f"output_witness={int(bool(witness))} "
                  f"status={status(actual)}")
    print(f"TEMPORAL_PHASE_CORRECT_CANDIDATE_PRESENT={correct_measured}/10")
    print(f"TEMPORAL_PHASE_WRONG_OUTPUT_WITH_CORRECT_CANDIDATE={wrong_with_measured}")
    print(f"TEMPORAL_PHASE_OUTPUT_WITNESSED={accepted_with_witness}/12")
    print("TEMPORAL_CANDIDATE_EVIDENCE_DIAGNOSTIC_ONLY=TRUE")
    print("TEMPORAL_STATIONARITY_DIAGNOSTIC_ONLY=TRUE")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
