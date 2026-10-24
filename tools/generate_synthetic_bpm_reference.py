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
    ("pulse_40", 40.0, "regular"),
    ("pulse_45", 45.0, "regular"),
    ("pulse_50", 50.0, "regular"),
    ("pulse_70", 70.0, "regular"),
    ("pulse_80", 80.0, "regular"),
    ("pulse_100", 100.0, "regular"),
    ("pulse_110", 110.0, "regular"),
    ("pulse_130", 130.0, "regular"),
    ("pulse_150", 150.0, "regular"),
    ("pulse_160", 160.0, "regular"),
    ("pulse_180", 180.0, "regular"),
    ("pulse_200", 200.0, "regular"),
    ("pulse_220", 220.0, "regular"),
    ("pulse_59_73", 59.73, "regular"),
    ("pulse_93_37", 93.37, "regular"),
    ("pulse_123_45", 123.45, "regular"),
    ("pulse_127_31", 127.31, "regular"),
    ("pulse_174_62", 174.62, "regular"),
    ("pulse_199_25", 199.25, "regular"),
    ("pulse_90_syncopated", 90.0, "syncopated"),
    ("pulse_175_syncopated", 175.0, "syncopated"),
    ("pulse_90_swing", 90.0, "swing"),
    ("pulse_175_swing", 175.0, "swing"),
    ("pulse_90_half_pulse", 90.0, "half_pulse"),
    ("pulse_175_half_pulse", 175.0, "half_pulse"),
    ("pulse_128_sparse", 128.0, "sparse"),
    ("pulse_175_sparse", 175.0, "sparse"),
    ("pulse_128_8s", 128.0, "regular"),
    ("pulse_128_12s", 128.0, "regular"),
    ("pulse_128_40s", 128.0, "regular"),
    ("pulse_128_44100_mono", 128.0, "regular"),
    ("pulse_128_48000_stereo", 128.0, "regular"),
    ("pulse_123_45_44100_stereo", 123.45, "regular"),
    ("single_click", None, "single_click"),
    ("steady_tone", None, "tone"),
    ("noise", None, "noise"),
    ("drum_60_four_floor", 60.0, "drum_four_floor"),
    ("drum_90_four_floor", 90.0, "drum_four_floor"),
    ("drum_110_four_floor", 110.0, "drum_four_floor"),
    ("drum_120_four_floor", 120.0, "drum_four_floor"),
    ("drum_128_four_floor", 128.0, "drum_four_floor"),
    ("drum_140_four_floor", 140.0, "drum_four_floor"),
    ("drum_175_four_floor", 175.0, "drum_four_floor"),
    ("drum_123_45_four_floor", 123.45, "drum_four_floor"),
    ("drum_90_breakbeat", 90.0, "drum_breakbeat"),
    ("drum_128_breakbeat", 128.0, "drum_breakbeat"),
    ("drum_175_breakbeat", 175.0, "drum_breakbeat"),
    ("drum_128_8s", 128.0, "drum_four_floor"),
    ("drum_128_44100_stereo", 128.0, "drum_four_floor"),
)

# Public-only fixture geometry. All remaining cases are 20s/16kHz/mono.
CASE_VARIANTS = {
    "pulse_128_8s": (8, 16000, 1),
    "pulse_128_12s": (12, 16000, 1),
    "pulse_128_40s": (40, 16000, 1),
    "pulse_128_44100_mono": (20, 44100, 1),
    "pulse_128_48000_stereo": (20, 48000, 2),
    "pulse_123_45_44100_stereo": (20, 44100, 2),
    "drum_128_8s": (8, 16000, 1),
    "drum_128_44100_stereo": (20, 44100, 2),
}


def event_times(bpm: float | None, style: str, duration: float) -> list[float]:
    if style == "single_click":
        return [0.25]
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



def mix_drum_voice(samples: bytearray, sample_rate: int, channels: int,
                   at_seconds: float, voice: str, gain: float, seed: int) -> None:
    """Add a deterministic synthesized percussion voice; no audio samples."""
    total_frames = len(samples) // (channels * 2)
    start = round(at_seconds * sample_rate)
    if start < 0 or start >= total_frames:
        return
    duration = (0.075 if voice == "kick" else
                0.055 if voice == "snare" else 0.020)
    count = min(round(sample_rate * duration), total_frames - start)
    state = (seed | 1) & 0xffffffff
    phase = 0.0
    for j in range(count):
        seconds = j / sample_rate
        if voice == "kick":
            # Smooth falling synthesized bass oscillator with a transient.
            freq = 48.0 + 120.0 * math.exp(-26.0 * seconds)
            phase += 2.0 * math.pi * freq / sample_rate
            sample = int(17000 * gain * math.exp(-42.0 * seconds)
                         * math.sin(phase))
        else:
            # Original deterministic noise, independently reseeded per hit.
            state ^= (state << 13) & 0xffffffff
            state ^= state >> 17
            state ^= (state << 5) & 0xffffffff
            whitened = (state & 0xffff) / 32767.5 - 1.0
            decay = 60.0 if voice == "snare" else 150.0
            amplitude = 8500 if voice == "snare" else 3200
            sample = int(amplitude * gain * math.exp(-decay * seconds)
                         * whitened)
        for ch in range(channels):
            offset = 2 * ((start + j) * channels + ch)
            earlier = struct.unpack_from("<h", samples, offset)[0]
            incoming = sample if ch == 0 else int(sample * 0.83)
            struct.pack_into("<h", samples, offset,
                             max(-32768, min(32767, earlier + incoming)))


def render_drum_beat_grid(samples: bytearray, bpm: float, style: str,
                          duration: float, sample_rate: int,
                          channels: int) -> list[int]:
    """Four-bar hierarchy: bass downbeat, backbeat snare, offbeat hi-hat."""
    period = 60.0 / bpm
    events = []
    beat = 0
    while True:
        tick = 0.25 + beat * period
        if tick >= duration - 0.025:
            break
        events.append(round(tick * sample_rate))
        beat_in_bar = beat % 4
        downbeat = beat_in_bar == 0
        if style == "drum_four_floor" or beat_in_bar in (0, 2):
            mix_drum_voice(samples, sample_rate, channels, tick, "kick",
                           1.17 if downbeat else 0.90, beat * 101 + 7)
        if beat_in_bar in (1, 3):
            mix_drum_voice(samples, sample_rate, channels, tick, "snare",
                           1.0, beat * 103 + 11)
        if style == "drum_breakbeat" and beat_in_bar == 2:
            ghost = tick + period / 2.0
            if ghost < duration - 0.025:
                mix_drum_voice(samples, sample_rate, channels, ghost, "kick",
                               0.48, beat * 107 + 13)
        mix_drum_voice(samples, sample_rate, channels, tick, "hat",
                       0.45 if downbeat else 0.32, beat * 109 + 17)
        offbeat = tick + period / 2.0
        if offbeat < duration - 0.025:
            mix_drum_voice(samples, sample_rate, channels, offbeat, "hat",
                           0.62, beat * 113 + 19)
        beat += 1
    return events


def render(name: str, bpm: float | None, style: str, target: Path) -> dict:
    duration, sample_rate, channels = CASE_VARIANTS.get(
        name, (DURATION_SECONDS, SAMPLE_RATE, 1))
    total = sample_rate * duration
    samples = bytearray(total * channels * 2)
    positions = []
    if style == "tone":
        # Constant energy without beats. A transient at startup alone does
        # not establish tempo; accepting any BPM here is unsafe.
        for frame in range(total):
            sample = int(6000 * math.sin(2 * math.pi * 220 * frame / sample_rate))
            for ch in range(channels):
                struct.pack_into("<h", samples, 2 * (frame * channels + ch), sample)
    elif style == "noise":
        # Deterministic white-ish xorshift noise: no private/copyright samples.
        state = 0x12345678
        for frame in range(total):
            state ^= (state << 13) & 0xffffffff
            state ^= state >> 17
            state ^= (state << 5) & 0xffffffff
            sample = int(state % 12001) - 6000
            for ch in range(channels):
                struct.pack_into("<h", samples, 2 * (frame * channels + ch), sample)
    elif style in ("drum_four_floor", "drum_breakbeat"):
        positions = render_drum_beat_grid(
            samples, bpm, style, duration, sample_rate, channels)
    else:
        click_samples = round(sample_rate * 0.0075)
        for t in event_times(bpm, style, duration):
            index = round(t * sample_rate)
            positions.append(index)
            for j in range(min(click_samples, total - index)):
                env = (click_samples - j) / click_samples
                magnitude = int(19000 * env *
                                math.sin(2 * math.pi * 910 * j / sample_rate))
                for ch in range(channels):
                    sample = magnitude if ch == 0 else int(magnitude * 0.85)
                    struct.pack_into("<h", samples,
                                     2 * ((index + j) * channels + ch), sample)
    with wave.open(str(target), "wb") as out:
        out.setnchannels(channels)
        out.setsampwidth(2)
        out.setframerate(sample_rate)
        out.writeframes(samples)
    return {
        "id": name, "reference_bpm": bpm, "style": style,
        "ambiguity": "meter_underdetermined"
                     if style in ("half_pulse", "sparse") else "none",
        "duration_sec": duration, "sample_rate": sample_rate,
        "channels": channels, "event_sample_positions": positions,
        "wav_sha256": hashlib.sha256(target.read_bytes()).hexdigest(),
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path)
    parser.add_argument("--case", choices=[c[0] for c in CASES],
                        help="Generate one public fixture for real-host smoke")
    parser.add_argument("--list-cases", action="store_true",
                        help="Emit only the fixed public fixture IDs")
    args = parser.parse_args()
    if args.list_cases:
        if args.output is not None or args.case is not None:
            parser.error("--list-cases cannot generate files")
        for name, _, _ in CASES:
            print(name)
        return
    if args.output is None:
        parser.error("--output is required for generation")
    dest = args.output.resolve()
    dest.mkdir(parents=True, exist_ok=True)
    if any(dest.iterdir()):
        parser.error("output directory must be empty (fail closed)")
    manifest = []
    cases = [c for c in CASES if args.case is None or c[0] == args.case]
    for name, bpm, style in cases:
        manifest.append(render(name, bpm, style, dest / (name + ".wav")))
    (dest / "reference.json").write_text(
        json.dumps({"schema": "synthetic_bpm_reference_v1", "cases": manifest},
                   indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"SYNTHETIC_REFERENCE_GENERATED={len(manifest)}")
    print("SYNTHETIC_REFERENCE_NO_OWNER_AUDIO=PASS")


if __name__ == "__main__":
    main()
