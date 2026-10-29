#!/usr/bin/env python3
"""Independent CI-only seek-call witness, not a physical decoder-position test."""
from __future__ import annotations
import math

KEYS = frozenset(("known_length_sec", "planned_window_sec", "offset_pct",
                  "seek_target_sec", "can_seek", "seek_invoked", "seek_returned"))

class SeekPlanError(ValueError):
    pass

def reconcile(rows: list[dict], *, required: bool = False) -> dict:
    windows = [r["metrics"] for r in rows if r["stage"] == "decode_window"]
    empty = {"seek_plan_verified": 0, "seek_plan_passes": 0,
             "seek_calls_returned": 0, "seek_decoder_position_verified": 0}
    if not windows:
        if required:
            raise SeekPlanError("native windows absent")
        return empty
    present = [KEYS.issubset(m) for m in windows]
    if not all(present):
        if required or any(present):
            raise SeekPlanError("incomplete native seek witness")
        return empty
    first = windows[0]
    total, length, duration = [first[k] for k in
                               ("pass_total", "known_length_sec", "planned_window_sec")]
    if (type(total) is not int or not 1 <= total <= 128 or
            type(length) not in (float, int) or not math.isfinite(length) or
            not 0 <= length <= 86400 or
            type(duration) not in (float, int) or not math.isfinite(duration) or
            not 0 < duration <= 600 or
            (length and (length < 3 or duration > length - 1 + 1e-6)) or
            (not length and total != 1)):
        raise SeekPlanError("invalid independent plan envelope")
    calls = 0
    for i, m in enumerate(windows):
        if (m["pass_index"] != i or m["pass_total"] != total or
                m["known_length_sec"] != length or
                m["planned_window_sec"] != duration):
            raise SeekPlanError("per-pass plan drift")
        for k in ("offset_pct", "seek_target_sec", "start_sec"):
            if type(m[k]) not in (float, int) or not math.isfinite(m[k]):
                raise SeekPlanError("nonfinite plan/seek value")
        for k in ("can_seek", "seek_invoked", "seek_returned"):
            if type(m[k]) is not int or m[k] not in (0, 1):
                raise SeekPlanError("invalid seek flag")
        if (m["seek_invoked"] != m["can_seek"] or
                m["seek_returned"] != m["seek_invoked"] or
                (not m["can_seek"] and total != 1) or
                (i and m["can_seek"] != first["can_seek"])):
            raise SeekPlanError("native seek not called or returned")
        pct, start = 0.0, 0.0
        if length:
            pct = 20 + 60 * (.5 if total <= 1 else i / (total-1))
            if pct >= 80:
                pct = max(0.0, 80 - .000001)
            start = min(max(0.0, length*pct/100),
                        max(0.0, length-duration-.5))
        if (abs(m["offset_pct"]-pct) > 1e-6 or
                abs(m["start_sec"]-start) > 1e-6 or
                abs(m["seek_target_sec"] -
                    (start if m["seek_invoked"] else 0.0)) > 1e-6):
            raise SeekPlanError("actual seek call disagrees with independent plan")
        rate, frames = m["source_rate_hz"], m["requested_frames"]
        if (type(rate) is not int or not 8000 <= rate <= 384000 or
                type(frames) is not int or abs(frames-rate*duration) > 1.01):
            raise SeekPlanError("source-frame budget not planned")
        calls += m["seek_returned"]
    if required and len(windows) != total:
        raise SeekPlanError("incomplete pass capture")
    return {"seek_plan_verified": int(len(windows) == total),
            "seek_plan_passes": len(windows), "seek_calls_returned": calls,
            "seek_decoder_position_verified": 0}
