#!/usr/bin/env python3
"""Synthetic-only, read-only genre-prior counterfactual contracts.

This is NOT a replacement for real A/B/C host runs. A is the frozen audio-only
shadow over a fixed candidate board; C may only abstain on a narrow, genuinely
measured competing hypothesis. B (legacy genre host) must be measured separately.
No oracle, track ID, filename or tag enters a candidate selector.
"""
from __future__ import annotations

import math
from collections.abc import Mapping, Sequence

MIN_LOCAL = 0.95
ALIAS_MARGIN = 0.10
NEAR_TIE = 0.02


def _board(candidates: Sequence[Mapping]) -> list[tuple[float, float, int]]:
    if len(candidates) > 32:
        raise ValueError('unbounded synthetic candidate board')
    rows = []
    for item in candidates:
        if not {'bpm', 'local_exact', 'rank'}.issubset(item):
            raise ValueError('missing audio candidate field')
        bpm, score, rank = float(item['bpm']), float(item['local_exact']), int(item['rank'])
        if not (math.isfinite(bpm) and 0 < bpm <= 1000 and
                math.isfinite(score) and 0 <= score <= 1 and 1 <= rank <= 256):
            raise ValueError('invalid audio candidate')
        rows.append((bpm, score, rank))
    return sorted(rows, key=lambda x: (-x[1], x[2]))


def _ranges(genres: Sequence[tuple[float, float]]) -> tuple[float, float] | None:
    if len(genres) > 4:
        raise ValueError('too many synthetic genre ranges')
    if not genres:
        return None
    lo, hi = 0., 1000.
    for pair in genres:
        if len(pair) != 2:
            raise ValueError('invalid synthetic genre range')
        low, high = float(pair[0]), float(pair[1])
        if not (math.isfinite(low) and math.isfinite(high) and
                0 < low < high <= 1000):
            raise ValueError('invalid synthetic genre bounds')
        lo, hi = max(lo, low), min(hi, high)
    return (lo, hi) if lo < hi else None  # contradictory tags: neutral


def audio_only(candidates: Sequence[Mapping]) -> float | None:
    """Frozen audio-only comparator; labels and genres are not consulted."""
    rows = _board(candidates)
    if not rows or rows[0][1] < MIN_LOCAL:
        return None
    bpm, best, _ = rows[0]
    for other, score, _ in rows[1:]:
        ratio = other / bpm
        if (abs(ratio - .5) <= .01 or abs(ratio - 2) <= .02) and best - score <= ALIAS_MARGIN:
            return None
    return bpm


def conservative_genre(candidates: Sequence[Mapping],
                       genres: Sequence[tuple[float, float]]) -> float | None:
    """Genre can trigger REVIEW HOLD only; never manufacture/promote a BPM.

    A disagreement is actionable only when the audio independently contains a
    second high-confidence near-tied candidate. Wrong/out-of-range tags cannot
    replace a clear measured audio winner; conflicting tags become neutral.
    """
    rows = _board(candidates)
    base = audio_only(candidates)
    bounds = _ranges(genres)
    if base is None or bounds is None:
        return base
    lo, hi = bounds
    if lo <= base <= hi:
        return base
    top = rows[0][1]
    for bpm, score, _ in rows[1:]:
        if (lo <= bpm <= hi and score >= MIN_LOCAL and
                top - score <= NEAR_TIE and abs(bpm - base) > .20):
            return None
    return base


def compare_audio_and_genre(candidates: Sequence[Mapping],
                            genre_scenarios: Mapping[str, Sequence[tuple[float, float]]],
                            legacy_host: Mapping[str, float | None] | None = None) -> dict:
    """A/C proposals without oracle. B must be supplied by a real host run.

    The B result is intentionally None when no legacy host measurement exists.
    This prevents accidental claims that a Python approximation is the existing
    C++ genre route or that a candidate-board counterfactual is a host A/B/C.
    """
    a = audio_only(candidates)
    out = {}
    for name, ranges in genre_scenarios.items():
        if not name.isascii() or not name.replace('_', '').isalnum() or len(name) > 40:
            raise ValueError('unbounded synthetic scenario label')
        c = conservative_genre(candidates, ranges)
        b = None
        if legacy_host is not None:
            if name not in legacy_host:
                raise ValueError('missing measured legacy host scenario')
            b = legacy_host[name]
            if b is not None and (not isinstance(b, (int, float)) or
                                  not math.isfinite(b) or b <= 0):
                raise ValueError('invalid legacy host measurement')
        out[name] = dict(audio_shadow=a, legacy_host=b,
                         legacy_measured=legacy_host is not None,
                         conservative_shadow=c)
    return out
