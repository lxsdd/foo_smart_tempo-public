#!/usr/bin/env python3
"""Deterministic public synthetic BPM corpus. No recordings, tags, or samples from users.

The exact musical beat period is encoded analytically in generated PCM.
Periodicity is only a ground truth for tests that have a clear metric pulse;
more complex/ambiguous rhythms should be scored separately, never relabeled
to make a failing analyzer pass.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import struct
from pathlib import Path

SAMPLE_RATE = 16000
DURATION_S = 32
# Keep current CI smoke lightweight: full-run BPM accuracy will be opt-in
# until the toolchain can measure the real component's numeric output.
CASES = (
    ("four_on_floor_120", 120.0, "straight", "unambiguous"),
    ("four_on_floor_137_5", 137.5, "straight", "unambiguous"),
    ("four_on_floor_174", 174.0, "straight", "unambiguous"),
    ("offbeat_hat_110", 110.0, "offbeat", "unambiguous"),
    ("half_time_snare_80", 80.0, "halftime", "pulse_family_ambiguous"),
    ("syncopated_128", 128.0, "syncopated", "pulse_family_ambiguous"),
)

def sample_at(index: int, beat_samples: float, pattern: str) -> float:
    # Calculate event time with double precision, never accumulating rounded
    # samples. The metrical bar is encoded in the primary quarter-note grid.
    beat_index = int(math.floor(index / beat_samples))
    sample_in_beat = index - round(beat_index * beat_samples)
    if sample_in_beat < 0:
        beat_index -= 1
        sample_in_beat = index - round(beat_index * beat_samples)
    # Strong kick once per quarter note; deterministic exponential decay.
    out = 0.0
    if sample_in_beat >= 0 and sample_in_beat < 1400:
        kick = (1.0 if beat_index % 4 == 0 else 0.65)
        out += kick * 0.60 * math.sin(2.0 * math.pi * 82.0 * sample_in_beat / SAMPLE_RATE) * math.exp(-sample_in_beat / 400.0)
    # Snare/hat accents deliberately vary but never use the reference BPM as
    # a target for the estimator.
    if pattern == "offbeat":
        hat_sample = index - round((beat_index + 0.5) * beat_samples)
        if 0 <= hat_sample < 340:
            out += 0.18 * math.sin(2.0 * math.pi * 3400.0 * hat_sample / SAMPLE_RATE) * math.exp(-hat_sample / 100.0)
    if pattern == "halftime" and beat_index % 4 == 2 and 0 <= sample_in_beat < 740:
        out += 0.20 * math.sin(2.0 * math.pi * 1400.0 * sample_in_beat / SAMPLE_RATE) * math.exp(-sample_in_beat / 180.0)
    if pattern == "syncopated" and beat_index % 4 in (1, 3):
        shift = int(round(beat_samples * 0.25))
        sync_sample = sample_in_beat - shift
        if 0 <= sync_sample < 400:
            out += 0.18 * math.sin(2.0 * math.pi * 750.0 * sync_sample / SAMPLE_RATE) * math.exp(-sync_sample / 130.0)
    return min(0.93, max(-0.93, out))

def make_wav(path: Path, bpm: float, pattern: str) -> dict:
    beat_samples = SAMPLE_RATE * 60.0 / bpm
    count = SAMPLE_RATE * DURATION_S
    data = bytearray(count * 2)
    for i in range(count):
        struct.pack_into("<h", data, i * 2, round(32767 * sample_at(i, beat_samples, pattern)))
    payload_size = len(data)
    header = (b"RIFF" + struct.pack("<I", 36 + payload_size) + b"WAVE"
              + b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, SAMPLE_RATE, SAMPLE_RATE * 2, 2, 16)
              + b"data" + struct.pack("<I", payload_size))
    path.write_bytes(header + data)
    return {"size_bytes": len(header) + payload_size,
            "sha256": hashlib.sha256(header + data).hexdigest()}

def generate(root: Path) -> list[dict]:
    if root.exists():
        raise FileExistsError("Refusing to overwrite an existing synthetic corpus")
    if root == root.parent:
        raise ValueError("Refusing a filesystem root as corpus output")
    root.mkdir(parents=True)
    rows = []
    for name, bpm, pattern, category in CASES:
        wav = root / (name + ".wav")
        digest = make_wav(wav, bpm, pattern)
        rows.append({"id": name, "filename": wav.name,
                     "reference_bpm": bpm, "beat_unit": "quarter_note",
                     "category": category, "pattern": pattern,
                     "duration_seconds": DURATION_S,
                     "sample_rate": SAMPLE_RATE, **digest})
    (root / "manifest.json").write_text(
        json.dumps({"schema": "smart_tempo_synthetic_corpus_v1", "cases": rows},
                   indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return rows

def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--output", required=True, type=Path)
    args = ap.parse_args()
    cases = generate(args.output.resolve())
    print("SYNTHETIC_TEMPO_CORPUS=PASS")
    print(f"SYNTHETIC_TEMPO_TRACKS={len(cases)}")

if __name__ == "__main__":
    main()
