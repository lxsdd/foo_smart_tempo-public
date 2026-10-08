// SPDX-License-Identifier: GPL-2.0-or-later
// Algorithmic port based on Audacity lib-music-information-retrieval
// (Matthieu Hodgkinson). See docs/reports/
// v21_hodgkinson_tatum_full_mir_port_20260530.md for provenance.

#pragma once

#include "hodgkinson_tatum_probe.h"

#include <array>
#include <span>
#include <vector>

namespace smart_tempo {

// Port of Matthieu Hodgkinson's Audacity MIR tempo chain used by the
// production measured-candidate engine.
// It produces measured segment candidates; terminal selection remains in the
// shared measured-candidate policy pipeline.
struct HodgkinsonFullMirSegment {
  std::span<const float> mono_samples;
  double sample_rate = 0.0;
  double segment_start_sec = 0.0;
  std::size_t segment_index = 0;
  std::size_t segment_count = 0;
};

struct HodgkinsonFullMirSegmentCandidate {
  bool enabled = false;
  bool winner = false;
  const char* frontend_source = "audacity_mir_full";
  double segment_start_sec = 0.0;
  double segment_duration_sec = 0.0;
  std::size_t segment_index = 0;
  std::size_t segment_count = 0;
  std::size_t rank = 0;
  double candidate_bpm = 0.0;
  double combined_score = 0.0;
  double quantization_score = 0.0;
  double meter_score = 0.0;
  double quantization_error = 0.0;
  std::size_t tatum_count = 0;
  std::size_t num_bars = 0;
  std::size_t beats_per_bar = 0;
  std::size_t total_beats = 0;
  std::size_t trailing_beats = 0;
  const char* meter = "unknown";
  std::size_t onset_count = 0;
  std::size_t odf_peak_count = 0;
  const char* reason = "not_evaluated";
};

struct HodgkinsonFullMirSegmentEvaluation {
  HodgkinsonTatumProbeResult result;
  std::array<HodgkinsonFullMirSegmentCandidate, 5> top_k_candidates{};
  std::size_t top_k_count = 0;
  std::array<HodgkinsonFullMirSegmentCandidate, 64>
      partial_bar_candidates{};
  std::size_t partial_bar_count = 0;
};

// Track-level evidence derived only from the partial-bar candidate frontend.
// Selection remains in the canonical FullBoard policy; this structure never
// synthesizes a BPM or mutates another candidate board.
struct HodgkinsonPartialBarAggregateShadow {
  bool enabled = false;
  bool candidate = false;
  double coarse_bpm = 0.0;
  double local_exact_bpm = 0.0;
  double local_exact_score = 0.0;
  double score_sum = 0.0;
  double runner_up_bpm = 0.0;
  double runner_up_score_sum = 0.0;
  double runner_up_score_ratio = 0.0;
  std::size_t rank1_hits = 0;
  std::size_t top5_hits = 0;
  std::size_t segment_count = 0;
  std::size_t total_segment_count = 0;
  std::size_t row_count = 0;
  double mean_quantization_score = 0.0;
  double mean_meter_score = 0.0;
  const char* reason = "not_evaluated";
};

// Exact refinement of one audio-ranked partial-bar family.
// The shared policy may use these measured rows only through its validated
// partial-bar release gates; the rows never create a synthetic BPM.
struct HodgkinsonPartialBarTopKExactCandidate {
  std::size_t family_rank = 0;
  std::size_t family_count = 0;
  double coarse_bpm = 0.0;
  double local_exact_bpm = 0.0;
  double local_exact_score = 0.0;
  double score_sum = 0.0;
  double score_ratio_to_top = 0.0;
  std::size_t rank1_hits = 0;
  std::size_t top5_hits = 0;
  std::size_t segment_count = 0;
  std::size_t total_segment_count = 0;
  std::size_t row_count = 0;
  double mean_quantization_score = 0.0;
  double mean_meter_score = 0.0;
  double fullboard_bpm = 0.0;
  double ratio_to_fullboard = 0.0;
  const char* reason = "not_evaluated";
};

[[nodiscard]] HodgkinsonTatumProbeResult
evaluate_hodgkinson_full_mir_segment(
    const HodgkinsonFullMirSegment& segment) noexcept;

[[nodiscard]] HodgkinsonFullMirSegmentEvaluation
evaluate_hodgkinson_full_mir_segment_with_candidates(
    const HodgkinsonFullMirSegment& segment) noexcept;

// Edge-tolerant candidate board. The production result and
// complete-bar Top-K board are identical to the function above; partial-bar
// rows are emitted separately and are never promoted here.
[[nodiscard]] HodgkinsonFullMirSegmentEvaluation
evaluate_hodgkinson_full_mir_segment_with_partial_bar_candidates(
    const HodgkinsonFullMirSegment& segment) noexcept;

[[nodiscard]] HodgkinsonTatumProbeResult
aggregate_hodgkinson_full_mir_results(
    const char* frontendSource,
    double referenceBpm,
    std::span<const HodgkinsonTatumProbeResult> segmentResults) noexcept;

// Fully independent MIR-only aggregate. Unlike
// aggregate_hodgkinson_full_mir_results(), this does not normalize candidate
// families around the current Smart Tempo final BPM.
[[nodiscard]] HodgkinsonTatumProbeResult
aggregate_hodgkinson_full_mir_only_results(
    const char* frontendSource,
    std::span<const HodgkinsonTatumProbeResult> segmentResults) noexcept;

}  // namespace smart_tempo
