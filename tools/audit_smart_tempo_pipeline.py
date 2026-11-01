#!/usr/bin/env python3
"""Read-only end-to-end Smart Tempo source architecture audit.

Audits source structure, NOT musical accuracy. No external data, audio, titles,
paths beyond a fixed repository-relative source allowlist, or file writes.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import math
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
SRC = "src/foo_smart_tempo/"
ANALYZER = SRC + "modern_bpm_analyzer.cpp"
PRIMARY = SRC + "mir_candidate_pipeline.cpp"
WORKER = SRC + "bpm_auto_analysis_thread.cpp"
MIR = SRC + "hodgkinson_full_mir.cpp"
WRITE = SRC + "tag_write_dispatch.cpp"
WINDOW = SRC + "analysis_window_config.h"
PASS = SRC + "analysis_pass_schedule.h"

# Stages reflect actual functions/calls in the current source, NOT an idealized
# diagram. Each is independently located by a fixed literal source anchor.
# Stage IDs are fixed; future event schema must map to these exact transitions.
STAGES = (
    ("route_snapshot", WORKER, "SmartTempoMapper::get_route_matches_for_tag_values_snapshot(", "routing"),
    ("worker_analyzer", WORKER, "BpmAnalyzerFactory::create(", "worker"),
    ("worker_analyze_call", WORKER, "analyzer->analyze(", "worker"),
    ("worker_receives_suppression", WORKER, "m_suppress_write_results[index] = diag.suppress_write", "writer"),
    ("audio_open", ANALYZER, "input_file.open(", "frontend"),
    ("window_plan", ANALYZER, "make_analysis_window_plan(", "frontend"),
    ("pass_offset", ANALYZER, "compute_analysis_pass_offset(", "frontend"),
    ("resample", ANALYZER, "resampler_entry::g_create(", "frontend"),
    ("odf_observe", ANALYZER, "observe_mir_odf_energy_flux_samples(", "frontend"),
    ("onset_peaks", ANALYZER, "append_mir_odf_peaks_from_frames(", "frontend"),
    ("full_mir_segment", ANALYZER, "evaluate_hodgkinson_full_mir_segment_with_partial_bar_candidates(", "mir"),
    ("full_mir_aggregate", ANALYZER, "aggregate_hodgkinson_full_mir_only_results(", "mir"),
    ("candidate_topk", ANALYZER, "add_mir_topk_candidate(", "candidates"),
    ("policy_fullboard", ANALYZER, "prepare_mir_policy_fullboard_candidates(", "candidates"),
    ("primary_select", ANALYZER, "compute_mir_primary_selection(", "primary"),
    ("policy_select", ANALYZER, "compute_mir_policy_decision(", "policy"),
    ("policy_invariant", ANALYZER, "enforce_mir_measured_candidate_invariant(", "safety"),
    ("policy_final_bpm", ANALYZER, "effective_mir_policy_output_bpm(", "policy"),
    ("manual_export", ANALYZER, "prepare_mir_manual_review_candidates(", "export"),
    ("meter_onset_detection", MIR, "onset_detection_function(", "mir"),
    ("meter_autocorrelation", MIR, "normalized_circular_autocorr(", "mir"),
    ("meter_candidate_topk", MIR, "populate_top_k_segment_candidates(", "mir"),
    ("lane_classification", PRIMARY, "classify_mir_lane(", "primary"),
    ("candidate_collapse", PRIMARY, "collapse_mir_candidates(", "candidates"),
    ("review_merge", PRIMARY, "prepare_mir_manual_review_candidates(", "export"),
    ("measured_output_witness", PRIMARY, "mir_output_has_measured_candidate_witness(", "safety"),
    ("write_selection_guard", WRITE, "selection_is_safe(items, &blockedPath", "safety"),
    ("async_tag_dispatch", WRITE, "->update_info_async(", "writer"),
)
SOURCES = tuple(sorted(set(path for _, path, _, _ in STAGES) | {WINDOW, PASS}))
ORDERED_ANALYZER_STAGES = (
    "audio_open", "window_plan", "pass_offset", "resample",
    "full_mir_segment", "candidate_topk", "full_mir_aggregate",
    "policy_fullboard", "primary_select", "policy_select",
    "policy_invariant", "policy_final_bpm", "manual_export",
)
RESEARCH_GAPS = (
    "primary_candidate_eligibility_and_veto_reason_not_fully_exported",
    "merged_manual_review_source_is_not_primary_selector_reachability",
    "meter_beat_unit_identifiability_not_qualified_on_unseen_audio",
    "production_genre_wrong_tag_host_B_unmeasured",
    "cpu_peak_rss_and_decoder_read_amplification_unmeasured",
    "real_music_blind_holdout_not_qualified",
)


class AuditError(ValueError):
    pass


def load_sources(root: Path = ROOT) -> dict[str, str]:
    return {path: (root / path).read_text(encoding="utf-8") for path in SOURCES}


def _positive_double(src: str, name: str) -> float:
    m = re.search(r"\b" + re.escape(name) + r"\s*=\s*(\d+(?:\.\d+)?)\s*;", src)
    if m is None:
        raise AuditError("missing numeric invariant " + name)
    return float(m.group(1))


def _require(condition: bool, explanation: str) -> None:
    if not condition:
        raise AuditError(explanation)


def window_geometry(track_seconds: float, seconds: int, passes: int,
                    min_pct: int = 20, max_pct: int = 80) -> dict:
    """Independent direct translation of analysis_pass_schedule.h.

    This is a theoretical requested decode window overlap, not measured
    CPU, actual input reads, or proof of decoder seek implementation.
    """
    if not (math.isfinite(track_seconds) and track_seconds > 1 and
            seconds > 0 and passes >= 1 and 0 <= min_pct < max_pct <= 100):
        raise AuditError("invalid theoretical geometry")
    duration = min(float(seconds), max(0.0, track_seconds - 1.0))
    _require(duration > 0, "nonpositive analysis duration")
    intervals = []
    for i in range(passes):
        t = 0.5 if passes == 1 else i / (passes - 1)
        pct = min(min_pct + (max_pct - min_pct) * t, max_pct - 0.000001)
        start = min(max(track_seconds * pct / 100.0, 0.0),
                    max(0.0, track_seconds - duration - 0.5))
        intervals.append((start, start + duration))
    union = 0.0
    right = 0.0
    for a, b in sorted(intervals):
        union += max(0.0, b - max(a, right))
        right = max(right, b)
    requested = duration * passes
    return {"track_seconds": track_seconds, "window_seconds": duration,
            "passes": passes, "requested_decode_window_seconds": requested,
            "unique_interval_union_seconds": round(union, 6),
            "estimated_repeat_factor": round(requested / union, 4),
            "kind": "theoretical_window_geometry_not_runtime_performance"}


def audit_sources(sources: dict[str, str]) -> dict:
    _require(set(sources) == set(SOURCES), "source allowlist mismatch")
    stages = []
    positions = {}
    for name, path, anchor, phase in STAGES:
        text = sources[path]
        offset = text.find(anchor)
        _require(offset >= 0, "missing source stage " + name)
        line = text.count("\n", 0, offset) + 1
        stages.append({"id": name, "owner": path, "phase": phase,
                       "line": line, "anchor": anchor})
        positions[name] = (path, offset)
    for first, second in zip(ORDERED_ANALYZER_STAGES,
                             ORDERED_ANALYZER_STAGES[1:]):
        _require(positions[first][1] < positions[second][1],
                 f"source stage order drift {first} -> {second}")
    _require(positions["write_selection_guard"][1] <
             positions["async_tag_dispatch"][1],
             "unsafe writer guard ordering drift")
    writer = sources[WRITE].lower()
    for fragment in ('"mp3"', '"flac"', '"wv"',
                     "blocked_unsafe_virtual_subsong", "if (!selection_is_safe("):
        _require(fragment in writer, "unsafe virtual-subsong guard drift " + fragment)
    analyzer = sources[ANALYZER]
    pipeline = sources[PRIMARY]
    worker = sources[WORKER]
    _require("routeMatches.front()" in worker,
             "metadata primary-route precedence unexpectedly changed")
    _require("lowGenreContext" in pipeline and
             "routeCenter <= 95.0 || routeMax <= 110.0" in pipeline,
             "metadata lane-priority semantics changed; independent review required")
    _require("m_diag.suppress_write = true" in analyzer and
             "mir_writer_invariant_hold" in analyzer,
             "review-hold/writer measured-witness drift")
    _require('"primary_selector"' in pipeline and '"full_board"' in pipeline and
             '"partial_bar_aggregate"' in pipeline,
             "manual-review provenance composition changed")
    _require("m_diag.measured_bpm_candidates.push_back" in analyzer,
             "diagnostic manual export unavailable")
    _require("constexpr double kMinBpm = 50.0" in sources[MIR] and
             "constexpr double kMaxBpm = 200.0" in sources[MIR],
             "Hodgkinson core tempo limits changed")
    primary_min = _positive_double(pipeline, "kMirProductionMinBpm")
    primary_max = _positive_double(pipeline, "kMirProductionMaxBpm")
    _require((primary_min, primary_max) == (40.0, 220.0),
             "production rail changed; requalify range-edge cases")
    _require((_positive_double(sources[WINDOW], "kDefaultAnalysisSecondsToRead"),
              _positive_double(sources[WINDOW], "kDefaultAnalysisSamplePasses"))
             == (20.0, 50.0), "default analysis geometry changed")
    _require((_positive_double(sources[WINDOW], "kAnalysisOffsetMinPct"),
              _positive_double(sources[WINDOW], "kAnalysisOffsetMaxPct"))
             == (20.0, 80.0), "window distribution changed")
    _require("std::clamp(offset.startSec, 0.0, maxAllowedStartSec)" in sources[PASS],
             "pass offset clamp drift")
    _require("samplePasses <= 1" in sources[PASS] and
             "maxPct" not in sources[PASS],
             "pass time-spread formula changed")
    hashed = {path: hashlib.sha256(sources[path].encode("utf-8")).hexdigest()
              for path in SOURCES}
    return {"schema": "smart_tempo_architecture_source_audit_v1",
            "claim": "static_source_structure_only_not_musical_accuracy",
            "source_hashes": hashed,
            "stage_count": len(stages),
            "stages": stages,
            "core_bpm_rail": [50.0, 200.0],
            "production_bpm_rail": [primary_min, primary_max],
            "default_geometry_180s": window_geometry(180, 20, 50),
            "observed_source_risks": [
                "metadata_route_first_match",
                "metadata_low_lane_precedes_high_audio_lane",
                "internal_50_200_vs_output_40_220_rail_mismatch",
                "default_20s_x_50_overlapping_windows",
            ],
            "unqualified_evidence_gaps": list(RESEARCH_GAPS)}


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--json", action="store_true",
                        help="print safe, bounded source-relative JSON to stdout")
    args = parser.parse_args()
    result = audit_sources(load_sources())
    if args.json:
        print(json.dumps(result, sort_keys=True, indent=2))
    else:
        print("SMART_TEMPO_ARCHITECTURE_STATIC_AUDIT=PASS")
        print(f"SMART_TEMPO_ARCHITECTURE_STAGE_COUNT={result['stage_count']}")
        for stage in result["stages"]:
            print(f"SMART_TEMPO_ARCH_STAGE={stage['id']} owner={stage['owner']} "
                  f"line={stage['line']} phase={stage['phase']}")
        g = result["default_geometry_180s"]
        print("SMART_TEMPO_THEORETICAL_WINDOW_SECONDS="
              f"{g['requested_decode_window_seconds']:.0f}")
        print(f"SMART_TEMPO_THEORETICAL_UNIQUE_SECONDS="
              f"{g['unique_interval_union_seconds']:.3f}")
        print(f"SMART_TEMPO_THEORETICAL_REPETITION_FACTOR="
              f"{g['estimated_repeat_factor']:.4f}")
        print("SMART_TEMPO_MUSICAL_QUALITY_NOT_INFERRED=1")


if __name__ == "__main__":
    main()
