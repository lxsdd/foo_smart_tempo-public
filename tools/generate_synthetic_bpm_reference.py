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
    # Independent rhythm-family challenge cohort; no imported audio samples.
    ("drum_75_shuffle", 75.0, "drum_shuffle"),
    ("drum_110_shuffle", 110.0, "drum_shuffle"),
    ("drum_160_shuffle", 160.0, "drum_shuffle"),
    ("drum_95_triplet", 95.0, "drum_triplet"),
    ("drum_125_triplet", 125.0, "drum_triplet"),
    ("drum_185_triplet", 185.0, "drum_triplet"),
    ("drum_90_half_time", 90.0, "drum_half_time"),
    ("drum_150_half_time", 150.0, "drum_half_time"),
    ("drum_100_odd_five", 100.0, "drum_odd_five"),
    ("drum_142_odd_five", 142.0, "drum_odd_five"),
    ("drum_105_dropouts", 105.0, "drum_dropouts"),
    ("drum_170_dropouts", 170.0, "drum_dropouts"),
    ("drum_124_dense_hats", 124.0, "drum_dense_hats"),
    ("drum_174_dense_hats", 174.0, "drum_dense_hats"),
    ("drum_112_poly_three", 112.0, "drum_poly_three"),
    ("drum_176_poly_three", 176.0, "drum_poly_three"),
    ("drum_88_ghost_notes", 88.0, "drum_ghost_notes"),
    ("drum_137_ghost_notes", 137.0, "drum_ghost_notes"),
    ("nonperiodic_percussion", None, "nonperiodic_percussion"),
    ("tempo_90_128_switch", None, "tempo_switch"),
)

# Separate 120-second, non-owner public fixtures for real foobar host
# window/pass sweeps. Deliberately excluded from the fixed 80-case baseline.
WINDOW_CASES = (
    ("window_128_four_floor", 128.0, "drum_four_floor"),
    ("window_175_breakbeat", 175.0, "drum_breakbeat"),
    ("window_110_shuffle", 110.0, "drum_shuffle"),
    ("window_100_odd_five", 100.0, "drum_odd_five"),
    ("window_90_128_switch", None, "tempo_switch"),
    ("window_nonperiodic_percussion", None, "nonperiodic_percussion"),
)

# Prospective parameter holdout: fixed before the new real-host run. These
# tempos/styles are not used in the 80-case profile exploration. They are
# still synthetic and do not substitute for human-labeled owner acceptance.
PROSPECTIVE_CASES = (
    ("prospective_four_floor_97_25", 97.25, "drum_four_floor"),
    ("prospective_four_floor_133_70", 133.70, "drum_four_floor"),
    ("prospective_four_floor_181_30", 181.30, "drum_four_floor"),
    ("prospective_breakbeat_96_70", 96.70, "drum_breakbeat"),
    ("prospective_breakbeat_144_20", 144.20, "drum_breakbeat"),
    ("prospective_breakbeat_189_60", 189.60, "drum_breakbeat"),
    ("prospective_shuffle_83_80", 83.80, "drum_shuffle"),
    ("prospective_shuffle_134_60", 134.60, "drum_shuffle"),
    ("prospective_triplet_107_20", 107.20, "drum_triplet"),
    ("prospective_triplet_156_40", 156.40, "drum_triplet"),
    ("prospective_odd_five_117_30", 117.30, "drum_odd_five"),
    ("prospective_odd_five_149_20", 149.20, "drum_odd_five"),
    ("prospective_poly_three_143_60", 143.60, "drum_poly_three"),
    ("prospective_dense_hats_118_40", 118.40, "drum_dense_hats"),
    ("prospective_half_pulse_153_60", 153.60, "half_pulse"),
    ("prospective_sparse_117_80", 117.80, "sparse"),
    ("prospective_tempo_switch_alt", None, "tempo_switch_alt"),
    ("prospective_nonperiodic_alt", None, "nonperiodic_percussion_alt"),
    ("prospective_tone_alt", None, "tone_alt"),
    ("prospective_noise_alt", None, "noise_alt"),
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
    **{name: (120, SAMPLE_RATE, 1) for name, _, _ in WINDOW_CASES},
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
        beats_per_bar = 5 if style == "drum_odd_five" else 4
        beat_in_bar = beat % beats_per_bar
        downbeat = beat_in_bar == 0
        # Fixed musical tempo; accents, instrumentation and phrasing vary.
        drop_bar = style == "drum_dropouts" and (beat // 4) % 4 == 2
        kick = style != "drum_breakbeat" or beat_in_bar in (0, 2)
        if not drop_bar and kick:
            mix_drum_voice(samples, sample_rate, channels, tick, "kick",
                           1.17 if downbeat else (0.38 if style == "drum_ghost_notes" else 0.90),
                           beat * 101 + 7)
        snare_beat = (beat_in_bar == 2 if style == "drum_half_time"
                      else beat_in_bar in (1, 3))
        if not drop_bar and snare_beat:
            mix_drum_voice(samples, sample_rate, channels, tick, "snare",
                           1.0, beat * 103 + 11)
        if style in ("drum_breakbeat", "drum_ghost_notes") and beat_in_bar == 2:
            ghost = tick + period / 2.0
            if ghost < duration - 0.025:
                mix_drum_voice(samples, sample_rate, channels, ghost, "kick",
                               0.35 if style == "drum_ghost_notes" else 0.48,
                               beat * 107 + 13)
        if not drop_bar:
            mix_drum_voice(samples, sample_rate, channels, tick, "hat",
                           0.45 if downbeat else 0.32, beat * 109 + 17)
            offsets = (
                (1.0 / 3, 2.0 / 3) if style in ("drum_triplet", "drum_poly_three") else
                (0.25, 0.5, 0.75) if style == "drum_dense_hats" else
                (2.0 / 3,) if style == "drum_shuffle" else (0.5,)
            )
            for offset_index, fraction in enumerate(offsets):
                offbeat = tick + period * fraction
                if offbeat < duration - 0.025:
                    gain = 0.35 if style == "drum_dense_hats" else 0.62
                    mix_drum_voice(samples, sample_rate, channels, offbeat,
                                   "hat", gain, beat * 113 + 19 + offset_index)
            if style == "drum_poly_three" and beat % 3 == 0:
                poly_time = tick + period * 1.5
                if poly_time < duration - 0.025:
                    mix_drum_voice(samples, sample_rate, channels, poly_time,
                                   "snare", 0.24, beat * 127 + 31)
        beat += 1
    return events



def render_nonperiodic_percussion(samples: bytearray, duration: float,
                                   sample_rate: int, channels: int,
                                   seed: int = 0x64136F23) -> None:
    """Nonstationary, seeded impacts with no stable event inter-onset period."""
    state = seed
    t = 0.19
    count = 0
    while t < duration - 0.15:
        state ^= (state << 13) & 0xffffffff
        state ^= state >> 17
        state ^= (state << 5) & 0xffffffff
        fraction = (state & 0xffff) / 65535.0
        t += 0.13 + 1.54 * fraction
        if t < duration - 0.15:
            mix_drum_voice(samples, sample_rate, channels, t,
                           "kick" if count % 3 == 0 else "snare",
                           0.65, count * 157 + 41)
        count += 1


def render_tempo_switch(samples: bytearray, duration: float,
                        sample_rate: int, channels: int,
                        alternate: bool = False) -> list[int]:
    """Two disjoint BPM halves: no single whole-track tempo."""
    positions = []
    early, late = (108.0, 142.0) if alternate else (90.0, 128.0)
    for start, end, bpm in ((0.25, duration / 2, early),
                            (duration / 2 + 0.25, duration, late)):
        index = 0
        while True:
            t = start + index * 60.0 / bpm
            if t >= end - 0.025:
                break
            positions.append(round(t * sample_rate))
            mix_drum_voice(samples, sample_rate, channels, t, "kick",
                           0.98, index * 103 + int(bpm))
            if index % 2:
                mix_drum_voice(samples, sample_rate, channels, t, "snare",
                               0.76, index * 107 + int(bpm))
            index += 1
    return positions



def oracle_beat_grid(bpm: float | None, style: str, duration: int,
                     sample_rate: int) -> list[int]:
    """Latent quarter-note grid, specified before synthesis (not DSP output).

    Silent implied beats remain present for sparse/half-pulse/dropout styles.
    Swing displaces some *sounded clicks*, not the underlying quarter grid.
    Nonstationary and no-tempo cases intentionally have no global beat grid.
    """
    if bpm is None:
        return []
    count = 0
    positions = []
    while True:
        sample = round((0.25 + count * 60.0 / bpm) * sample_rate)
        if sample >= round((duration - 0.025) * sample_rate):
            break
        positions.append(sample)
        count += 1
    return positions


def oracle_tempo_segments(style: str, duration: int,
                          sample_rate: int) -> list[dict]:
    """Explicit local BPMs for a switch; never fabricate one global BPM."""
    if not style.startswith("tempo_switch"):
        return []
    early, late = ((108.0, 142.0) if style.endswith("_alt")
                   else (90.0, 128.0))
    return [
        {"start_sample": round(start * sample_rate),
         "end_sample": round(end * sample_rate), "bpm": bpm}
        for start, end, bpm in (
            (0.25, duration / 2, early),
            (duration / 2 + 0.25, duration, late),
        )
    ]


def fixture_seed(style: str) -> int | None:
    """Record fixed initial PRNG seeds; drum hits use indexed voice seeds."""
    return {
        "noise": 0x12345678, "noise_alt": 0x87654321,
        "nonperiodic_percussion": 0x64136F23,
        "nonperiodic_percussion_alt": 0x4A2B7C31,
    }.get(style)


def render(name: str, bpm: float | None, style: str, target: Path) -> dict:
    duration, sample_rate, channels = CASE_VARIANTS.get(
        name, (DURATION_SECONDS, SAMPLE_RATE, 1))
    total = sample_rate * duration
    samples = bytearray(total * channels * 2)
    positions = []
    if style in ("tone", "tone_alt"):
        # Constant energy without beats. A transient at startup alone does
        # not establish tempo; accepting any BPM here is unsafe.
        for frame in range(total):
            sample = int(6000 * math.sin(2 * math.pi * (437 if style == "tone_alt" else 220) * frame / sample_rate))
            for ch in range(channels):
                struct.pack_into("<h", samples, 2 * (frame * channels + ch), sample)
    elif style in ("noise", "noise_alt"):
        # Deterministic white-ish xorshift noise: no private/copyright samples.
        state = 0x87654321 if style == "noise_alt" else 0x12345678
        for frame in range(total):
            state ^= (state << 13) & 0xffffffff
            state ^= state >> 17
            state ^= (state << 5) & 0xffffffff
            sample = int(state % 12001) - 6000
            for ch in range(channels):
                struct.pack_into("<h", samples, 2 * (frame * channels + ch), sample)
    elif style in ("nonperiodic_percussion", "nonperiodic_percussion_alt"):
        seed = 0x4A2B7C31 if style.endswith("_alt") else 0x64136F23
        render_nonperiodic_percussion(samples, duration, sample_rate, channels,
                                      seed)
    elif style in ("tempo_switch", "tempo_switch_alt"):
        positions = render_tempo_switch(samples, duration, sample_rate,
                                        channels, style.endswith("_alt"))
    elif style.startswith("drum_"):
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
        "ambiguity": ("meter_underdetermined"
                      if style in ("half_pulse", "sparse") else
                      "tempo_nonstationary" if style.startswith("tempo_switch") else "none"),
        "duration_sec": duration, "sample_rate": sample_rate,
        "channels": channels, "event_sample_positions": positions,
        # Distinguish the predeclared *musical* grid from audible onsets.
        "beat_sample_positions": oracle_beat_grid(bpm, style, duration, sample_rate),
        "beat_grid_status": ("not_globally_defined" if bpm is None else
                             "latent_partly_silent_or_shifted"
                             if style in ("half_pulse", "sparse", "swing",
                                          "drum_dropouts") else "declared"),
        "tempo_segments": oracle_tempo_segments(style, duration, sample_rate),
        "meter": ("5/4" if style == "drum_odd_five" else
                  "4/4" if style.startswith("drum_") else "unspecified"),
        "generator_seed": fixture_seed(style),
        "seed_recipe": ("indexed_drum_voice_seeds_v1"
                        if style.startswith("drum_") or
                           style.startswith("tempo_switch") else
                        "fixed_xorshift32_v1" if fixture_seed(style) is not None
                        else "deterministic_no_prng"),
        "corpus_partition": ("prospective_holdout" if name.startswith("prospective_")
                             else "window_evaluation" if name.startswith("window_")
                             else "development"),
        "wav_sha256": hashlib.sha256(target.read_bytes()).hexdigest(),
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path)
    parser.add_argument("--case", choices=[c[0] for c in CASES],
                        help="Generate one public fixture for real-host smoke")
    parser.add_argument("--list-cases", action="store_true",
                        help="Emit only the fixed public fixture IDs")
    parser.add_argument("--window-case", choices=[c[0] for c in WINDOW_CASES],
                        help="Generate one 120s public window-sweep fixture")
    parser.add_argument("--list-window-cases", action="store_true",
                        help="List the six 120s public window-sweep fixture IDs")
    parser.add_argument("--prospective-case", choices=[c[0] for c in PROSPECTIVE_CASES],
                        help="Generate one predeclared synthetic parameter holdout")
    parser.add_argument("--list-prospective-cases", action="store_true",
                        help="List the predeclared synthetic parameter holdout IDs")
    args = parser.parse_args()
    list_count = sum((args.list_cases, args.list_window_cases,
                      args.list_prospective_cases))
    selected_count = sum(x is not None for x in
                         (args.case, args.window_case, args.prospective_case))
    if list_count:
        if args.output is not None or selected_count or list_count != 1:
            parser.error("list exactly one fixture family; do not generate files")
        family = (WINDOW_CASES if args.list_window_cases else
                  PROSPECTIVE_CASES if args.list_prospective_cases else CASES)
        for name, _, _ in family:
            print(name)
        return
    if selected_count > 1:
        parser.error("select at most one fixture family")
    if args.output is None:
        parser.error("--output is required for generation")
    dest = args.output.resolve()
    dest.mkdir(parents=True, exist_ok=True)
    if any(dest.iterdir()):
        parser.error("output directory must be empty (fail closed)")
    manifest = []
    cases = ([c for c in PROSPECTIVE_CASES if c[0] == args.prospective_case]
             if args.prospective_case is not None else
             [c for c in WINDOW_CASES if c[0] == args.window_case]
             if args.window_case is not None else
             [c for c in CASES if args.case is None or c[0] == args.case])
    for name, bpm, style in cases:
        manifest.append(render(name, bpm, style, dest / (name + ".wav")))
    (dest / "reference.json").write_text(
        json.dumps({"schema": "synthetic_bpm_reference_v1",
                    "oracle_metadata_revision": "musical_grid_v2",
                    "cases": manifest},
                   indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"SYNTHETIC_REFERENCE_GENERATED={len(manifest)}")
    print("SYNTHETIC_REFERENCE_NO_OWNER_AUDIO=PASS")


if __name__ == "__main__":
    main()
