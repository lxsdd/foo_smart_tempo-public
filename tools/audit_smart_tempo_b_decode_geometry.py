#!/usr/bin/env python3
"""Independent native decoder-window conservation for disposable CI WAV traces.

This audits ACTUAL decoded and resampled frame counts, not the theoretical
analysis-pass geometry. No PCM, music identifiers, oracle BPM or user paths.
It cannot prove exact seek scheduling without an independent plan witness.
"""
from __future__ import annotations

import math


class DecodeGeometryError(ValueError):
    pass


def reconcile(rows: list[dict], *, required: bool = False) -> dict:
    windows = [r["metrics"] for r in rows if r["stage"] == "decode_window"]
    if not windows:
        if required:
            raise DecodeGeometryError("original-host decode windows absent")
        return {"decode_geometry_verified": 0, "decode_geometry_passes": 0,
                "decode_geometry_truncated_passes": 0,
                "decode_geometry_coverage_seconds": 0.0,
                "decode_geometry_overlap_seconds": 0.0,
                "decode_geometry_analysis_seconds": 0.0,
                "decode_geometry_source_analysis_delta_seconds": 0.0}
    if len(windows) > 128:
        raise DecodeGeometryError("decode pass bound exceeded")
    first = windows[0]
    declared = first["pass_total"]
    source_rate = first["source_rate_hz"]
    analysis_rate = first["analysis_rate_hz"]
    requested = first["requested_frames"]
    if (type(declared) is not int or not 1 <= declared <= 128 or
            type(source_rate) is not int or not 8000 <= source_rate <= 384000 or
            type(analysis_rate) is not int or
            analysis_rate != min(source_rate, 48000) or
            type(requested) is not int or not 0 < requested <= source_rate * 600):
        raise DecodeGeometryError("native decode configuration invalid")
    if required and len(windows) != declared:
        raise DecodeGeometryError("native pass capture incomplete")
    if len(windows) > declared:
        raise DecodeGeometryError("more decoded passes than planned")
    intervals = []
    analysis_seconds = decoded_seconds = 0.0
    truncated = 0
    for i, m in enumerate(windows):
        if (type(m["pass_index"]) is not int or m["pass_index"] != i or
                m["pass_total"] != declared or
                m["source_rate_hz"] != source_rate or
                m["analysis_rate_hz"] != analysis_rate or
                m["requested_frames"] != requested):
            raise DecodeGeometryError("native per-pass configuration drift")
        decoded = m["decoded_frames"]
        analysis = m["analysis_frames"]
        start = m["start_sec"]
        if (type(decoded) is not int or type(analysis) is not int or
                not 0 < decoded <= requested or not 0 < analysis or
                type(start) not in (float, int) or
                not math.isfinite(start) or not 0 <= start <= 86400 or
                (i and start < windows[i - 1]["start_sec"])):
            raise DecodeGeometryError("invalid decoded frame/offset evidence")
        source_seconds = decoded / source_rate
        output_seconds = analysis / analysis_rate
        # No resampler means the original analyzer consumes exactly one
        # analysis frame per decoded source frame. With a resampler, allow
        # bounded filter/flush latency but never a lost multi-second window.
        if analysis_rate == source_rate:
            if analysis != decoded:
                raise DecodeGeometryError("same-rate analysis lost source frames")
        elif abs(output_seconds - source_seconds) > .25:
            raise DecodeGeometryError("resampler output duration drift")
        decoded_seconds += source_seconds
        analysis_seconds += output_seconds
        truncated += int(decoded < requested)
        intervals.append((start, start + source_seconds))
    # Independent interval-union sweep, not the native C++ counter or
    # an inference from the number of requested analysis passes.
    covered = 0.0
    right = -1.0
    for left, stop in intervals:
        covered += max(0.0, stop - max(left, right))
        right = max(right, stop)
    overlap = decoded_seconds - covered
    if (not math.isfinite(covered) or covered <= 0 or
            overlap < -1e-8 or overlap > decoded_seconds + 1e-8):
        raise DecodeGeometryError("invalid decoded interval union")
    return {
        "decode_geometry_verified": int(len(windows) == declared),
        "decode_geometry_passes": len(windows),
        "decode_geometry_truncated_passes": truncated,
        "decode_geometry_coverage_seconds": round(covered, 6),
        "decode_geometry_overlap_seconds": round(max(0.0, overlap), 6),
        "decode_geometry_analysis_seconds": round(analysis_seconds, 6),
        "decode_geometry_source_analysis_delta_seconds":
            round(analysis_seconds - decoded_seconds, 6),
    }
