#!/usr/bin/env python3
"""Compare legacy and current Smart Tempo verbose logs locally, without audio or tags.

Supports an older run with global MIR sampling preferences but no per-track
MirSamplingPlan. Reports aggregates ONLY: no labels, paths, track keys, raw BPM,
or per-track rows. Outputs are still private research evidence: do not upload.
"""
from __future__ import annotations

import argparse
import collections
import json
import math
import re
from dataclasses import dataclass, field as dataclass_field
from pathlib import Path

EVENT = re.compile(
    r"foo_smart_tempo: \[(?P<label>.*)\] "
    r"(?P<event>MirAnalysisProvenance|MirSamplingPlan|MirPolicyCandidateBoard|"
    r"MirPolicyDecision|HodgkinsonPrimarySegment|Decision:|"
    r"Track Timing:|Final Output:)(?=\s|$)"
)
PREFS = re.compile(
    r"foo_smart_tempo prefs \(MIR Engine\): .*?"
    r"secondsPerSample=(\d+), samplesPerSong=(\d+), "
    r"offsetRange=(\d+)-(\d+)%"
)
OUTPUT = re.compile(r"^([0-9]+(?:[.,][0-9]+)?) BPM(?:,|$)")


class EvidenceError(ValueError):
    """No path, label, track key, or source data should appear in errors."""


def field(payload: str, name: str) -> str:
    hit = re.search(r"(?:^|\s)" + re.escape(name) + r"=([^\s]+)", payload)
    if hit is None:
        raise EvidenceError("Missing required evidence field: " + name)
    return hit.group(1)


def numeric(value: str) -> float:
    try:
        result = float(value.replace(",", "."))
    except ValueError as error:
        raise EvidenceError("Invalid numeric evidence") from error
    if not math.isfinite(result):
        raise EvidenceError("Nonfinite numeric evidence")
    return result


def integer(value: str) -> int:
    if not re.fullmatch(r"\d+", value):
        raise EvidenceError("Invalid integer evidence")
    return int(value)


def csv_field(payload: str, name: str) -> str:
    for part in payload.split(", "):
        if part.startswith(name + "="):
            return part[len(name) + 1:]
    raise EvidenceError("Missing required terminal field: " + name)


@dataclass
class Track:
    # Identity and display labels remain only in memory, never in output.
    identity: tuple[str, int] = dataclass_field(repr=False)
    label: str = dataclass_field(repr=False)
    build: str = dataclass_field(repr=False)
    schema: str = ""
    plan: tuple[int, int, int, int] | None = None
    board: list[str] = dataclass_field(default_factory=list, repr=False)
    source_candidate: int | None = None
    decision: str | None = None
    state: str | None = None
    bpm: float | None = None
    timing_ms: float | None = None
    segments: dict[int, tuple[int, str]] = dataclass_field(default_factory=dict, repr=False)


@dataclass
class Run:
    tracks: dict[tuple[str, int], Track]
    requested: tuple[int, int, int, int]
    has_per_track_geometry: bool
    build: str = dataclass_field(repr=False)
    schema: str = ""


def parse(path: Path) -> Run:
    events: list[tuple[str, str, str]] = []
    prefs: set[tuple[int, int, int, int]] = set()
    with path.open("r", encoding="utf-8-sig", errors="replace") as src:
        for line in src:
            preference = PREFS.search(line)
            if preference:
                prefs.add(tuple(int(x) for x in preference.groups()))
            match = EVENT.search(line)
            if match:
                events.append((match["label"], match["event"],
                               line[match.end():].strip()))
    tracks: dict[tuple[str, int], Track] = {}
    labels: dict[str, tuple[str, int] | None] = {}
    for label, event, payload in events:
        if event != "MirAnalysisProvenance":
            continue
        raw_key = dataclass_field(payload, "track_key")
        key = (raw_key, integer(field(payload, "subsong")))
        if not raw_key.isdecimal() or key in tracks:
            raise EvidenceError("Invalid or repeated track provenance")
        tracks[key] = Track(key, label, field(payload, "build_id"),
                            field(payload, "policy_schema"))
        labels[label] = key if label not in labels else None
    if not tracks:
        raise EvidenceError("No per-track provenance; enable verbose logging")

    for label, event, payload in events:
        if event == "MirAnalysisProvenance":
            continue
        if event in {"MirSamplingPlan", "MirPolicyCandidateBoard"}:
            raw_key = dataclass_field(payload, "track_key")
            if event == "MirSamplingPlan":
                key = (raw_key, integer(field(payload, "subsong")))
            else:
                possible = [key for key, value in tracks.items()
                            if key[0] == raw_key and value.label == label]
                if len(possible) != 1:
                    raise EvidenceError("Ambiguous candidate board identity")
                key = possible[0]
        else:
            key = labels.get(label)
            if key is None:
                raise EvidenceError("Ambiguous terminal or segment label")
        if key not in tracks:
            raise EvidenceError("Event does not match track provenance")
        track = tracks[key]
        if event == "MirSamplingPlan":
            if track.plan is not None or field(payload, "schema") != "mir_sampling_plan_v1":
                raise EvidenceError("Duplicate or unsupported sampling plan")
            track.plan = (
                integer(field(payload, "requested_window_seconds")),
                integer(field(payload, "requested_passes")),
                integer(field(payload, "offset_min_pct")),
                integer(field(payload, "offset_max_pct")),
            )
            if numeric(field(payload, "effective_window_seconds")) <= 0 or (
                integer(field(payload, "effective_passes")) <= 0
            ):
                raise EvidenceError("Invalid effective geometry")
        elif event == "MirPolicyCandidateBoard":
            if field(payload, "schema") != "mir_policy_candidate_board_v1":
                raise EvidenceError("Unknown candidate board schema")
            # Index order should not affect measured-evidence equality.
            signature = " ".join(chunk for chunk in payload.split()
                                 if not chunk.startswith("index="))
            track.board.append(signature)
        elif event == "MirPolicyDecision":
            if track.source_candidate is not None:
                raise EvidenceError("Duplicate policy decision")
            track.source_candidate = integer(field(payload, "source_candidate"))
            if track.source_candidate not in (0, 1):
                raise EvidenceError("Invalid source candidate flag")
        elif event == "HodgkinsonPrimarySegment":
            index = integer(field(payload, "segment_index"))
            count = integer(field(payload, "segment_count"))
            reason = dataclass_field(payload, "reason")
            if count == 0 or index >= count or not re.fullmatch(
                r"[a-z][a-z0-9_]*", reason
            ):
                raise EvidenceError("Invalid segment evidence")
            if index in track.segments or (track.segments and
                    next(iter(track.segments.values()))[0] != count):
                raise EvidenceError("Duplicated or inconsistent segment evidence")
            track.segments[index] = (count, reason)
        elif event == "Decision:":
            if track.decision is not None:
                raise EvidenceError("Duplicate final decision")
            track.decision = csv_field(payload, "decision_class")
        elif event == "Track Timing:":
            if track.timing_ms is not None:
                raise EvidenceError("Duplicate timing")
            track.timing_ms = numeric(csv_field(payload, "total_ms"))
            if track.timing_ms < 0:
                raise EvidenceError("Negative analysis time")
        elif event == "Final Output:":
            if track.state is not None:
                raise EvidenceError("Duplicate terminal output")
            if payload.startswith("no writable BPM"):
                track.state = "HOLD"
            else:
                match = OUTPUT.match(payload)
                if match is None:
                    raise EvidenceError("Unknown terminal output format")
                track.bpm = numeric(match.group(1))
                if track.bpm <= 0:
                    raise EvidenceError("Nonpositive terminal BPM")
                track.state = "OUTPUT"

    if any(t.state is None or t.decision is None or t.timing_ms is None
           for t in tracks.values()):
        raise EvidenceError("Incomplete run; no partial-track comparisons")
    builds = {t.build for t in tracks.values()}
    schemas = {t.schema for t in tracks.values()}
    if len(builds) != 1 or len(schemas) != 1:
        raise EvidenceError("Mixed engine builds or policy schemas within a run")
    plans = [t.plan for t in tracks.values()]
    if any(plan is None for plan in plans):
        if any(plan is not None for plan in plans):
            raise EvidenceError("Partially missing per-track plans")
        if len(prefs) != 1:
            raise EvidenceError("Legacy run requires one unambiguous global MIR settings line")
        requested = next(iter(prefs))
        has_plan = False
    else:
        unique = set(plans)
        if len(unique) != 1:
            raise EvidenceError("Mixed requested window geometry within a run")
        requested = next(iter(unique))
        if prefs and (len(prefs) != 1 or next(iter(prefs)) != requested):
            raise EvidenceError("Global settings contradict per-track geometry")
        has_plan = True
    if requested[0] <= 0 or requested[1] <= 0 or not (
        0 <= requested[2] <= requested[3] <= 100
    ):
        raise EvidenceError("Invalid requested sampling configuration")
    return Run(tracks, requested, has_plan, next(iter(builds)),
               next(iter(schemas)))


def stage(track: Track) -> str:
    if track.source_candidate is None:
        return "UNKNOWN_POLICY"
    if not track.board and track.source_candidate == 0:
        return "NO_MEASURED_SOURCE"
    if not track.board:
        return "SOURCE_WITHOUT_BOARD"
    if track.state == "HOLD":
        return "MEASURED_HELD"
    return "MEASURED_OUTPUT"


def aggregate(baseline: Run, variant: Run) -> dict:
    if set(baseline.tracks) != set(variant.tracks):
        raise EvidenceError("Runs contain different track identities")
    if baseline.schema != variant.schema:
        raise EvidenceError("Policy schema mismatch")
    if baseline.requested != variant.requested:
        raise EvidenceError("Different requested sampling configurations")
    if not baseline.tracks:
        raise EvidenceError("Empty comparison")
    changes = collections.Counter()
    transitions = collections.Counter()
    baseline_stage = collections.Counter()
    variant_stage = collections.Counter()
    prior_reasons = collections.Counter()
    next_reasons = collections.Counter()
    candidate_changes = 0
    primary_trace_changes = 0
    trace_incomplete = [0, 0]
    timing = [0.0, 0.0]
    for key in baseline.tracks:
        a, b = baseline.tracks[key], variant.tracks[key]
        stages = (stage(a), stage(b))
        transitions["->".join(stages)] += 1
        baseline_stage[stages[0]] += 1
        variant_stage[stages[1]] += 1
        changes[f"{a.state}_TO_{b.state}"] += 1
        if a.state != b.state:
            changes["TERMINAL_STATE_CHANGED"] += 1
        if a.bpm is not None and b.bpm is not None and (
                abs(a.bpm - b.bpm) > 0.01):
            changes["OUTPUT_BPM_CHANGED"] += 1
        if a.decision != b.decision:
            changes["DECISION_CLASS_CHANGED"] += 1
        candidate_changes += (collections.Counter(a.board) !=
                              collections.Counter(b.board))
        for index, track in enumerate((a, b)):
            if not track.segments or (
                len(track.segments) != next(iter(track.segments.values()))[0]
            ):
                trace_incomplete[index] += 1
        a_reasons = collections.Counter(x[1] for x in a.segments.values())
        b_reasons = collections.Counter(x[1] for x in b.segments.values())
        prior_reasons.update(a_reasons)
        next_reasons.update(b_reasons)
        primary_trace_changes += a_reasons != b_reasons
        timing[0] += a.timing_ms
        timing[1] += b.timing_ms
    return {
        "schema": "d12_crossbuild_aggregate_v1",
        "scope": "AGGREGATE_ONLY_NO_REFERENCE_ACCURACY_NO_TAG_WRITE_PROOF",
        "track_count": len(baseline.tracks),
        "engine_binaries_differ": baseline.build != variant.build,
        "sampling_evidence": (
            "PER_TRACK_BOTH" if baseline.has_per_track_geometry and
            variant.has_per_track_geometry else "LEGACY_GLOBAL_UNVERIFIED"
        ),
        "requested_window_seconds": baseline.requested[0],
        "requested_passes": baseline.requested[1],
        "terminal_transitions": dict(sorted(changes.items())),
        "baseline_candidate_stages": dict(sorted(baseline_stage.items())),
        "variant_candidate_stages": dict(sorted(variant_stage.items())),
        "candidate_stage_transitions": dict(sorted(transitions.items())),
        "candidate_boards_changed": candidate_changes,
        "segment_reason_distributions_changed_tracks": primary_trace_changes,
        "baseline_segment_reasons": dict(sorted(prior_reasons.items())),
        "variant_segment_reasons": dict(sorted(next_reasons.items())),
        "baseline_incomplete_segment_traces": trace_incomplete[0],
        "variant_incomplete_segment_traces": trace_incomplete[1],
        "mean_baseline_time_ms": round(timing[0] / len(baseline.tracks), 3),
        "mean_variant_time_ms": round(timing[1] / len(baseline.tracks), 3),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--variant", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        report = aggregate(parse(args.baseline), parse(args.variant))
    except (EvidenceError, OSError) as error:
        # Never print OSError filename, raw track identity or line content.
        reason = str(error) if isinstance(error, EvidenceError) else "cannot read local evidence"
        parser.exit(status=2, message="D12_COMPARE=REJECTED: " + reason + "\n")
    args.output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n",
                           encoding="utf-8")
    print("D12_COMPARE=PASS aggregate-only, track_count=%d" %
          report["track_count"])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
