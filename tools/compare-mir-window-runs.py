#!/usr/bin/env python3
"""Compare two private foobar Smart Tempo verbose logs offline.

This evaluator is intentionally reference-blind and does not upload any data.
The inputs may contain private track labels: NEVER paste input logs or raw output
into public issues or CI. This source file contains no owner music data.
"""
from __future__ import annotations

import argparse
import collections
import csv
import json
import math
import re
from dataclasses import dataclass, field
from pathlib import Path

EVENT = re.compile(
    r"foo_smart_tempo: \[(?P<label>.*)\] "
    r"(?P<event>MirAnalysisProvenance|MirSamplingPlan|MirPolicyCandidateBoard|"
    r"MirPolicyDecision|Decision:|Track Timing:|Final Output:)(?=\s|$)"
)
BPM = re.compile(r"^([0-9]+(?:[.,][0-9]+)?) BPM(?:,|$)")
CANDIDATE_FIELDS = (
    "origin", "cluster_bpm", "local_exact_bpm", "local_exact_score", "base_score",
    "alias_classes", "support", "winner_support", "score_sum",
    "best_combined_score", "pulse_score", "pulse_section_support",
    "pulse_phase_vs", "pulse_axial_phase_vs", "continuous_score",
    "continuous_section_support", "continuous_score_ratio",
    "continuous_section_stability", "rows", "total_segment_count",
)


class EvidenceError(ValueError):
    pass


def get_space_field(payload: str, name: str) -> str:
    match = re.search(r"(?:^|\s)" + re.escape(name) + r"=([^\s]+)", payload)
    if not match:
        raise EvidenceError("Missing telemetry field: " + name)
    return match.group(1)


def get_csv_field(payload: str, name: str) -> str:
    for item in payload.split(", "):
        if item.startswith(name + "="):
            return item[len(name) + 1:]
    raise EvidenceError("Missing decision field: " + name)


def as_float(value: str) -> float:
    try:
        v = float(value.replace(",", "."))
    except ValueError as e:
        raise EvidenceError("Non-numeric telemetry value") from e
    if not math.isfinite(v):
        raise EvidenceError("Non-finite telemetry value")
    return v


def as_int(value: str) -> int:
    if not re.fullmatch(r"[0-9]+", value):
        raise EvidenceError("Malformed nonnegative integer telemetry value")
    return int(value)


@dataclass
class Track:
    key: tuple[str, int]
    label: str = field(repr=False)
    build_id: str
    schema: str
    plan: dict = field(default_factory=dict)
    candidates: list[tuple] = field(default_factory=list)
    policy: dict = field(default_factory=dict)
    decision: dict = field(default_factory=dict)
    final: dict = field(default_factory=dict)
    timing_ms: float | None = None


def parse_log(path: Path) -> dict[tuple[str, int], Track]:
    # Two passes allow interleaved per-track telemetry from parallel workers.
    events = []
    with path.open("r", encoding="utf-8-sig", errors="replace") as reader:
        for line in reader:
            match = EVENT.search(line)
            if match:
                events.append((match["label"], match["event"],
                               line[match.end():].strip()))
    tracks: dict[tuple[str, int], Track] = {}
    labels: dict[str, tuple[str, int] | None] = {}
    for label, event, payload in events:
        if event != "MirAnalysisProvenance":
            continue
        key = (get_space_field(payload, "track_key"),
               as_int(get_space_field(payload, "subsong")))
        if key in tracks:
            raise EvidenceError("Duplicate track provenance within one run")
        if not re.fullmatch(r"[0-9]+", key[0]):
            raise EvidenceError("Malformed track identity")
        tracks[key] = Track(
            key=key, label=label,
            build_id=get_space_field(payload, "build_id"),
            schema=get_space_field(payload, "policy_schema"),
        )
        if label in labels:
            labels[label] = None
        else:
            labels[label] = key
    if not tracks:
        raise EvidenceError("Missing MirAnalysisProvenance: enable verbose logs")

    for label, event, payload in events:
        if event == "MirAnalysisProvenance":
            continue
        if event in {"MirSamplingPlan", "MirPolicyCandidateBoard"}:
            raw_key = get_space_field(payload, "track_key")
            if event == "MirSamplingPlan":
                key = (raw_key, as_int(get_space_field(payload, "subsong")))
            else:
                associated = [k for k, record in tracks.items()
                              if k[0] == raw_key and record.label == label]
                if len(associated) != 1:
                    raise EvidenceError("Ambiguous candidate board track_key/subsong")
                key = associated[0]
        else:
            key = labels.get(label)
            if key is None:
                raise EvidenceError("Ambiguous or missing label-only final event")
        if key not in tracks:
            raise EvidenceError("Telemetry event without matching provenance")
        track = tracks[key]
        if event == "MirSamplingPlan":
            if track.plan:
                raise EvidenceError("Duplicate sampling plan")
            if get_space_field(payload, "schema") != "mir_sampling_plan_v1":
                raise EvidenceError("Unsupported sampling plan schema")
            track.plan = {
                "requested_window_seconds": as_int(get_space_field(payload, "requested_window_seconds")),
                "requested_passes": as_int(get_space_field(payload, "requested_passes")),
                "effective_window_seconds": as_float(get_space_field(payload, "effective_window_seconds")),
                "effective_passes": as_int(get_space_field(payload, "effective_passes")),
                "known_track_length": as_int(get_space_field(payload, "known_track_length")),
                "track_length_seconds": as_float(get_space_field(payload, "track_length_seconds")),
                "offset_min_pct": as_int(get_space_field(payload, "offset_min_pct")),
                "offset_max_pct": as_int(get_space_field(payload, "offset_max_pct")),
            }
        elif event == "MirPolicyCandidateBoard":
            if get_space_field(payload, "schema") != "mir_policy_candidate_board_v1":
                raise EvidenceError("Unsupported candidate board schema")
            track.candidates.append(tuple(get_space_field(payload, k) for k in CANDIDATE_FIELDS))
        elif event == "MirPolicyDecision":
            if track.policy:
                raise EvidenceError("Duplicate policy decision")
            track.policy = {
                "write": as_int(get_space_field(payload, "output_would_write")),
                "hold": as_int(get_space_field(payload, "output_would_review_hold")),
                "source_candidate": as_int(get_space_field(payload, "source_candidate")),
                "bpm": as_float(get_space_field(payload, "output_bpm")),
            }
        elif event == "Decision:":
            if track.decision:
                raise EvidenceError("Duplicate terminal decision")
            track.decision = {
                "final_bpm": as_float(get_csv_field(payload, "final")),
                "class": get_csv_field(payload, "decision_class"),
            }
        elif event == "Final Output:":
            if track.final:
                raise EvidenceError("Duplicate final output")
            if payload.startswith("no writable BPM"):
                track.final = {"state": "HOLD", "bpm": None}
            else:
                m = BPM.match(payload)
                if not m:
                    raise EvidenceError("Unknown Final Output form")
                final_bpm = as_float(m.group(1))
                if final_bpm <= 0.0:
                    raise EvidenceError("Nonpositive writable BPM")
                track.final = {"state": "WRITE", "bpm": final_bpm}
        elif event == "Track Timing:":
            if track.timing_ms is not None:
                raise EvidenceError("Duplicate timing")
            track.timing_ms = as_float(get_csv_field(payload, "total_ms"))
    for track in tracks.values():
        if not track.plan or not track.decision or not track.final or track.timing_ms is None:
            raise EvidenceError("Incomplete per-track evidence; do not compare partial logs")
        if track.plan["requested_window_seconds"] <= 0 or track.plan["requested_passes"] <= 0:
            raise EvidenceError("Invalid plan geometry")
        if track.plan["effective_window_seconds"] <= 0 or track.plan["effective_passes"] <= 0:
            raise EvidenceError("Invalid effective plan")
        if track.final["state"] == "WRITE" and track.decision["final_bpm"] <= 0:
            raise EvidenceError("Final writable result contradicts decision")
    return tracks


def candidate_stage(track: Track) -> str:
    """Diagnostic stage only; never equate policy state with ground truth."""
    if not track.policy:
        return "UNKNOWN_POLICY"
    has_board = len(track.candidates) > 0
    has_source = track.policy["source_candidate"] == 1
    if track.policy["source_candidate"] not in (0, 1):
        return "INCONSISTENT"
    if not has_board and not has_source:
        return "NO_MEASURED_SOURCE"
    if not has_board and has_source:
        return "SOURCE_WITHOUT_POLICY_BOARD"
    if track.final["state"] == "HOLD":
        return "MEASURED_BUT_HOLD"
    return "MEASURED_AND_OUTPUT"


def compare(baseline: dict, variant: dict) -> dict:
    if set(baseline) != set(variant):
        raise EvidenceError("Track identity mismatch: all runs must use identical tracks")

    def uniform_requested_geometry(run: dict) -> tuple[int, int]:
        settings = {
            (t.plan["requested_window_seconds"], t.plan["requested_passes"])
            for t in run.values()
        }
        if len(settings) != 1:
            raise EvidenceError("Mixed sampling settings within a single run")
        return next(iter(settings))

    a_seconds, a_passes = uniform_requested_geometry(baseline)
    b_seconds, b_passes = uniform_requested_geometry(variant)
    if a_seconds != b_seconds and a_passes != b_passes:
        raise EvidenceError("Both window length and pass count changed: confounded experiment")
    sweep_dimension = ("window_seconds" if a_seconds != b_seconds else
                       "passes" if a_passes != b_passes else "repeat")

    # No comparisons between different engine builds/policy schemas.
    rows = []
    for index, key in enumerate(sorted(baseline), 1):
        a, b = baseline[key], variant[key]
        if (a.build_id, a.schema) != (b.build_id, b.schema):
            raise EvidenceError("Build or policy schema mismatch")
        if a.plan["offset_min_pct"] != b.plan["offset_min_pct"] or a.plan["offset_max_pct"] != b.plan["offset_max_pct"]:
            raise EvidenceError("Offset policy mismatch")
        ca, cb = collections.Counter(a.candidates), collections.Counter(b.candidates)
        candidate_overlap = sum((ca & cb).values())
        rows.append({
            "track": "T%04d" % index,
            "baseline_window": a.plan["effective_window_seconds"],
            "variant_window": b.plan["effective_window_seconds"],
            "baseline_passes": a.plan["effective_passes"],
            "variant_passes": b.plan["effective_passes"],
            "baseline_candidate_count": len(a.candidates),
            "variant_candidate_count": len(b.candidates),
            "baseline_candidate_stage": candidate_stage(a),
            "variant_candidate_stage": candidate_stage(b),
            "identical_candidate_rows": candidate_overlap,
            "candidate_board_identical": ca == cb,
            "baseline_state": a.final["state"],
            "variant_state": b.final["state"],
            "baseline_bpm": a.final["bpm"],
            "variant_bpm": b.final["bpm"],
            "baseline_policy_write": a.policy.get("write"),
            "variant_policy_write": b.policy.get("write"),
            "baseline_time_ms": a.timing_ms,
            "variant_time_ms": b.timing_ms,
            "decision_class_changed": a.decision["class"] != b.decision["class"],
        })
    counts = collections.Counter((r["baseline_state"], r["variant_state"]) for r in rows)
    return {
        "schema": "mir_window_comparison_v1",
        "track_count": len(rows),
        "sweep_dimension": sweep_dimension,
        "baseline_requested_window_seconds": a_seconds,
        "variant_requested_window_seconds": b_seconds,
        "baseline_requested_passes": a_passes,
        "variant_requested_passes": b_passes,
        "write_to_hold": counts["WRITE", "HOLD"],
        "hold_to_write": counts["HOLD", "WRITE"],
        "write_to_write": counts["WRITE", "WRITE"],
        "hold_to_hold": counts["HOLD", "HOLD"],
        "candidate_boards_changed": sum(not r["candidate_board_identical"] for r in rows),
        "baseline_candidate_stages": dict(sorted(collections.Counter(
            r["baseline_candidate_stage"] for r in rows).items())),
        "variant_candidate_stages": dict(sorted(collections.Counter(
            r["variant_candidate_stage"] for r in rows).items())),
        "candidate_stage_changes": sum(
            r["baseline_candidate_stage"] != r["variant_candidate_stage"] for r in rows),
        "decision_classes_changed": sum(r["decision_class_changed"] for r in rows),
        "mean_baseline_time_ms": sum(r["baseline_time_ms"] for r in rows) / len(rows),
        "mean_variant_time_ms": sum(r["variant_time_ms"] for r in rows) / len(rows),
        "rows": rows,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--variant", type=Path, required=True)
    parser.add_argument("--output-json", type=Path, required=True)
    parser.add_argument("--output-csv", type=Path)
    args = parser.parse_args()
    result = compare(parse_log(args.baseline), parse_log(args.variant))
    args.output_json.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    if args.output_csv:
        with args.output_csv.open("w", newline="", encoding="utf-8") as out:
            writer = csv.DictWriter(out, fieldnames=list(result["rows"][0]))
            writer.writeheader()
            writer.writerows(result["rows"])
    print("MW1_EVIDENCE_COMPARE=PASS tracks=%d; output files stay local" % result["track_count"])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
