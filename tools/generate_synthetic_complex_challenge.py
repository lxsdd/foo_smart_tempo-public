#!/usr/bin/env python3
"""Independent, synthetic-only complex-rhythm stress cohort.

The reference grid is declared before synthesis. It is never supplied to the
Smart Tempo host analyzer, only to the after-the-fact scorer. No recordings,
owner media, samples or personal data are loaded or committed.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import wave

from generate_synthetic_bpm_reference import (
    mix_drum_voice, render_nonperiodic_percussion, render_tempo_switch,
)

# Frozen independent challenge cohort. The pulse grid is precisely known, but
# intentional half/double traps are METER-AMBIGUOUS, not BPM correctness tests.
CASES = (
    ("challenge_73_25_cross32", 73.25, "cross32"),
    ("challenge_97_25_cross54", 97.25, "cross54"),
    ("challenge_129_37_clave", 129.37, "clave"),
    ("challenge_161_80_odd7", 161.80, "odd7"),
    ("challenge_189_60_halftrap", 189.60, "halftrap"),
    ("challenge_67_40_doubletrap", 67.40, "doubletrap"),
    ("challenge_143_60_ghostswing", 143.60, "ghostswing"),
    ("challenge_118_75_phasehats", 118.75, "phasehats"),
    ("challenge_103_30_sparsekick", 103.30, "sparsekick"),
    ("challenge_151_25_humanized", 151.25, "humanized"),
    ("challenge_84_60_masked", 84.60, "masked"),
    ("challenge_212_50_fast", 212.50, "fast"),
    ("challenge_57_35_slow", 57.35, "slow"),
    ("challenge_90_147_switch", None, "switch"),
    ("challenge_nonperiodic", None, "nonperiodic"),
    ("challenge_steady_tone", None, "tone"),
)
VARIANTS = {
    "challenge_212_50_fast": (8, 16000, 1),
    "challenge_57_35_slow": (40, 16000, 1),
    "challenge_129_37_clave": (20, 44100, 2),
    "challenge_118_75_phasehats": (20, 48000, 2),
}


def geometry(case_id: str) -> tuple[int, int, int]:
    return VARIANTS.get(case_id, (20, 16000, 1))


def render(case_id: str, bpm: float | None, style: str, output: Path) -> dict:
    duration, rate, channels = geometry(case_id)
    audio = bytearray(duration * rate * channels * 2)
    grid = []
    sections = []
    if style == "tone":
        for frame in range(duration * rate):
            sample = int(6000 * math.sin(2 * math.pi * 333 * frame / rate))
            for ch in range(channels):
                struct.pack_into("<h", audio, 2 * (frame * channels + ch), sample)
    elif style == "nonperiodic":
        render_nonperiodic_percussion(audio, duration, rate, channels,
                                      seed=0x70D9AC31)
    elif style == "switch":
        # Deliberately NO single whole-track BPM; segment tempos are known.
        grid = render_tempo_switch(audio, duration, rate, channels,
                                   alternate=True)
        sections = [{"start_sec": 0.25, "end_sec": duration / 2,
                     "bpm": 108.0},
                    {"start_sec": duration / 2 + 0.25,
                     "end_sec": float(duration), "bpm": 142.0}]
    else:
        assert bpm is not None and bpm > 0
        period = 60.0 / bpm
        # Only the intentionally masked fixture gets a quiet harmonic bed.
        if style == "masked":
            for frame in range(duration * rate):
                sec = frame / rate
                val = int(2800 * (math.sin(2 * math.pi * 187 * sec) +
                                  0.45 * math.sin(2 * math.pi * 311 * sec)))
                for ch in range(channels):
                    struct.pack_into("<h", audio,
                                     2 * (frame * channels + ch), val)
        beat = 0
        while True:
            ideal = 0.25 + beat * period
            if ideal >= duration - 0.10:
                break
            grid.append(round(ideal * rate))
            bar = beat % (7 if style == "odd7" else 4)
            # Humanized ACTUAL onset differs from the fixed underlying grid.
            shift = (((beat * 17) % 9) - 4) * 0.002 if style == "humanized" else 0
            tick = ideal + shift
            strong = bar == 0
            kick = (bar in (0, 2) if style == "sparsekick" else
                    beat % 2 == 0 if style == "halftrap" else True)
            if kick:
                mix_drum_voice(audio, rate, channels, tick, "kick",
                               1.28 if strong else 0.73,
                               beat * 101 + 17)
            snare = (bar == 2 if style == "halftrap" else
                     bar in ((2, 4) if style == "odd7" else (1, 3)))
            if snare:
                mix_drum_voice(audio, rate, channels, tick, "snare",
                               0.82, beat * 103 + 23)
            # The hats preserve subdivisions in sparse-kick and halftrap cases,
            # but the underlying perceptual metric level is not guaranteed.
            mix_drum_voice(audio, rate, channels, tick, "hat",
                           0.5 if strong else 0.24, beat * 107 + 29)
            offsets = ((1/3, 2/3) if style == "ghostswing" else
                       (1/4, 1/2, 3/4) if style == "fast" else
                       (0.36, 0.86) if style == "phasehats" and beat % 2 else
                       (1/2,))
            for off_index, fraction in enumerate(offsets):
                t = tick + period * fraction
                if t < duration - 0.05:
                    mix_drum_voice(audio, rate, channels, t, "hat",
                                   0.33, beat * 109 + off_index + 31)
            if style == "ghostswing" and bar in (1, 3):
                t = tick + period * (2/3)
                if t < duration - 0.05:
                    mix_drum_voice(audio, rate, channels, t, "snare",
                                   0.18, beat * 113 + 37)
            if style in ("cross32", "cross54") and beat % 4 == 0:
                # Competing 3-in-2 or 5-in-4 metrical stream; downbeat snare
                # accents retain the declared quarter-note grid.
                groups, span = (3, 2) if style == "cross32" else (5, 4)
                for j in range(1, groups):
                    t = tick + period * span * j / groups
                    if t < duration - 0.05:
                        mix_drum_voice(audio, rate, channels, t, "snare",
                                       0.41, beat * 127 + j + 43)
            if style == "clave" and bar == 0:
                for fraction in (0.0, 1.5, 2.5, 3.5):
                    t = tick + period * fraction
                    if t < duration - 0.05:
                        mix_drum_voice(audio, rate, channels, t, "snare",
                                       0.35, beat * 131 + int(fraction * 10))
            if style == "doubletrap" and beat % 2 == 0:
                t = tick + period / 2
                if t < duration - 0.05:
                    mix_drum_voice(audio, rate, channels, t, "kick",
                                   0.82, beat * 139 + 47)
            beat += 1
    with wave.open(str(output), "wb") as writer:
        writer.setnchannels(channels)
        writer.setsampwidth(2)
        writer.setframerate(rate)
        writer.writeframes(audio)
    return {
        "id": case_id, "style": "stress_" + style,
        "reference_bpm": bpm,
        "ambiguity": ("meter_underdetermined"
                      if style in ("halftrap", "doubletrap") else
                      "tempo_nonstationary" if style == "switch" else "none"),
        "duration_sec": duration, "sample_rate": rate, "channels": channels,
        "event_sample_positions": grid, "tempo_segments": sections,
        "wav_sha256": hashlib.sha256(output.read_bytes()).hexdigest(),
    }


def main() -> None:
    p = argparse.ArgumentParser()
    p.add_argument("--list-cases", action="store_true")
    p.add_argument("--case", choices=[row[0] for row in CASES])
    p.add_argument("--output", type=Path)
    args = p.parse_args()
    if args.list_cases:
        if args.output is not None or args.case is not None:
            p.error("listing must not generate files")
        for name, _, _ in CASES:
            print(name)
        return
    if args.output is None:
        p.error("--output is required")
    selected = [row for row in CASES
                if args.case is None or row[0] == args.case]
    dest = args.output.resolve()
    dest.mkdir(parents=True, exist_ok=True)
    if any(dest.iterdir()):
        p.error("output directory must be empty")
    manifest = [render(name, bpm, style, dest / (name + ".wav"))
                for name, bpm, style in selected]
    (dest / "reference.json").write_text(
        json.dumps({"schema": "synthetic_bpm_reference_v1", "cases": manifest},
                   indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"SYNTHETIC_CHALLENGE_GENERATED={len(manifest)}")


if __name__ == "__main__":
    main()
