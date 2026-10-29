#!/usr/bin/env python3
"""Deterministic public nonstationary WAV for original-host RAW selector coverage.

Four pulse families, no single-BPM oracle, private audio or network imports.
"""
from __future__ import annotations
import argparse
import math
from pathlib import Path
import struct
import wave

RATE = 16000
SECTION_SECONDS = 45
TEMPOS = (103.0, 127.0, 154.0, 181.0)
NAME = "raw_route_multitempo.wav"


def generate(path: Path) -> tuple[int, tuple[int, ...]]:
    total = RATE * SECTION_SECONDS * len(TEMPOS)
    samples = bytearray(total * 2)
    positions = []
    click_samples = 120
    for section, bpm in enumerate(TEMPOS):
        begin = section * SECTION_SECONDS
        end = begin + SECTION_SECONDS
        t = begin + 0.25
        while t < end - 0.025:
            pos = round(t * RATE)
            positions.append(pos)
            for j in range(min(click_samples, total - pos)):
                envelope = (click_samples - j) / click_samples
                value = int(19000 * envelope *
                            math.sin(2 * math.pi * 910 * j / RATE))
                struct.pack_into("<h", samples, (pos + j) * 2, value)
            t += 60.0 / bpm
    with wave.open(str(path), "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(RATE)
        wav.writeframes(samples)
    return total, tuple(positions)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    if any(output.iterdir()):
        parser.error("output directory must be empty")
    total, positions = generate(output / NAME)
    if not positions or total != 2880000:
        parser.error("synthetic pulse generation failed")
    print("SMART_TEMPO_RAW_ROUTE_SYNTHETIC_GENERATED=1")
    print("SMART_TEMPO_RAW_ROUTE_NONSTATIONARY_NO_SINGLE_BPM=PASS")


if __name__ == "__main__":
    main()
