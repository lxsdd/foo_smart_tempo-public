#!/usr/bin/env python3
"""Synthetic-only fixed meter/beat-unit controls. Never used to select BPM."""
from __future__ import annotations
import argparse
from fractions import Fraction
import hashlib
import json
from pathlib import Path
import struct
import wave

RATE, DURATION, START = 16000, 20, Fraction(1, 4)
# Name, musical BPM (None means undetermined), meter, beat unit, eighth-note period
CASES = (
    ("meter_3_4_120_quarter", 120, "3/4", "quarter", Fraction(1, 4)),
    ("meter_6_8_80_dotted", 80, "6/8", "dotted_quarter", Fraction(1, 4)),
    ("meter_3_4_150_quarter", 150, "3/4", "quarter", Fraction(1, 5)),
    ("meter_6_8_100_dotted", 100, "6/8", "dotted_quarter", Fraction(1, 5)),
    ("meter_ambiguous_120q_80dq", None, "underdetermined", "underdetermined", Fraction(1, 4)),
)


def sample_positions(interval: Fraction) -> list[int]:
    positions = []
    i = 0
    while True:
        t = START + i * interval
        if t >= DURATION:
            return positions
        sample = t * RATE
        if sample.denominator != 1:
            raise ValueError("sample-grid alignment unexpected")
        positions.append(int(sample))
        i += 1


def synth(case: tuple, target: Path) -> dict:
    name, bpm, meter, unit, subdivision_period = case
    positions = sample_positions(subdivision_period)
    beat_stride = 2 if meter == "3/4" else 3
    beats = positions[::beat_stride] if bpm is not None else []
    bars = positions[::6]
    pcm = bytearray(RATE * DURATION * 2)
    for i, sample in enumerate(positions):
        if meter == "3/4":
            peak = 11500 if i % 6 == 0 else 6500 if i % 2 == 0 else 2100
        elif meter == "6/8":
            peak = 11500 if i % 6 == 0 else 7900 if i % 6 == 3 else 2100
        else:
            peak = 4200
        # Fully deterministic 80-sample triangle, only synthetic PCM; no PRNG.
        for j in range(80):
            value = (peak * (80 - j)) // 80
            struct.pack_into("<h", pcm, (sample + j) * 2, value)
    with wave.open(str(target), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(pcm)
    return dict(id=name, reference_bpm=bpm, meter=meter, beat_unit=unit,
                ambiguity="meter_underdetermined" if bpm is None else "none",
                bar_sample_positions=bars, beat_sample_positions=beats,
                subdivision_sample_positions=positions,
                sample_rate=RATE, duration_sec=DURATION,
                generator_revision="meter_grid_v2_fractional_samples_no_prng",
                corpus_partition="separate_synthetic_meter_challenge_v2",
                wav_sha256=hashlib.sha256(target.read_bytes()).hexdigest())


def main() -> None:
    p = argparse.ArgumentParser()
    p.add_argument("--output", type=Path)
    p.add_argument("--list-cases", action="store_true")
    p.add_argument("--case", choices=[x[0] for x in CASES])
    args = p.parse_args()
    if args.list_cases:
        print("\n".join(x[0] for x in CASES))
        return
    if args.output is None:
        p.error("--output required")
    path = args.output.resolve()
    path.mkdir(parents=True, exist_ok=True)
    if any(path.iterdir()):
        p.error("output must be empty")
    chosen = [x for x in CASES if args.case is None or x[0] == args.case]
    rows = [synth(x, path / (x[0] + ".wav")) for x in chosen]
    (path / "reference.json").write_text(json.dumps({
        "schema": "synthetic_meter_challenge_v2", "cases": rows
    }, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"SYNTHETIC_METER_CASES={len(rows)}")


if __name__ == "__main__":
    main()
