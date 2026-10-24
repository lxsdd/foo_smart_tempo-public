#!/usr/bin/env python3
"""Generate deterministic, synthetic-only reference BPM WAVs for CI.
No owner songs, samples, audio imports, metadata, network or proprietary corpora.
The exact timing oracle is determined *before* synthesis, never inferred by DSP.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import wave

SAMPLE_RATE = 16000
DURATION_SECONDS = 20
# Intentionally not only round-number BPMs; distinct tempo families / aliases.
CASES = (
    ("pulse_60", 60.0, "regular"),
    ("pulse_90", 90.0, "regular"),
    ("pulse_120", 120.0, "regular"),
    ("pulse_128", 128.0, "regular"),
    ("pulse_140", 140.0, "regular"),
    ("pulse_175", 175.0, "regular"),
    ("pulse_128_syncopated", 128.0, "syncopated"),
    ("pulse_120_swing", 120.0, "swing"),
    ("pulse_128_half_pulse", 128.0, "half_pulse"),
    ("pulse_120_sparse", 120.0, "sparse"),
    ("silence", None, "silence"),
)


def event_times(bpm: float | None, style: str, duration: float) -> list[float]:
    if bpm is None:
        return []
    period = 60.0 / bpm
    times = []
    beat = 0
    while True:
        if style == "sparse" and beat % 4 != 0:
            beat += 1
            continue
        if style == "half_pulse" and beat % 2:
            beat += 1
            continue
        t = 0.25 + beat * period
        if style == "swing" and beat % 2:
            t += period / 6
        if t >= duration - 0.025:
            break
        times.append(t)
        if style == "syncopated" and beat % 2 == 0:
            offbeat = t + period * 0.75
            if offbeat < duration - 0.025:
                times.append(offbeat)
        beat += 1
    return sorted(times)


def render(name: str, bpm: float | None, style: str, target: Path) -> dict:
    total = int(SAMPLE_RATE * DURATION_SECONDS)
    samples = bytearray(total * 2)
    click_samples = 120  # 7.5 ms tone, distinct from a zero-width impulse
    positions = []
    for t in event_times(bpm, style, DURATION_SECONDS):
        index = round(t * SAMPLE_RATE)
        positions.append(index)
        for j in range(min(click_samples, total - index)):
            env = (click_samples - j) / click_samples
            magnitude = int(19000 * env * math.sin(2 * math.pi * 910 * j / SAMPLE_RATE))
            struct.pack_into("<h", samples, (index + j) * 2, magnitude)
    with wave.open(str(target), "wb") as out:
        out.setnchannels(1)
        out.setsampwidth(2)
        out.setframerate(SAMPLE_RATE)
        out.writeframes(samples)
    return {
        "id": name, "reference_bpm": bpm, "style": style,
        "duration_sec": DURATION_SECONDS, "sample_rate": SAMPLE_RATE,
        "event_sample_positions": positions,
        "wav_sha256": hashlib.sha256(target.read_bytes()).hexdigest(),
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    dest = args.output.resolve()
    dest.mkdir(parents=True, exist_ok=True)
    if any(dest.iterdir()):
        parser.error("output directory must be empty (fail closed)")
    manifest = []
    for name, bpm, style in CASES:
        manifest.append(render(name, bpm, style, dest / (name + ".wav")))
    (dest / "reference.json").write_text(
        json.dumps({"schema": "synthetic_bpm_reference_v1", "cases": manifest},
                   indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"SYNTHETIC_REFERENCE_GENERATED={len(manifest)}")
    print("SYNTHETIC_REFERENCE_NO_OWNER_AUDIO=PASS")


if __name__ == "__main__":
    main()
