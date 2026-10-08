#include "stdafx.h"

#include "modern_bpm_analyzer.h"

#include "analysis_shared_types.h"
#include "analysis_audio_features.h"
#include "analysis_math_utils.h"
#include "analysis_onset_events.h"
#include "analysis_pass_schedule.h"
#include "analysis_progress.h"
#include "analysis_telemetry.h"
#include "analysis_window_config.h"
#include "aligned_vector.h"
#include "experimental_feature_flags.h"
#include "analysis_decision_support.h"
#include "foo_smart_tempo.h"
#include "hodgkinson_full_mir.h"
#include "mir_candidate_pipeline.h"
#include "hodgkinson_tatum_probe.h"
#include "preferences.h"
#include "smart_tempo_helpers.h"
#include "smart_tempo_mapper.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>


using smpl_t = float;
using uint_t = unsigned int;

namespace {
using namespace smart_tempo::mir_pipeline;
constexpr double kMirProductionMinBpm = 40.0;
constexpr double kMirProductionMaxBpm = 220.0;
constexpr double kMirProductionMinIoiSeconds =
    0.85 * 60.0 / (kMirProductionMaxBpm * 8.0);
constexpr unsigned kMirProductionSampleRateCapHz = 48000;
constexpr const char* kMirPolicySchema = "mir_hodgkinson_policy_v1";
constexpr uint64_t kFnv1aOffset = UINT64_C(14695981039346656037);
constexpr uint64_t kFnv1aPrime = UINT64_C(1099511628211);

[[nodiscard]] constexpr uint64_t fnv1a_literal(const char* text) noexcept {
  uint64_t hash = kFnv1aOffset;
  while (*text != '\0') {
    hash ^= static_cast<unsigned char>(*text++);
    hash *= kFnv1aPrime;
  }
  return hash;
}

constexpr uint64_t kMirBuildId = fnv1a_literal(__DATE__ "T" __TIME__);

[[nodiscard]] uint64_t stable_track_key(metadb_handle_ptr track) noexcept {
  uint64_t hash = kFnv1aOffset;
  if (!track.is_valid()) {
    return hash;
  }
  const unsigned char* path =
      reinterpret_cast<const unsigned char*>(track->get_path());
  while (*path != 0) {
    unsigned char value = *path++;
    if (value == '\\') {
      value = '/';
    } else if (value >= 'A' && value <= 'Z') {
      value = static_cast<unsigned char>(value + ('a' - 'A'));
    }
    hash ^= value;
    hash *= kFnv1aPrime;
  }
  const uint32_t subsong = track->get_subsong_index();
  for (unsigned shift = 0; shift < 32; shift += 8) {
    hash ^= static_cast<unsigned char>((subsong >> shift) & 0xffu);
    hash *= kFnv1aPrime;
  }
  return hash;
}

using smart_tempo::PolicyProjectionReason;
using smart_tempo::aligned_vector;
using smart_tempo::AnalyzeMetricsScope;
using smart_tempo::initial_confidence_for_mode;
using smart_tempo::kAnalysisOffsetMaxPct;
using smart_tempo::kAnalysisOffsetMinPct;
using smart_tempo::kDecodeSafetyMarginSeconds;
using smart_tempo::kMinimumTrackLengthSeconds;
using smart_tempo::kPassEndSafetyMarginSeconds;

struct AliasSectionMetrics {
  double section_support = 0.0;
  double section_stability = 0.0;
};

[[nodiscard]] AliasSectionMetrics compute_alias_section_metrics(
    std::span<const uint64_t> sortedPeaks,
    std::span<const smart_tempo::HodgkinsonTatumProbeResult> segments,
    double aliasBpm,
    double sampleRate);

struct MaterialRiskBucket {
  double bpm = 0.0;
  std::vector<uint8_t> segments;
  std::vector<uint8_t> winner_segments;
};

struct MaterialRiskAccumulator {
  std::array<MaterialRiskBucket, 6> buckets{};
  std::size_t segment_count = 0;
};

struct PartialBarAggregateCandidate {
  double bpm = 0.0;
  double score_sum = 0.0;
  double quantization_sum = 0.0;
  double meter_sum = 0.0;
  std::size_t row_count = 0;
  std::size_t rank1_hits = 0;
  std::size_t top5_hits = 0;
  std::vector<uint8_t> segments;
};

struct PartialBarAggregateAccumulator {
  std::vector<PartialBarAggregateCandidate> candidates;
  std::size_t segment_count = 0;
};

void initialize_partial_bar_aggregate_accumulator(
    PartialBarAggregateAccumulator& out, std::size_t segmentCount) {
  out.segment_count = segmentCount;
  out.candidates.clear();
  out.candidates.reserve(64);
}

void update_partial_bar_aggregate_accumulator(
    PartialBarAggregateAccumulator& accumulator,
    const smart_tempo::HodgkinsonFullMirSegmentEvaluation& evaluation) {
  if (accumulator.segment_count == 0) {
    return;
  }
  for (std::size_t index = 0; index < evaluation.partial_bar_count; ++index) {
    const auto& row = evaluation.partial_bar_candidates[index];
    if (!(row.candidate_bpm > 0.0) || !std::isfinite(row.candidate_bpm) ||
        row.segment_index >= accumulator.segment_count) {
      continue;
    }
    auto candidate = std::find_if(
        accumulator.candidates.begin(), accumulator.candidates.end(),
        [&](const PartialBarAggregateCandidate& item) {
          return std::abs(item.bpm - row.candidate_bpm) < 0.001;
        });
    if (candidate == accumulator.candidates.end()) {
      PartialBarAggregateCandidate item;
      item.bpm = row.candidate_bpm;
      item.segments.assign(accumulator.segment_count, uint8_t{0});
      accumulator.candidates.push_back(std::move(item));
      candidate = std::prev(accumulator.candidates.end());
    }
    candidate->score_sum += row.combined_score;
    candidate->quantization_sum += row.quantization_score;
    candidate->meter_sum += row.meter_score;
    ++candidate->row_count;
    candidate->rank1_hits += row.rank == 1 ? 1 : 0;
    candidate->top5_hits += row.rank <= 5 ? 1 : 0;
    candidate->segments[row.segment_index] = 1;
  }
}

[[nodiscard]] smart_tempo::HodgkinsonPartialBarAggregateShadow
compute_partial_bar_aggregate_shadow(
    const PartialBarAggregateAccumulator& accumulator,
    MirLocalExactCache& localExactCache,
    bool enabled) {
  smart_tempo::HodgkinsonPartialBarAggregateShadow shadow;
  shadow.enabled = enabled;
  shadow.total_segment_count = accumulator.segment_count;
  if (!shadow.enabled || accumulator.candidates.empty()) {
    shadow.reason = shadow.enabled ? "no_partial_bar_candidates" : "disabled";
    return shadow;
  }

  std::vector<const PartialBarAggregateCandidate*> ranked;
  ranked.reserve(accumulator.candidates.size());
  for (const auto& candidate : accumulator.candidates) {
    ranked.push_back(&candidate);
  }
  std::sort(ranked.begin(), ranked.end(),
            [](const PartialBarAggregateCandidate* lhs,
               const PartialBarAggregateCandidate* rhs) {
              if (lhs->score_sum != rhs->score_sum) {
                return lhs->score_sum > rhs->score_sum;
              }
              return lhs->bpm < rhs->bpm;
            });

  const auto& selected = *ranked.front();
  shadow.candidate = true;
  shadow.coarse_bpm = selected.bpm;
  shadow.score_sum = selected.score_sum;
  shadow.rank1_hits = selected.rank1_hits;
  shadow.top5_hits = selected.top5_hits;
  shadow.segment_count = static_cast<std::size_t>(std::count(
      selected.segments.begin(), selected.segments.end(), uint8_t{1}));
  shadow.row_count = selected.row_count;
  if (selected.row_count > 0) {
    const double rows = static_cast<double>(selected.row_count);
    shadow.mean_quantization_score = selected.quantization_sum / rows;
    shadow.mean_meter_score = selected.meter_sum / rows;
  }
  if (ranked.size() > 1) {
    shadow.runner_up_bpm = ranked[1]->bpm;
    shadow.runner_up_score_sum = ranked[1]->score_sum;
    shadow.runner_up_score_ratio =
        selected.score_sum > 0.0
            ? shadow.runner_up_score_sum / selected.score_sum
            : 0.0;
  }

  // Exact scoring is deterministic. Reusing the track-local cache avoids
  // rescanning overlapping BPM points without changing the measured result.
  const MirLocalExactBpm exact = localExactCache.measure(selected.bpm);
  shadow.local_exact_bpm = exact.bpm;
  shadow.local_exact_score = exact.score;
  shadow.reason = exact.bpm > 0.0 ? "partial_bar_top1_local_exact_shadow"
                                 : "partial_bar_top1_exact_unavailable";
  return shadow;
}

[[nodiscard]] bool has_partial_bar_cross_view_coarse_family(
    const PartialBarAggregateAccumulator& accumulator,
    const smart_tempo::MirPolicyDecision& decision,
    std::span<const MirPolicyMeasuredCandidate> fullBoardCandidates,
    bool enabled) noexcept {
  if (!enabled || accumulator.segment_count == 0 ||
      accumulator.candidates.empty() ||
      !has_mir_policy_partial_bar_cross_view_candidate(
          decision, fullBoardCandidates, true)) {
    return false;
  }
  double topScore = 0.0;
  for (const auto& partial : accumulator.candidates) {
    if (std::isfinite(partial.score_sum)) {
      topScore = std::max(topScore, partial.score_sum);
    }
  }
  if (!(topScore > 0.0)) {
    return false;
  }
  const double totalSegments =
      static_cast<double>(accumulator.segment_count);
  for (const auto& fullBoard : fullBoardCandidates) {
    if (!is_mir_policy_partial_bar_cross_view_candidate(decision, fullBoard)) {
      continue;
    }
    for (const auto& partial : accumulator.candidates) {
      if (!std::isfinite(partial.bpm) ||
          !std::isfinite(partial.score_sum) ||
          partial.score_sum / topScore < kMirPolicyCrossViewMinScoreRatio) {
        continue;
      }
      const double top5Support =
          static_cast<double>(partial.top5_hits) / totalSegments;
      const double segmentSupport =
          static_cast<double>(std::count(partial.segments.begin(),
                                         partial.segments.end(), uint8_t{1})) /
          totalSegments;
      if (top5Support >= kMirPolicyCrossViewMinTop5Support &&
          segmentSupport >= kMirPolicyCrossViewMinSegmentSupport &&
          std::abs(fullBoard.bpm - partial.bpm) <=
              kMirPolicyCrossViewMaxCoarseBridgeBpm) {
        return true;
      }
    }
  }
  return false;
}

[[nodiscard]] std::vector<
    smart_tempo::HodgkinsonPartialBarTopKExactCandidate>
compute_partial_bar_topk_exact_telemetry(
    const PartialBarAggregateAccumulator& accumulator,
    MirLocalExactCache& localExactCache, double fullboardBpm,
    std::size_t limit) {
  std::vector<const PartialBarAggregateCandidate*> ranked;
  ranked.reserve(accumulator.candidates.size());
  for (const auto& candidate : accumulator.candidates) {
    ranked.push_back(&candidate);
  }
  std::sort(ranked.begin(), ranked.end(),
            [](const PartialBarAggregateCandidate* lhs,
               const PartialBarAggregateCandidate* rhs) {
              if (lhs->score_sum != rhs->score_sum) {
                return lhs->score_sum > rhs->score_sum;
              }
              return lhs->bpm < rhs->bpm;
            });

  const std::size_t rowCount = std::min(limit, ranked.size());
  std::vector<smart_tempo::HodgkinsonPartialBarTopKExactCandidate> rows;
  rows.reserve(rowCount);
  const double topScore = ranked.empty() ? 0.0 : ranked.front()->score_sum;
  for (std::size_t index = 0; index < rowCount; ++index) {
    const auto& source = *ranked[index];
    const MirLocalExactBpm exact = localExactCache.measure(source.bpm);
    smart_tempo::HodgkinsonPartialBarTopKExactCandidate row;
    row.family_rank = index + 1;
    row.family_count = ranked.size();
    row.coarse_bpm = source.bpm;
    row.local_exact_bpm = exact.bpm;
    row.local_exact_score = exact.score;
    row.score_sum = source.score_sum;
    row.score_ratio_to_top = topScore > 0.0 ? source.score_sum / topScore : 0.0;
    row.rank1_hits = source.rank1_hits;
    row.top5_hits = source.top5_hits;
    row.segment_count = static_cast<std::size_t>(std::count(
        source.segments.begin(), source.segments.end(), uint8_t{1}));
    row.total_segment_count = accumulator.segment_count;
    row.row_count = source.row_count;
    if (source.row_count > 0) {
      const double sourceRows = static_cast<double>(source.row_count);
      row.mean_quantization_score = source.quantization_sum / sourceRows;
      row.mean_meter_score = source.meter_sum / sourceRows;
    }
    row.fullboard_bpm = fullboardBpm;
    row.ratio_to_fullboard =
        fullboardBpm > 0.0 && exact.bpm > 0.0 ? exact.bpm / fullboardBpm
                                              : 0.0;
    row.reason = exact.bpm > 0.0 ? "family_conflict_topk_local_exact"
                                : "family_conflict_topk_exact_unavailable";
    rows.push_back(row);
  }
  return rows;
}

void initialize_material_risk_accumulator(MaterialRiskAccumulator& out,
                                          std::size_t segmentCount) {
  constexpr std::array<double, 6> kTargets{{108.0, 120.0, 132.0,
                                            135.0, 136.0, 144.0}};
  out.segment_count = segmentCount;
  for (std::size_t i = 0; i < out.buckets.size(); ++i) {
    out.buckets[i].bpm = kTargets[i];
    out.buckets[i].segments.assign(segmentCount, uint8_t{0});
    out.buckets[i].winner_segments.assign(segmentCount, uint8_t{0});
  }
}

[[nodiscard]] int material_risk_bucket_index(double clusteredBpm) noexcept {
  constexpr std::array<double, 6> kTargets{{108.0, 120.0, 132.0,
                                            135.0, 136.0, 144.0}};
  for (std::size_t i = 0; i < kTargets.size(); ++i) {
    if (std::abs(clusteredBpm - kTargets[i]) < 0.001) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

void update_material_risk_accumulator(
    MaterialRiskAccumulator& accumulator,
    const smart_tempo::HodgkinsonFullMirSegmentEvaluation& evaluation) {
  if (accumulator.segment_count == 0 || evaluation.top_k_count == 0) {
    return;
  }
  constexpr std::array<double, 11> kAliasMultipliers{{
      1.0, 0.5, 2.0, 2.0 / 3.0, 1.5, 0.75, 4.0 / 3.0,
      1.0 / 3.0, 3.0, 0.25, 4.0,
  }};
  for (std::size_t topKIndex = 0; topKIndex < evaluation.top_k_count;
       ++topKIndex) {
    const auto& candidate = evaluation.top_k_candidates[topKIndex];
    if (!(candidate.candidate_bpm > 0.0) ||
        !std::isfinite(candidate.candidate_bpm) ||
        candidate.segment_index >= accumulator.segment_count) {
      continue;
    }
    for (const double multiplier : kAliasMultipliers) {
      const double aliasBpm =
          cluster_material_bpm(candidate.candidate_bpm * multiplier);
      const int bucketIndex = material_risk_bucket_index(aliasBpm);
      if (bucketIndex < 0) {
        continue;
      }
      auto& bucket =
          accumulator.buckets[static_cast<std::size_t>(bucketIndex)];
      bucket.segments[candidate.segment_index] = 1;
      if (candidate.winner) {
        bucket.winner_segments[candidate.segment_index] = 1;
      }
    }
  }
}

[[nodiscard]] double segment_support(const std::vector<uint8_t>& flags,
                                     std::size_t segmentCount) noexcept {
  if (segmentCount == 0 || flags.empty()) {
    return 0.0;
  }
  const auto count = static_cast<std::size_t>(
      std::count(flags.begin(), flags.end(), uint8_t{1}));
  return static_cast<double>(count) / static_cast<double>(segmentCount);
}

[[nodiscard]] double material_support(const MaterialRiskAccumulator& acc,
                                      double bpm) noexcept {
  const int index = material_risk_bucket_index(cluster_material_bpm(bpm));
  if (index < 0) {
    return 0.0;
  }
  const auto& bucket = acc.buckets[static_cast<std::size_t>(index)];
  return segment_support(bucket.segments, acc.segment_count);
}

[[nodiscard]] double material_winner_support(
    const MaterialRiskAccumulator& acc, double bpm) noexcept {
  const int index = material_risk_bucket_index(cluster_material_bpm(bpm));
  if (index < 0) {
    return 0.0;
  }
  const auto& bucket = acc.buckets[static_cast<std::size_t>(index)];
  return segment_support(bucket.winner_segments, acc.segment_count);
}

[[nodiscard]] smart_tempo::HodgkinsonMaterialRiskEvidence
compute_hodgkinson_material_risk_evidence(
    const MaterialRiskAccumulator& acc,
    const smart_tempo::HodgkinsonOnlyFamilyResolverShadow& resolver,
    double routeMinBpm,
    double routeMaxBpm,
    bool routeMatched,
    bool genericRoute) noexcept {
  smart_tempo::HodgkinsonMaterialRiskEvidence evidence;
  evidence.enabled =
      smart_tempo::experimental::kEnableHodgkinsonMaterialRiskEvidence;
  evidence.route_min_bpm = routeMinBpm;
  evidence.route_max_bpm = routeMaxBpm;
  evidence.route_center_bpm = (routeMinBpm + routeMaxBpm) * 0.5;
  evidence.route_matched = routeMatched;
  evidence.generic_route = genericRoute;
  evidence.segment_count = acc.segment_count;
  evidence.route_valid =
      routeMatched && !genericRoute && routeMinBpm > 0.0 &&
      routeMaxBpm > routeMinBpm &&
      evidence.route_center_bpm >= 124.0 && evidence.route_center_bpm <= 154.0;
  evidence.support_108 = material_support(acc, 108.0);
  evidence.support_120 = material_support(acc, 120.0);
  evidence.support_132 = material_support(acc, 132.0);
  evidence.support_135 = material_support(acc, 135.0);
  evidence.support_136 = material_support(acc, 136.0);
  evidence.support_144 = material_support(acc, 144.0);
  evidence.winner_support_144 = material_winner_support(acc, 144.0);
  evidence.selected_context_bpm =
      resolver.unique_alias_bpm > 0.0 ? resolver.unique_alias_bpm
                                      : resolver.center_alias_bpm;
  if (evidence.route_valid &&
      !(evidence.selected_context_bpm >= 143.0 &&
        evidence.selected_context_bpm <= 145.0) &&
      evidence.support_144 >= 0.02 && routeMinBpm <= 144.0 &&
      routeMaxBpm >= 144.0) {
    // Midway-like material risk can be visible in the segment Top-K buckets
    // even when the family resolver has no unique route alias. Keep this as
    // observability only; it must not promote a BPM.
    evidence.selected_context_bpm = 144.0;
  }
  evidence.candidate =
      evidence.enabled && evidence.route_valid &&
      evidence.selected_context_bpm >= 143.0 &&
      evidence.selected_context_bpm <= 145.0;
  evidence.review_hold_candidate =
      evidence.candidate && evidence.support_120 >= 0.55 &&
      evidence.support_108 >= 0.45 && evidence.support_132 >= 0.20 &&
      evidence.support_144 >= 0.55 && evidence.winner_support_144 <= 0.17 &&
      (std::max)(evidence.support_135, evidence.support_136) <= 0.05;
  evidence.decision_class =
      evidence.review_hold_candidate
          ? "midway_like_material_risk_review_hold"
          : (evidence.candidate ? "selected_144_context" : "not_144_context");
  evidence.reason =
      evidence.review_hold_candidate
          ? "strong_harmonic_material_weak_lower_family"
          : (evidence.candidate ? "selected_144_without_midway_signature"
                                : "outside_midway_review_context");
  return evidence;
}

struct ContinuousRefinementScanPoint {
  double bpm = 0.0;
  double score = 0.0;
  AliasPhaseMetrics phase;
};

void add_distinct_refinement_peak(
    std::array<ContinuousRefinementScanPoint, 3>& peaks,
    const ContinuousRefinementScanPoint& candidate) noexcept {
  constexpr double kMinPeakDistanceBpm = 0.20;
  if (!(candidate.bpm > 0.0) || !std::isfinite(candidate.bpm) ||
      !std::isfinite(candidate.score)) {
    return;
  }

  for (ContinuousRefinementScanPoint& peak : peaks) {
    if (peak.bpm > 0.0 &&
        std::abs(peak.bpm - candidate.bpm) < kMinPeakDistanceBpm) {
      if (candidate.score > peak.score) {
        peak = candidate;
      }
      return;
    }
  }

  auto weakest = std::min_element(
      peaks.begin(), peaks.end(),
      [](const ContinuousRefinementScanPoint& a,
         const ContinuousRefinementScanPoint& b) {
        return a.score < b.score;
      });
  if (weakest != peaks.end() &&
      (weakest->bpm <= 0.0 || candidate.score > weakest->score)) {
    *weakest = candidate;
  }
}

[[nodiscard]] std::array<ContinuousRefinementScanPoint, 3>
sort_refinement_peaks(
    std::array<ContinuousRefinementScanPoint, 3> peaks) noexcept {
  std::sort(peaks.begin(), peaks.end(),
            [](const ContinuousRefinementScanPoint& a,
               const ContinuousRefinementScanPoint& b) {
              if ((a.bpm > 0.0) != (b.bpm > 0.0)) {
                return a.bpm > 0.0;
              }
              return a.score > b.score;
            });
  return peaks;
}

[[nodiscard]] smart_tempo::HodgkinsonContinuousRefinementEvidence
compute_hodgkinson_continuous_refinement_evidence(
    const smart_tempo::HodgkinsonTatumProbeResult& familyResult,
    const smart_tempo::HodgkinsonFamilyAliasOption& alias,
    std::size_t familyRank,
    std::span<const uint64_t> sortedMirPeaks,
    std::span<const smart_tempo::HodgkinsonTatumProbeResult> segments,
    double sampleRate) {
  smart_tempo::HodgkinsonContinuousRefinementEvidence evidence;
  evidence.enabled =
      smart_tempo::experimental::kEnableHodgkinsonContinuousRefinementEvidence ||
      smart_tempo::experimental::kEnableMirPrimaryTelemetry;
  evidence.route_valid = true;
  evidence.family_rank = familyRank;
  evidence.alias_bpm = alias.bpm;
  evidence.alias_multiplier = alias.multiplier;
  evidence.alias_class = alias.multiplier_class;
  evidence.odf_peak_count = sortedMirPeaks.size();
  evidence.accepted_segment_count = segments.size();
  evidence.scan_step_bpm = 0.25;
  evidence.scan_start_bpm = (std::max)(30.0, alias.bpm - 6.0);
  evidence.scan_end_bpm = alias.bpm + 6.0;

  if (familyRank >= 1 &&
      familyRank <= familyResult.family_rank_bpm.size()) {
    const std::size_t index = familyRank - 1;
    evidence.family_bpm = familyResult.family_rank_bpm[index];
    evidence.family_score = familyResult.family_rank_score[index];
    evidence.family_support = familyResult.family_rank_support[index];
  }

  if (!evidence.enabled) {
    evidence.reason = "disabled";
    return evidence;
  }
  if (sortedMirPeaks.size() < 2 || !(alias.bpm > 0.0) ||
      !std::isfinite(alias.bpm) || !(sampleRate > 0.0) ||
      !std::isfinite(sampleRate)) {
    evidence.reason = "insufficient_input";
    return evidence;
  }

  const AliasPhaseMetrics basePhase =
      compute_alias_phase_metrics(sortedMirPeaks, alias.bpm, sampleRate);
  evidence.base_score = compute_continuous_refinement_score(basePhase);

  std::array<ContinuousRefinementScanPoint, 3> peaks{};
  for (double bpm = evidence.scan_start_bpm;
       bpm <= evidence.scan_end_bpm + (evidence.scan_step_bpm * 0.5);
       bpm += evidence.scan_step_bpm) {
    const AliasPhaseMetrics phase =
        compute_alias_phase_metrics(sortedMirPeaks, bpm, sampleRate);
    ContinuousRefinementScanPoint point;
    point.bpm = bpm;
    point.score = compute_continuous_refinement_score(phase);
    point.phase = phase;
    ++evidence.scan_count;
    add_distinct_refinement_peak(peaks, point);
  }

  peaks = sort_refinement_peaks(peaks);
  const ContinuousRefinementScanPoint& best = peaks[0];
  const ContinuousRefinementScanPoint& runnerUp = peaks[1];
  evidence.best_bpm = best.bpm;
  evidence.best_score = best.score;
  evidence.best_phase_vs = best.phase.phase_vs;
  evidence.best_axial_phase_vs = best.phase.axial_phase_vs;
  evidence.best_density = best.phase.normalized_onset_density;
  evidence.runner_up_bpm = runnerUp.bpm;
  evidence.runner_up_score = runnerUp.score;
  evidence.peak_separation_bpm =
      (best.bpm > 0.0 && runnerUp.bpm > 0.0)
          ? std::abs(best.bpm - runnerUp.bpm)
          : 0.0;
  evidence.score_delta = evidence.best_score - evidence.base_score;
  evidence.score_ratio =
      evidence.base_score > 0.0 ? evidence.best_score / evidence.base_score
                                : 0.0;
  evidence.peak1_bpm = peaks[0].bpm;
  evidence.peak1_score = peaks[0].score;
  evidence.peak2_bpm = peaks[1].bpm;
  evidence.peak2_score = peaks[1].score;
  evidence.peak3_bpm = peaks[2].bpm;
  evidence.peak3_score = peaks[2].score;

  const AliasSectionMetrics sectionMetrics =
      compute_alias_section_metrics(sortedMirPeaks, segments, evidence.best_bpm,
                                    sampleRate);
  evidence.section_support = sectionMetrics.section_support;
  evidence.section_stability = sectionMetrics.section_stability;
  evidence.reason = evidence.scan_count > 0 ? "mir_local_phase_scan"
                                            : "empty_scan";
  return evidence;
}

void refine_hodgkinson_continuous_evidence_exact(
    smart_tempo::HodgkinsonContinuousRefinementEvidence& evidence,
    std::span<const uint64_t> sortedMirPeaks,
    double sampleRate) {
  if (sortedMirPeaks.size() < 2 || !(evidence.best_bpm > 0.0) ||
      !std::isfinite(evidence.best_bpm) || !(sampleRate > 0.0) ||
      !std::isfinite(sampleRate)) {
    return;
  }

  constexpr double kFineHalfWindowBpm = 0.50;
  constexpr double kFineStepBpm = 0.01;
  evidence.fine_scan_start_bpm =
      (std::max)(30.0, evidence.best_bpm - kFineHalfWindowBpm);
  evidence.fine_scan_end_bpm = evidence.best_bpm + kFineHalfWindowBpm;
  evidence.fine_scan_step_bpm = kFineStepBpm;

  double bestBpm = evidence.best_bpm;
  double bestScore = -std::numeric_limits<double>::infinity();
  const std::size_t stepCount = static_cast<std::size_t>(std::llround(
      (evidence.fine_scan_end_bpm - evidence.fine_scan_start_bpm) /
      kFineStepBpm));
  for (std::size_t index = 0; index <= stepCount; ++index) {
    const double bpm = evidence.fine_scan_start_bpm +
                       static_cast<double>(index) * kFineStepBpm;
    const AliasPhaseMetrics phase =
        compute_alias_phase_metrics(sortedMirPeaks, bpm, sampleRate);
    const double score = compute_continuous_refinement_score(phase);
    ++evidence.fine_scan_count;
    if (score > bestScore) {
      bestScore = score;
      bestBpm = bpm;
    }
  }

  evidence.fine_best_bpm = bestBpm;
  evidence.fine_best_score = bestScore;
  evidence.fine_score_delta = bestScore - evidence.best_score;
  evidence.fine_score_ratio = evidence.best_score > 0.0
                                 ? bestScore / evidence.best_score
                                 : 0.0;
}



[[nodiscard]] double compute_interval_recurrence_score(
    std::span<const uint64_t> sortedPeaks,
    double bpm,
    double sampleRate) {
  if (sortedPeaks.size() < 2 || !(bpm > 0.0) || !std::isfinite(bpm) ||
      !(sampleRate > 0.0) || !std::isfinite(sampleRate)) {
    return 0.0;
  }

  const double periodSamples = (60.0 / bpm) * sampleRate;
  if (!(periodSamples > 0.0) || !std::isfinite(periodSamples)) {
    return 0.0;
  }

  const double toleranceSamples =
      (std::max)(0.010 * periodSamples, 0.006 * sampleRate);
  constexpr std::array<int, 3> kMultiples{1, 2, 4};
  double hitWeight = 0.0;
  double totalWeight = 0.0;

  const uint64_t maxPeak = sortedPeaks.back();
  for (const uint64_t peak : sortedPeaks) {
    for (const int multiple : kMultiples) {
      const double target = static_cast<double>(peak) +
                            periodSamples * static_cast<double>(multiple);
      if (target > static_cast<double>(maxPeak) + toleranceSamples) {
        continue;
      }
      const double weight = 1.0 / static_cast<double>(multiple);
      totalWeight += weight;
      const uint64_t targetSample = static_cast<uint64_t>(std::llround(target));
      const auto lower = std::lower_bound(sortedPeaks.begin(), sortedPeaks.end(),
                                          targetSample);
      bool hit = false;
      if (lower != sortedPeaks.end()) {
        const double diff = std::abs(static_cast<double>(*lower) - target);
        hit = diff <= toleranceSamples;
      }
      if (!hit && lower != sortedPeaks.begin()) {
        const auto prev = std::prev(lower);
        const double diff = std::abs(static_cast<double>(*prev) - target);
        hit = diff <= toleranceSamples;
      }
      if (hit) {
        hitWeight += weight;
      }
    }
  }

  return totalWeight > 0.0 ? hitWeight / totalWeight : 0.0;
}

void compute_hodgkinson_interval_recurrence_evidence(
    smart_tempo::HodgkinsonContinuousRefinementEvidence& evidence,
    std::span<const uint64_t> sortedMirPeaks,
    std::span<const smart_tempo::HodgkinsonTatumProbeResult> segments,
    double sampleRate) {
  if (sortedMirPeaks.size() < 2 || !(evidence.scan_start_bpm > 0.0) ||
      evidence.scan_end_bpm < evidence.scan_start_bpm ||
      !(sampleRate > 0.0) || !std::isfinite(sampleRate)) {
    return;
  }

  constexpr double kIntervalStepBpm = 0.05;
  evidence.interval_scan_start_bpm = evidence.scan_start_bpm;
  evidence.interval_scan_end_bpm = evidence.scan_end_bpm;
  evidence.interval_scan_step_bpm = kIntervalStepBpm;

  std::array<ContinuousRefinementScanPoint, 3> peaks{};
  double bestBpm = 0.0;
  double bestScore = -std::numeric_limits<double>::infinity();
  const std::size_t stepCount = static_cast<std::size_t>(std::llround(
      (evidence.interval_scan_end_bpm - evidence.interval_scan_start_bpm) /
      kIntervalStepBpm));
  for (std::size_t index = 0; index <= stepCount; ++index) {
    const double bpm = evidence.interval_scan_start_bpm +
                       static_cast<double>(index) * kIntervalStepBpm;
    const double score =
        compute_interval_recurrence_score(sortedMirPeaks, bpm, sampleRate);
    ++evidence.interval_scan_count;
    ContinuousRefinementScanPoint point;
    point.bpm = bpm;
    point.score = score;
    add_distinct_refinement_peak(peaks, point);
    if (score > bestScore) {
      bestScore = score;
      bestBpm = bpm;
    }
  }

  peaks = sort_refinement_peaks(peaks);
  evidence.interval_best_bpm = bestBpm;
  evidence.interval_best_score = bestScore > 0.0 ? bestScore : 0.0;
  evidence.interval_peak1_bpm = peaks[0].bpm;
  evidence.interval_peak1_score = peaks[0].score;
  evidence.interval_peak2_bpm = peaks[1].bpm;
  evidence.interval_peak2_score = peaks[1].score;
  evidence.interval_peak3_bpm = peaks[2].bpm;
  evidence.interval_peak3_score = peaks[2].score;

  if (evidence.alias_bpm > 0.0 && std::isfinite(evidence.alias_bpm)) {
    evidence.interval_alias_score = compute_interval_recurrence_score(
        sortedMirPeaks, evidence.alias_bpm, sampleRate);
  }
  const double continuousBpm = evidence.fine_best_bpm > 0.0
                                   ? evidence.fine_best_bpm
                                   : evidence.best_bpm;
  if (continuousBpm > 0.0 && std::isfinite(continuousBpm)) {
    evidence.interval_continuous_best_score = compute_interval_recurrence_score(
        sortedMirPeaks, continuousBpm, sampleRate);
  }


  constexpr double kFineIntervalRadiusBpm = 0.50;
  constexpr double kFineIntervalStepBpm = 0.01;
  evidence.interval_fine_scan_start_bpm =
      (std::max)(evidence.interval_scan_start_bpm,
                 bestBpm - kFineIntervalRadiusBpm);
  evidence.interval_fine_scan_end_bpm =
      (std::min)(evidence.interval_scan_end_bpm,
                 bestBpm + kFineIntervalRadiusBpm);
  evidence.interval_fine_scan_step_bpm = kFineIntervalStepBpm;
  double fineBestBpm = bestBpm;
  double fineBestScore = bestScore;
  const std::size_t fineStepCount = static_cast<std::size_t>(std::llround(
      (evidence.interval_fine_scan_end_bpm -
       evidence.interval_fine_scan_start_bpm) /
      kFineIntervalStepBpm));
  for (std::size_t index = 0; index <= fineStepCount; ++index) {
    const double bpm = evidence.interval_fine_scan_start_bpm +
                       static_cast<double>(index) * kFineIntervalStepBpm;
    const double score =
        compute_interval_recurrence_score(sortedMirPeaks, bpm, sampleRate);
    ++evidence.interval_fine_scan_count;
    if (score > fineBestScore) {
      fineBestScore = score;
      fineBestBpm = bpm;
    }
  }
  evidence.interval_fine_best_bpm = fineBestBpm;
  evidence.interval_fine_best_score = fineBestScore > 0.0 ? fineBestScore : 0.0;

  if (!segments.empty() && evidence.interval_fine_scan_count > 0) {
    constexpr std::size_t kMinSegmentPeaks = 4;
    constexpr double kClusterToleranceBpm = 0.50;
    std::vector<double> segmentBestBpms;
    segmentBestBpms.reserve(segments.size());

    for (const auto& segment : segments) {
      if (!(segment.segment_duration_sec > 0.0) ||
          !std::isfinite(segment.segment_duration_sec) ||
          !std::isfinite(segment.segment_start_sec)) {
        continue;
      }
      const uint64_t startSample = static_cast<uint64_t>(std::llround(
          (std::max)(0.0, segment.segment_start_sec * sampleRate)));
      const uint64_t endSample = static_cast<uint64_t>(std::llround(
          (std::max)(0.0, (segment.segment_start_sec +
                           segment.segment_duration_sec) *
                              sampleRate)));
      if (endSample <= startSample) {
        continue;
      }
      const auto lower = std::lower_bound(sortedMirPeaks.begin(),
                                          sortedMirPeaks.end(), startSample);
      const auto upper = std::lower_bound(lower, sortedMirPeaks.end(),
                                          endSample);
      const std::size_t peakCount =
          static_cast<std::size_t>(std::distance(lower, upper));
      if (peakCount < kMinSegmentPeaks) {
        continue;
      }

      const auto segmentPeaks = std::span<const uint64_t>(&(*lower), peakCount);
      double segmentBestBpm = evidence.interval_fine_scan_start_bpm;
      double segmentBestScore = -std::numeric_limits<double>::infinity();
      for (std::size_t index = 0; index < evidence.interval_fine_scan_count;
           ++index) {
        const double bpm = evidence.interval_fine_scan_start_bpm +
                           static_cast<double>(index) *
                               evidence.interval_fine_scan_step_bpm;
        const double score =
            compute_interval_recurrence_score(segmentPeaks, bpm, sampleRate);
        if (score > segmentBestScore) {
          segmentBestScore = score;
          segmentBestBpm = bpm;
        }
      }
      segmentBestBpms.push_back(segmentBestBpm);
    }

    if (!segmentBestBpms.empty()) {
      std::sort(segmentBestBpms.begin(), segmentBestBpms.end());
      const auto quantile = [&segmentBestBpms](double fraction) {
        const double index =
            fraction * static_cast<double>(segmentBestBpms.size() - 1);
        const std::size_t lower = static_cast<std::size_t>(std::floor(index));
        const std::size_t upper = static_cast<std::size_t>(std::ceil(index));
        const double remainder = index - static_cast<double>(lower);
        return segmentBestBpms[lower] * (1.0 - remainder) +
               segmentBestBpms[upper] * remainder;
      };

      evidence.interval_segment_count = segmentBestBpms.size();
      evidence.interval_segment_median_bpm = quantile(0.50);
      evidence.interval_segment_q25_bpm = quantile(0.25);
      evidence.interval_segment_q75_bpm = quantile(0.75);
      evidence.interval_segment_iqr_bpm =
          evidence.interval_segment_q75_bpm - evidence.interval_segment_q25_bpm;
      evidence.interval_segment_min_bpm = segmentBestBpms.front();
      evidence.interval_segment_max_bpm = segmentBestBpms.back();

      std::size_t supportBest = 0;
      std::size_t supportAlias = 0;
      for (const double bpm : segmentBestBpms) {
        if (std::abs(bpm - evidence.interval_fine_best_bpm) <=
            kClusterToleranceBpm) {
          ++supportBest;
        }
        if (evidence.alias_bpm > 0.0 &&
            std::abs(bpm - evidence.alias_bpm) <= kClusterToleranceBpm) {
          ++supportAlias;
        }
      }
      evidence.interval_segment_support_best_050 =
          static_cast<double>(supportBest) /
          static_cast<double>(segmentBestBpms.size());
      evidence.interval_segment_support_alias_050 =
          static_cast<double>(supportAlias) /
          static_cast<double>(segmentBestBpms.size());

      struct Cluster {
        double sum = 0.0;
        std::size_t count = 0;
        double center = 0.0;
      };
      std::vector<Cluster> clusters;
      for (const double bpm : segmentBestBpms) {
        bool merged = false;
        for (Cluster& cluster : clusters) {
          if (std::abs(bpm - cluster.center) <= kClusterToleranceBpm) {
            cluster.sum += bpm;
            ++cluster.count;
            cluster.center = cluster.sum / static_cast<double>(cluster.count);
            merged = true;
            break;
          }
        }
        if (!merged) {
          clusters.push_back(Cluster{bpm, 1, bpm});
        }
      }
      std::sort(clusters.begin(), clusters.end(), [](const Cluster& a,
                                                     const Cluster& b) {
        if (a.count != b.count) {
          return a.count > b.count;
        }
        return a.center < b.center;
      });
      const auto setCluster = [&](std::size_t index, double& bpmOut,
                                  double& supportOut) {
        if (index >= clusters.size()) {
          return;
        }
        bpmOut = clusters[index].center;
        supportOut = static_cast<double>(clusters[index].count) /
                     static_cast<double>(segmentBestBpms.size());
      };
      setCluster(0, evidence.interval_segment_cluster1_bpm,
                 evidence.interval_segment_cluster1_support);
      setCluster(1, evidence.interval_segment_cluster2_bpm,
                 evidence.interval_segment_cluster2_support);
      setCluster(2, evidence.interval_segment_cluster3_bpm,
                 evidence.interval_segment_cluster3_support);
    }
  }
}

void compute_hodgkinson_continuous_range_segment_consensus(
    smart_tempo::HodgkinsonContinuousRefinementEvidence& evidence,
    std::span<const uint64_t> sortedMirPeaks,
    std::span<const smart_tempo::HodgkinsonTatumProbeResult> segments,
    double sampleRate) {
  if (sortedMirPeaks.size() < 2 || segments.empty() ||
      evidence.scan_count == 0 || !(evidence.scan_step_bpm > 0.0) ||
      !(evidence.scan_start_bpm > 0.0) ||
      evidence.scan_end_bpm < evidence.scan_start_bpm ||
      !(sampleRate > 0.0) || !std::isfinite(sampleRate)) {
    return;
  }

  constexpr std::size_t kMinSegmentPeaks = 4;
  constexpr double kClusterToleranceBpm = 0.50;
  std::vector<double> segmentBestBpms;
  segmentBestBpms.reserve(segments.size());

  for (const auto& segment : segments) {
    if (!(segment.segment_duration_sec > 0.0) ||
        !std::isfinite(segment.segment_duration_sec) ||
        !std::isfinite(segment.segment_start_sec)) {
      continue;
    }
    const uint64_t startSample = static_cast<uint64_t>(std::llround(
        (std::max)(0.0, segment.segment_start_sec * sampleRate)));
    const uint64_t endSample = static_cast<uint64_t>(std::llround(
        (std::max)(0.0, (segment.segment_start_sec +
                         segment.segment_duration_sec) *
                            sampleRate)));
    if (endSample <= startSample) {
      continue;
    }

    const auto lower = std::lower_bound(sortedMirPeaks.begin(),
                                        sortedMirPeaks.end(), startSample);
    const auto upper = std::lower_bound(lower, sortedMirPeaks.end(), endSample);
    const std::size_t peakCount =
        static_cast<std::size_t>(std::distance(lower, upper));
    if (peakCount < kMinSegmentPeaks) {
      continue;
    }

    const auto segmentPeaks = std::span<const uint64_t>(&(*lower), peakCount);
    double bestBpm = evidence.scan_start_bpm;
    double bestScore = -std::numeric_limits<double>::infinity();
    for (std::size_t index = 0; index < evidence.scan_count; ++index) {
      const double bpm = evidence.scan_start_bpm +
                         static_cast<double>(index) * evidence.scan_step_bpm;
      const double score = compute_continuous_refinement_score(
          compute_alias_phase_metrics(segmentPeaks, bpm, sampleRate));
      if (score > bestScore) {
        bestScore = score;
        bestBpm = bpm;
      }
    }
    segmentBestBpms.push_back(bestBpm);
  }

  if (segmentBestBpms.empty()) {
    return;
  }

  std::sort(segmentBestBpms.begin(), segmentBestBpms.end());
  const auto quantile = [&segmentBestBpms](double fraction) {
    const double index =
        fraction * static_cast<double>(segmentBestBpms.size() - 1);
    const std::size_t lower = static_cast<std::size_t>(std::floor(index));
    const std::size_t upper = static_cast<std::size_t>(std::ceil(index));
    const double remainder = index - static_cast<double>(lower);
    return segmentBestBpms[lower] * (1.0 - remainder) +
           segmentBestBpms[upper] * remainder;
  };

  evidence.range_segment_count = segmentBestBpms.size();
  evidence.range_segment_median_bpm = quantile(0.50);
  evidence.range_segment_q25_bpm = quantile(0.25);
  evidence.range_segment_q75_bpm = quantile(0.75);
  evidence.range_segment_iqr_bpm = evidence.range_segment_q75_bpm -
                                   evidence.range_segment_q25_bpm;
  evidence.range_segment_min_bpm = segmentBestBpms.front();
  evidence.range_segment_max_bpm = segmentBestBpms.back();

  std::size_t supportBest = 0;
  std::size_t supportAlias = 0;
  for (const double bpm : segmentBestBpms) {
    if (evidence.best_bpm > 0.0 &&
        std::abs(bpm - evidence.best_bpm) <= kClusterToleranceBpm) {
      ++supportBest;
    }
    if (evidence.alias_bpm > 0.0 &&
        std::abs(bpm - evidence.alias_bpm) <= kClusterToleranceBpm) {
      ++supportAlias;
    }
  }
  evidence.range_segment_support_best_050 =
      static_cast<double>(supportBest) /
      static_cast<double>(segmentBestBpms.size());
  evidence.range_segment_support_alias_050 =
      static_cast<double>(supportAlias) /
      static_cast<double>(segmentBestBpms.size());

  struct Cluster {
    double sum = 0.0;
    std::size_t count = 0;
    double center = 0.0;
  };
  std::vector<Cluster> clusters;
  for (const double bpm : segmentBestBpms) {
    bool merged = false;
    for (Cluster& cluster : clusters) {
      if (std::abs(bpm - cluster.center) <= kClusterToleranceBpm) {
        cluster.sum += bpm;
        ++cluster.count;
        cluster.center = cluster.sum / static_cast<double>(cluster.count);
        merged = true;
        break;
      }
    }
    if (!merged) {
      clusters.push_back(Cluster{bpm, 1, bpm});
    }
  }
  std::sort(clusters.begin(), clusters.end(), [](const Cluster& a,
                                                 const Cluster& b) {
    if (a.count != b.count) {
      return a.count > b.count;
    }
    return a.center < b.center;
  });

  const auto setCluster = [&](std::size_t index, double& bpmOut,
                              double& supportOut) {
    if (index >= clusters.size()) {
      return;
    }
    bpmOut = clusters[index].center;
    supportOut = static_cast<double>(clusters[index].count) /
                 static_cast<double>(segmentBestBpms.size());
  };
  setCluster(0, evidence.range_segment_cluster1_bpm,
             evidence.range_segment_cluster1_support);
  setCluster(1, evidence.range_segment_cluster2_bpm,
             evidence.range_segment_cluster2_support);
  setCluster(2, evidence.range_segment_cluster3_bpm,
             evidence.range_segment_cluster3_support);
}

void compute_hodgkinson_continuous_segment_consensus(
    smart_tempo::HodgkinsonContinuousRefinementEvidence& evidence,
    std::span<const uint64_t> sortedMirPeaks,
    std::span<const smart_tempo::HodgkinsonTatumProbeResult> segments,
    double sampleRate) {
  if (sortedMirPeaks.size() < 2 || segments.empty() ||
      evidence.fine_scan_count == 0 || !(evidence.fine_best_bpm > 0.0) ||
      !(evidence.fine_scan_step_bpm > 0.0) ||
      !(sampleRate > 0.0) || !std::isfinite(evidence.fine_best_bpm) ||
      !std::isfinite(sampleRate)) {
    return;
  }

  constexpr std::size_t kMinSegmentPeaks = 4;
  std::vector<double> segmentBestBpms;
  segmentBestBpms.reserve(segments.size());

  for (const auto& segment : segments) {
    if (!(segment.segment_duration_sec > 0.0) ||
        !std::isfinite(segment.segment_duration_sec) ||
        !std::isfinite(segment.segment_start_sec)) {
      continue;
    }
    const uint64_t startSample = static_cast<uint64_t>(std::llround(
        (std::max)(0.0, segment.segment_start_sec * sampleRate)));
    const uint64_t endSample = static_cast<uint64_t>(std::llround(
        (std::max)(0.0, (segment.segment_start_sec +
                         segment.segment_duration_sec) *
                            sampleRate)));
    if (endSample <= startSample) {
      continue;
    }

    const auto lower = std::lower_bound(sortedMirPeaks.begin(),
                                        sortedMirPeaks.end(), startSample);
    const auto upper = std::lower_bound(lower, sortedMirPeaks.end(),
                                        endSample);
    const std::size_t peakCount =
        static_cast<std::size_t>(std::distance(lower, upper));
    if (peakCount < kMinSegmentPeaks) {
      continue;
    }

    const auto segmentPeaks =
        std::span<const uint64_t>(&(*lower), peakCount);
    double bestBpm = evidence.fine_scan_start_bpm;
    double bestScore = -std::numeric_limits<double>::infinity();
    for (std::size_t index = 0; index < evidence.fine_scan_count; ++index) {
      const double bpm = evidence.fine_scan_start_bpm +
                         static_cast<double>(index) *
                             evidence.fine_scan_step_bpm;
      const double score = compute_continuous_refinement_score(
          compute_alias_phase_metrics(segmentPeaks, bpm, sampleRate));
      if (score > bestScore) {
        bestScore = score;
        bestBpm = bpm;
      }
    }
    segmentBestBpms.push_back(bestBpm);
  }

  if (segmentBestBpms.empty()) {
    return;
  }

  std::sort(segmentBestBpms.begin(), segmentBestBpms.end());
  const auto quantile = [&segmentBestBpms](double fraction) {
    const double index =
        fraction * static_cast<double>(segmentBestBpms.size() - 1);
    const std::size_t lower = static_cast<std::size_t>(std::floor(index));
    const std::size_t upper = static_cast<std::size_t>(std::ceil(index));
    const double remainder = index - static_cast<double>(lower);
    return segmentBestBpms[lower] * (1.0 - remainder) +
           segmentBestBpms[upper] * remainder;
  };

  evidence.fine_segment_count = segmentBestBpms.size();
  evidence.fine_segment_median_bpm = quantile(0.50);
  evidence.fine_segment_q25_bpm = quantile(0.25);
  evidence.fine_segment_q75_bpm = quantile(0.75);
  evidence.fine_segment_iqr_bpm = evidence.fine_segment_q75_bpm -
                                  evidence.fine_segment_q25_bpm;
  evidence.fine_segment_min_bpm = segmentBestBpms.front();
  evidence.fine_segment_max_bpm = segmentBestBpms.back();

  std::size_t support005 = 0;
  std::size_t support010 = 0;
  for (const double bpm : segmentBestBpms) {
    const double difference = std::abs(bpm - evidence.fine_best_bpm);
    support005 += difference <= 0.05 ? 1 : 0;
    support010 += difference <= 0.10 ? 1 : 0;
  }
  evidence.fine_segment_support_005 =
      static_cast<double>(support005) /
      static_cast<double>(segmentBestBpms.size());
  evidence.fine_segment_support_010 =
      static_cast<double>(support010) /
      static_cast<double>(segmentBestBpms.size());
}

[[nodiscard]] AliasSectionMetrics compute_alias_section_metrics(
    std::span<const uint64_t> sortedPeaks,
    std::span<const smart_tempo::HodgkinsonTatumProbeResult> segments,
    double aliasBpm,
    double sampleRate) {
  AliasSectionMetrics metrics;
  if (sortedPeaks.size() < 2 || segments.empty() || aliasBpm <= 0.0 ||
      !std::isfinite(aliasBpm) || sampleRate <= 0.0 ||
      !std::isfinite(sampleRate)) {
    return metrics;
  }

  constexpr std::size_t kMinSegmentPeaks = 4;
  constexpr double kSupportScoreThreshold = 0.12;
  std::vector<double> scores;
  scores.reserve(segments.size());

  for (const auto& segment : segments) {
    if (segment.segment_duration_sec <= 0.0 ||
        !std::isfinite(segment.segment_duration_sec) ||
        !std::isfinite(segment.segment_start_sec)) {
      continue;
    }
    const double segmentStartSample =
        (std::max)(0.0, segment.segment_start_sec * sampleRate);
    const double segmentEndSample =
        (std::max)(0.0, (segment.segment_start_sec +
                         segment.segment_duration_sec) *
                            sampleRate);
    const uint64_t startSample =
        static_cast<uint64_t>(std::llround(segmentStartSample));
    const uint64_t endSample =
        static_cast<uint64_t>(std::llround(segmentEndSample));
    if (endSample <= startSample) {
      continue;
    }

    const auto lower = std::lower_bound(sortedPeaks.begin(), sortedPeaks.end(),
                                        startSample);
    const auto upper = std::lower_bound(lower, sortedPeaks.end(), endSample);
    const auto count = static_cast<std::size_t>(std::distance(lower, upper));
    if (count < kMinSegmentPeaks) {
      continue;
    }

    const AliasPhaseMetrics segmentPhase =
        compute_alias_phase_metrics(std::span<const uint64_t>(&(*lower), count),
                                    aliasBpm, sampleRate);
    scores.push_back((std::max)(segmentPhase.phase_vs,
                                segmentPhase.axial_phase_vs));
  }

  if (scores.empty()) {
    return metrics;
  }

  std::size_t supportCount = 0;
  double sum = 0.0;
  for (const double score : scores) {
    sum += score;
    if (score >= kSupportScoreThreshold) {
      ++supportCount;
    }
  }
  const double mean = sum / static_cast<double>(scores.size());
  double variance = 0.0;
  for (const double score : scores) {
    const double delta = score - mean;
    variance += delta * delta;
  }
  variance /= static_cast<double>(scores.size());
  const double stddev = std::sqrt(variance);

  metrics.section_support =
      static_cast<double>(supportCount) / static_cast<double>(scores.size());
  metrics.section_stability =
      mean > 0.0 ? std::clamp(mean / (mean + stddev), 0.0, 1.0) : 0.0;
  return metrics;
}
using smart_tempo::log_analysis_sample_rate;
using smart_tempo::DecisionSummaryLogSnapshot;
using smart_tempo::log_decision_summary;
using smart_tempo::log_standard_decision;
using smart_tempo::log_track_timing;
using smart_tempo::log_telemetry_header;
using smart_tempo::make_track_timing_log;
using smart_tempo::query_runtime_stats_snapshot;
using smart_tempo::runtime_counters;
using smart_tempo::reset_runtime_counters;
using smart_tempo::compute_analysis_pass_offset;
using smart_tempo::make_analysis_window_plan;
using smart_tempo::verbose_console_logging_enabled;

constexpr double kStrongConsensusOutOfRouteMinSupport = 0.80;
constexpr double kStrongConsensusOutOfRouteCandidateRelTolerance = 0.035;
constexpr double kStrongConsensusOutOfRouteCandidateAbsToleranceBpm = 4.0;
constexpr double kStrongConsensusOutOfRouteFinalConflictRel = 0.12;
constexpr double kStrongConsensusOutOfRouteFinalConflictAbsBpm = 8.0;
constexpr double kStrongConsensusOutOfRouteMinBandQuality = 0.70;
constexpr const char* kStrongConsensusOutOfRouteRawCandidateReason =
    "strong-pass-consensus-out-of-route-raw-candidate";
constexpr const char* kStrongConsensusHalfPulseRawCandidateReason =
    "strong-pass-consensus-half-pulse-raw-candidate";

struct MirOdfFrame {
  uint64_t hop_end_sample = 0;
  double energy_flux = 0.0;
  double high_frequency_flux = 0.0;
};

[[nodiscard]] double compute_hop_log_energy_samples(
    std::span<const smpl_t> hop) noexcept {
  if (hop.empty()) {
    return 0.0;
  }
  double energy = 0.0;
  for (const smpl_t value : hop) {
    const double sample = static_cast<double>(value);
    energy += sample * sample;
  }
  energy /= static_cast<double>(hop.size());
  return std::log1p(energy);
}


[[nodiscard]] double compute_hop_log_high_frequency_energy_samples(
    std::span<const smpl_t> hop) noexcept {
  if (hop.size() < 2) {
    return 0.0;
  }
  double energy = 0.0;
  double previous = static_cast<double>(hop.front());
  for (std::size_t i = 1; i < hop.size(); ++i) {
    const double sample = static_cast<double>(hop[i]);
    const double diff = sample - previous;
    energy += diff * diff;
    previous = sample;
  }
  energy /= static_cast<double>(hop.size() - 1);
  return std::log1p(energy);
}


void observe_mir_odf_energy_flux_samples(
    std::vector<MirOdfFrame>& frames,
    std::span<const smpl_t> hop,
    uint64_t hopEndSample,
    double& previousLogEnergy,
    bool& hasPreviousLogEnergy,
    double& previousLogHighFrequencyEnergy,
    bool& hasPreviousLogHighFrequencyEnergy) {
  const double logEnergy = compute_hop_log_energy_samples(hop);
  const double logHighFrequencyEnergy =
      compute_hop_log_high_frequency_energy_samples(hop);
  double energyFlux = 0.0;
  if (hasPreviousLogEnergy) {
    energyFlux = (std::max)(0.0, logEnergy - previousLogEnergy);
  }
  previousLogEnergy = logEnergy;
  hasPreviousLogEnergy = true;
  double highFrequencyFlux = 0.0;
  if (hasPreviousLogHighFrequencyEnergy) {
    highFrequencyFlux =
        (std::max)(0.0, logHighFrequencyEnergy -
                            previousLogHighFrequencyEnergy);
  }
  previousLogHighFrequencyEnergy = logHighFrequencyEnergy;
  hasPreviousLogHighFrequencyEnergy = true;
  frames.push_back(MirOdfFrame{hopEndSample, energyFlux, highFrequencyFlux});
}

enum class MirOdfFluxKind {
  Energy,
  HighFrequency,
};

[[nodiscard]] double mir_odf_flux_value(const MirOdfFrame& frame,
                                        MirOdfFluxKind kind) noexcept {
  switch (kind) {
    case MirOdfFluxKind::Energy:
      return frame.energy_flux;
    case MirOdfFluxKind::HighFrequency:
      return frame.high_frequency_flux;
  }
  return 0.0;
}

void append_mir_odf_peaks_from_frames(
    aligned_vector<uint64_t>& outPeakSamples,
    std::vector<double>& outPeakWeights,
    const std::vector<MirOdfFrame>& frames,
    uint64_t passStartSample,
    uint64_t minSpacingSamples,
    MirOdfFluxKind kind) {
  if (frames.size() < 3) {
    return;
  }

  double positiveSum = 0.0;
  double positiveSqSum = 0.0;
  double maxFlux = 0.0;
  size_t positiveCount = 0;
  for (const auto& frame : frames) {
    const double flux = mir_odf_flux_value(frame, kind);
    if (flux <= 0.0 || !std::isfinite(flux)) {
      continue;
    }
    positiveSum += flux;
    positiveSqSum += flux * flux;
    maxFlux = (std::max)(maxFlux, flux);
    ++positiveCount;
  }
  if (positiveCount == 0 || maxFlux <= 0.0) {
    return;
  }

  const double mean = positiveSum / static_cast<double>(positiveCount);
  const double variance =
      (std::max)(0.0, (positiveSqSum / static_cast<double>(positiveCount)) -
                          (mean * mean));
  const double stddev = std::sqrt(variance);
  const double threshold = (std::max)(mean + (0.35 * stddev), maxFlux * 0.12);
  const uint64_t spacing = (std::max)(uint64_t{1}, minSpacingSamples);

  uint64_t lastAccepted = 0;
  bool hasLastAccepted = false;
  for (size_t i = 1; i + 1 < frames.size(); ++i) {
    const double flux = mir_odf_flux_value(frames[i], kind);
    if (!(flux >= threshold) ||
        flux < mir_odf_flux_value(frames[i - 1], kind) ||
        flux < mir_odf_flux_value(frames[i + 1], kind)) {
      continue;
    }

    const uint64_t absoluteSample = passStartSample + frames[i].hop_end_sample;
    if (hasLastAccepted && absoluteSample <= lastAccepted + spacing) {
      if (flux > outPeakWeights.back()) {
        outPeakSamples.back() = absoluteSample;
        outPeakWeights.back() = flux;
        lastAccepted = absoluteSample;
      }
      continue;
    }
    outPeakSamples.push_back(absoluteSample);
    outPeakWeights.push_back(flux);
    lastAccepted = absoluteSample;
    hasLastAccepted = true;
  }
}

} // namespace

void ModernBpmAnalyzer::WaitWhilePaused(abort_callback& p_abort) {
  if (m_pauseFlag == nullptr) return;
  while (m_pauseFlag->load() && !p_abort.is_aborting()) {
    if (m_pauseCv != nullptr && m_pauseMutex != nullptr) {
      std::unique_lock<std::mutex> lk(*m_pauseMutex);
      m_pauseCv->wait(lk, [this, &p_abort]() {
        return !m_pauseFlag->load() || p_abort.is_aborting();
      });
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
}

double ModernBpmAnalyzer::analyze(metadb_handle_ptr track, threaded_process_status& status,
                                   abort_callback& p_abort, double priorMinBpm,
                                   double priorMaxBpm,
                                   const char* trackIdentifier,
                                   const RoutingLogContext* routingContext,
                                   bool captureMeasuredCandidates) {
  // Fine-grained per-hop/per-chunk timers are intentionally debug-only; they are
  // measurable overhead in large library scans.
  const bool verboseLogs = verbose_console_logging_enabled();
  AnalyzeMetricsScope metrics(verboseLogs);

  m_diag.confidence = initial_confidence_for_mode();
  m_diag.error_reason.reset();
  m_diag.policy_reason.reset();
  m_diag.decision_class.reset();
  m_diag.decision_note.reset();
  m_diag.measured_bpm_candidates.clear();
  m_diag.is_uncertain = false;
  m_diag.suppress_write = false;

  pfc::string8 trackLabel;
  if (trackIdentifier != nullptr && trackIdentifier[0] != '\0') {
    trackLabel = trackIdentifier;
  }
  if (trackLabel.is_empty() && track.is_valid()) {
    trackLabel = pfc::string_filename_ext(track->get_path());
  }
  if (trackLabel.is_empty()) {
    trackLabel = "<unknown>";
  }
  const uint64_t trackKey = stable_track_key(track);
  const uint32_t subsongIndex =
      track.is_valid() ? track->get_subsong_index() : 0;
  if (verboseLogs) {
    smart_tempo::log_mir_analysis_provenance(
        trackLabel.get_ptr(), trackKey, subsongIndex, kMirBuildId,
        kMirPolicySchema);
  }

  const int secondsToRead = clamp_analysis_seconds_to_read(
      (int)bpm_config_analysis_seconds_to_read);
  const int samplePasses = clamp_analysis_sample_passes(
      (int)bpm_config_analysis_sample_passes);

  const int offsetMinPct = kAnalysisOffsetMinPct;
  const int offsetMaxPct = kAnalysisOffsetMaxPct;
  const bool hasPriorWindow =
      priorMinBpm > 0.0 && priorMaxBpm > priorMinBpm;
  double targetMinBpm = 0.0;
  double targetMaxBpm = 999.0;
  const char* routeName = "Fallback Safety Rails";
  if (hasPriorWindow) {
    targetMinBpm = priorMinBpm;
    targetMaxBpm = priorMaxBpm;
    if (routingContext != nullptr && routingContext->route_matched) {
      routeName = "Smart Metadata Route";
    }
  }

  // Onset extraction is measured audio evidence. Metadata routes may re-rank
  // its candidates later, but must not alter the candidate frontend itself.
  const double dynamicMinIoiSeconds = kMirProductionMinIoiSeconds;
  m_diag.method = "Hodgkinson/MIR";


  input_helper input_file;
  service_ptr_t<file> file_svc;
  try {
    input_file.open(file_svc, track, 0, p_abort, false, false);
  } catch (...) {
    m_diag.error_reason = "Could not open file for analysis";
    return 0.0;
  }
  if (!input_file.is_open()) {
    m_diag.error_reason = "Could not open file for analysis";
    return 0.0;
  }

  const double trackLength = track->get_length();
  const bool hasKnownTrackLength = std::isfinite(trackLength) && trackLength > 0.0;
  if (hasKnownTrackLength && trackLength < kMinimumTrackLengthSeconds) {
    m_diag.error_reason = "Track too short for analysis";
    return 0.0;
  }
  const auto windowPlan = make_analysis_window_plan(
      secondsToRead, samplePasses, hasKnownTrackLength, trackLength,
      kDecodeSafetyMarginSeconds);
  if (!windowPlan.valid()) {
    m_diag.error_reason = windowPlan.error;
    return 0.0;
  }
  const double effectiveSecondsToRead = windowPlan.secondsToRead;
  const int effectiveSamplePasses = windowPlan.samplePasses;
  // Provenance is diagnostic only: no metadata, candidate, or DSP selection
  // uses these output fields. This binds MW1/D12 runs to their actual geometry.
  if (verboseLogs) {
    smart_tempo::log_mir_sampling_plan(
        trackLabel.get_ptr(), trackKey, subsongIndex, secondsToRead,
        samplePasses, effectiveSecondsToRead, effectiveSamplePasses,
        hasKnownTrackLength, trackLength, offsetMinPct, offsetMaxPct);
  }

  aligned_vector<uint64_t> allMirOdfPeaksAbs;
  std::vector<double> allMirOdfPeakWeights;
  aligned_vector<uint64_t> allMirHighFrequencyOdfPeaksAbs;
  std::vector<double> allMirHighFrequencyOdfPeakWeights;
  std::vector<smart_tempo::HodgkinsonTatumProbeResult>
      hodgkinsonFullMirSegmentResults;
  MaterialRiskAccumulator materialRiskAccumulator;
  initialize_material_risk_accumulator(
      materialRiskAccumulator, static_cast<std::size_t>(effectiveSamplePasses));
  PartialBarAggregateAccumulator partialBarAggregateAccumulator;
  const bool partialBarEnabled =
      kEnableMirPolicyPartialBarFamilyConflictRecovery ||
      smart_tempo::experimental::kEnableHodgkinsonPartialBarProbe;
  if (partialBarEnabled) {
    initialize_partial_bar_aggregate_accumulator(
        partialBarAggregateAccumulator,
        static_cast<std::size_t>(effectiveSamplePasses));
  }
  // Primary selection and policy resolution consume one canonical measured
  // source. No second board or repeated Hold view is constructed.
  MirCandidateBoard mirMeasuredCandidateBoard;
  // Retain full-track MIR peaks for exact local and continuous measurements.
  aligned_vector<uint64_t> mirPrimaryPhasePeaks;
  if (smart_tempo::experimental::kEnableMirPrimaryTelemetry ||
      smart_tempo::experimental::kEnableMirPolicyPipeline) {
    initialize_mir_candidate_board(
        mirMeasuredCandidateBoard,
        static_cast<std::size_t>(effectiveSamplePasses));
  }
  bool gotFormat = false;
  unsigned sampleRate = 0;
  unsigned analysisSampleRate = 0;
  unsigned channelCount = 0;
  size_t samplesToReadPerPass = 0;
  size_t analysisSamplesToReadPerPass = 0;
  uint_t analysisWinSize = 0;
  uint_t analysisHopSize = 0;
  bool downsampleInput = false;
  service_ptr_t<dsp> analysisResampler;
  bool telemetryHeaderLogged = false;
  uint64_t dynamicMinIoiSamples = 1;
  smart_tempo::LowFrequencyEnergyTracker energyTracker;
  int progress = 0;
  std::vector<MirOdfFrame> mirOdfFrames;
  std::vector<float> hodgkinsonFullMirPassSamples;
  std::vector<smpl_t> mirOnlyHop;
  status.set_progress_secondary(0, effectiveSamplePasses);

  for (int pass = 0; pass < effectiveSamplePasses; ++pass) {
    WaitWhilePaused(p_abort);
    if (p_abort.is_aborting()) break;

    const auto passOffset = compute_analysis_pass_offset(
        pass, effectiveSamplePasses, hasKnownTrackLength, trackLength,
        effectiveSecondsToRead, offsetMinPct, offsetMaxPct,
        kPassEndSafetyMarginSeconds);
    const double passStartSec = passOffset.startSec;
    const double offset_pct = passOffset.offsetPct;

    if (input_file.can_seek()) {
      input_file.seek(passStartSec, p_abort);
    } else if (pass > 0) {
      break; // cannot seek, single pass only
    }

    audio_chunk_impl chunk;
    if (!metrics.run_decode_work([&] { return input_file.run(chunk, p_abort); })) {
      break;
    }

    if (!gotFormat) {
      sampleRate = chunk.get_srate();
      channelCount = chunk.get_channels();
      if (sampleRate == 0 || channelCount == 0) {
        m_diag.error_reason = "Invalid stream format";
        return 0.0;
      }
      analysisSampleRate =
          (std::min)(sampleRate, kMirProductionSampleRateCapHz);
      downsampleInput = analysisSampleRate != sampleRate;
      if (downsampleInput &&
          !resampler_entry::g_create(analysisResampler, sampleRate,
                                     analysisSampleRate, 1.0f)) {
        m_diag.error_reason = "High-quality analysis resampler unavailable";
        return 0.0;
      }
      energyTracker.configure(static_cast<double>(analysisSampleRate));
      dynamicMinIoiSamples = (std::max)(
          1ull, static_cast<uint64_t>(std::llround(
                    dynamicMinIoiSeconds * static_cast<double>(analysisSampleRate))));
      if (verboseLogs) {
        log_analysis_sample_rate(sampleRate, analysisSampleRate,
                                 kMirProductionSampleRateCapHz,
                                 downsampleInput, dynamicMinIoiSeconds,
                                 dynamicMinIoiSamples);
      }

        // High-BPM stable resolution policy (44.1kHz baseline: 1024/512).
        if (analysisSampleRate >= 32000) {
          analysisWinSize = 1024;
          analysisHopSize = 512;
        } else if (analysisSampleRate >= 16000) {
          analysisWinSize = 512;
          analysisHopSize = 256;
        } else {
          analysisWinSize = 256;
          analysisHopSize = 128;
        }
      if (analysisHopSize >= analysisWinSize) {
        analysisHopSize = (std::max)(8u, analysisWinSize / 2);
      }

      samplesToReadPerPass = (std::max)(
          static_cast<size_t>(1),
          static_cast<size_t>(std::llround(static_cast<double>(sampleRate) *
                                           effectiveSecondsToRead)));
      analysisSamplesToReadPerPass = (std::max)(
          static_cast<size_t>(1),
          static_cast<size_t>(std::llround(
              static_cast<double>(analysisSampleRate) * effectiveSecondsToRead)));
      hodgkinsonFullMirSegmentResults.reserve(effectiveSamplePasses);
      if (smart_tempo::experimental::kEnableHodgkinsonTatumProbe) {
        hodgkinsonFullMirPassSamples.reserve(analysisSamplesToReadPerPass);
      }
      mirOdfFrames.reserve(
          (analysisSamplesToReadPerPass + analysisHopSize - 1) /
          analysisHopSize);
      mirOnlyHop.assign(analysisHopSize, smpl_t{0});
      gotFormat = true;
    }
    const double sampleRateF = (double)sampleRate;
    const double analysisSampleRateF = (double)analysisSampleRate;

    if (verboseLogs && !telemetryHeaderLogged) {
      log_telemetry_header(
          trackLabel.get_ptr(), sampleRate, analysisSampleRate,
          analysisWinSize, analysisHopSize, dynamicMinIoiSeconds,
          dynamicMinIoiSamples);
      telemetryHeaderLogged = true;
    }

    size_t samplesRead = 0;
    bool canceledPass = false;
    mirOdfFrames.clear();
    hodgkinsonFullMirPassSamples.clear();
    double mirOdfPreviousLogEnergy = 0.0;
    bool mirOdfHasPreviousLogEnergy = false;
    double mirOdfPreviousLogHighFrequencyEnergy = 0.0;
    bool mirOdfHasPreviousLogHighFrequencyEnergy = false;
    smart_tempo::ProgressMillisAccumulator progressAccumulator(
        m_progressMillis, sampleRateF);

    uint_t mirOnlyHopFill = 0;
    uint64_t mirOnlySamplesSeen = 0;
    size_t analysisSamplesEmitted = 0;
    auto emitAnalysisSample = [&](smpl_t outSample) {
      if (analysisSamplesEmitted >= analysisSamplesToReadPerPass) return;
      ++analysisSamplesEmitted;
      energyTracker.observe(static_cast<double>(outSample));
      if (smart_tempo::experimental::kEnableHodgkinsonTatumProbe) {
        hodgkinsonFullMirPassSamples.push_back(static_cast<float>(outSample));
      }
      mirOnlyHop[mirOnlyHopFill] = outSample;
      ++mirOnlyHopFill;
      ++mirOnlySamplesSeen;
      if (mirOnlyHopFill == analysisHopSize) {
        metrics.run_onset_work([&] {
          observe_mir_odf_energy_flux_samples(
              mirOdfFrames,
              std::span<const smpl_t>(mirOnlyHop.data(), mirOnlyHop.size()),
              mirOnlySamplesSeen, mirOdfPreviousLogEnergy,
              mirOdfHasPreviousLogEnergy,
              mirOdfPreviousLogHighFrequencyEnergy,
              mirOdfHasPreviousLogHighFrequencyEnergy);
        });
        mirOnlyHopFill = 0;
      }
    };

    auto consume_analysis_chunk = [&](const audio_chunk& ch) {
      if (ch.get_srate() != analysisSampleRate || ch.get_channels() == 0) {
        m_diag.error_reason = "Unexpected analysis resampler output format";
        return false;
      }
      const audio_sample* data = ch.get_data();
      const size_t remaining =
          analysisSamplesToReadPerPass - analysisSamplesEmitted;
      const size_t frameCount =
          (std::min)(static_cast<size_t>(ch.get_sample_count()), remaining);
      const unsigned outputChannels = ch.get_channels();
      for (size_t frame = 0; frame < frameCount; ++frame) {
        if ((frame & 0x1FF) == 0 && p_abort.is_aborting()) return false;
        const smpl_t mono =
            smart_tempo::mixdown_interleaved_frame_as<smpl_t>(
                data, frame, outputChannels);
        emitAnalysisSample(mono);
      }
      return true;
    };

    auto consume_source_chunk = [&](const audio_chunk_impl& ch,
                                    size_t frameCount) {
      if (!downsampleInput) return consume_analysis_chunk(ch);

      audio_chunk_impl sourceChunk;
      sourceChunk.set_data(ch.get_data(), frameCount, ch.get_channels(),
                           ch.get_srate(), ch.get_channel_config());
      dsp_chunk_list_impl outputChunks;
      outputChunks.add_chunk(&sourceChunk);
      dsp_track_t noTrack;
      analysisResampler->run_abortable(&outputChunks, noTrack, 0, p_abort);
      for (t_size index = 0; index < outputChunks.get_count(); ++index) {
        if (!consume_analysis_chunk(*outputChunks.get_item(index))) return false;
      }
      return true;
    };

    auto process_current_chunk = [&](size_t count) {
      if (count == 0) return true;
      if (!metrics.run_decode_work(
              [&] { return consume_source_chunk(chunk, count); })) {
        return false;
      }
      samplesRead += count;
      progressAccumulator.accumulate(count);
      return true;
    };

    // first chunk
    {
      size_t count = chunk.get_sample_count();
      if (count > samplesToReadPerPass) count = samplesToReadPerPass;
      if (!process_current_chunk(count)) {
        canceledPass = true;
      }
    }
    if (canceledPass) {
      progressAccumulator.flush(true);
      break;
    }

    // remaining chunks
    while (samplesRead < samplesToReadPerPass) {
      WaitWhilePaused(p_abort);
      if (p_abort.is_aborting()) break;
      if (!metrics.run_decode_work([&] { return input_file.run(chunk, p_abort); })) {
        break;
      }

      size_t count = chunk.get_sample_count();
      const size_t remain = samplesToReadPerPass - samplesRead;
      if (count > remain) count = remain;
      if (!process_current_chunk(count)) {
        canceledPass = true;
        break;
      }
    }
    if (canceledPass) {
      progressAccumulator.flush(true);
      break;
    }

    if (downsampleInput) {
      dsp_chunk_list_impl tailChunks;
      dsp_track_t noTrack;
      analysisResampler->run_abortable(&tailChunks, noTrack, dsp::FLUSH,
                                       p_abort);
      for (t_size index = 0; index < tailChunks.get_count(); ++index) {
        if (!consume_analysis_chunk(*tailChunks.get_item(index))) {
          canceledPass = true;
          break;
        }
      }
    }
    if (canceledPass) {
      progressAccumulator.flush(true);
      break;
    }

    // MIR-only uses complete
    // analysis hops and keeps the partial tail as segment audio, not ODF state.
    progressAccumulator.flush(true);

    if (smart_tempo::experimental::kEnableHodgkinsonTatumProbe) {
      const uint64_t passStartSample =
          (uint64_t)std::llround(passStartSec * analysisSampleRateF);
      append_mir_odf_peaks_from_frames(allMirOdfPeaksAbs, allMirOdfPeakWeights,
                                       mirOdfFrames, passStartSample,
                                       dynamicMinIoiSamples,
                                       MirOdfFluxKind::Energy);
      append_mir_odf_peaks_from_frames(
          allMirHighFrequencyOdfPeaksAbs,
          allMirHighFrequencyOdfPeakWeights, mirOdfFrames, passStartSample,
          dynamicMinIoiSamples, MirOdfFluxKind::HighFrequency);
      if (!hodgkinsonFullMirPassSamples.empty()) {
        smart_tempo::HodgkinsonFullMirSegment fullMirSegment;
        fullMirSegment.mono_samples =
            std::span<const float>(hodgkinsonFullMirPassSamples.data(),
                                   hodgkinsonFullMirPassSamples.size());
        fullMirSegment.sample_rate = analysisSampleRateF;
        fullMirSegment.segment_start_sec = passStartSec;
        fullMirSegment.segment_index = static_cast<size_t>(pass);
        fullMirSegment.segment_count = static_cast<size_t>(effectiveSamplePasses);
        smart_tempo::HodgkinsonTatumProbeResult fullMirResult;
        smart_tempo::HodgkinsonFullMirSegmentEvaluation fullMirEvaluation;
        if (partialBarEnabled) {
          fullMirEvaluation = smart_tempo::
              evaluate_hodgkinson_full_mir_segment_with_partial_bar_candidates(
                  fullMirSegment);
          fullMirResult = fullMirEvaluation.result;
        } else if (
            smart_tempo::experimental::kEnableHodgkinsonSegmentTopKTelemetry ||
            smart_tempo::experimental::kEnableHodgkinsonMaterialRiskEvidence ||
            smart_tempo::experimental::
                kEnableMirPrimaryTelemetry) {
          fullMirEvaluation =
              smart_tempo::evaluate_hodgkinson_full_mir_segment_with_candidates(
                  fullMirSegment);
          fullMirResult = fullMirEvaluation.result;
        } else {
          fullMirResult =
              smart_tempo::evaluate_hodgkinson_full_mir_segment(fullMirSegment);
        }
        if (verboseLogs) {
          smart_tempo::log_hodgkinson_primary_segment(trackLabel.get_ptr(),
                                                      fullMirResult);
          if (smart_tempo::experimental::
                  kEnableHodgkinsonSegmentTopKTelemetry) {
            for (std::size_t topKIndex = 0;
                 topKIndex < fullMirEvaluation.top_k_count; ++topKIndex) {
              smart_tempo::log_hodgkinson_primary_segment_candidate(
                  trackLabel.get_ptr(), trackKey,
                  fullMirEvaluation.top_k_candidates[topKIndex]);
            }
          }
          if (partialBarEnabled) {
            for (std::size_t partialIndex = 0;
                 partialIndex < fullMirEvaluation.partial_bar_count;
                 ++partialIndex) {
              smart_tempo::log_hodgkinson_primary_segment_candidate(
                  trackLabel.get_ptr(), trackKey,
                  fullMirEvaluation.partial_bar_candidates[partialIndex]);
            }
          }
        }
        if (partialBarEnabled) {
          update_partial_bar_aggregate_accumulator(
              partialBarAggregateAccumulator, fullMirEvaluation);
        }
        if (smart_tempo::experimental::kEnableHodgkinsonMaterialRiskEvidence) {
          update_material_risk_accumulator(materialRiskAccumulator,
                                           fullMirEvaluation);
        }
        if (smart_tempo::experimental::
                kEnableMirPrimaryTelemetry ||
            smart_tempo::experimental::kEnableMirPolicyPipeline) {
          for (std::size_t topKIndex = 0;
               topKIndex < fullMirEvaluation.top_k_count; ++topKIndex) {
            if (smart_tempo::experimental::kEnableMirPrimaryTelemetry ||
                smart_tempo::experimental::kEnableMirPolicyPipeline) {
              add_mir_topk_candidate(
                  mirMeasuredCandidateBoard,
                  fullMirEvaluation.top_k_candidates[topKIndex]);
            }
          }
        }
        if (fullMirResult.loop_fit_score > 0.0 ||
            fullMirResult.candidate_bpm > 0.0) {
          hodgkinsonFullMirSegmentResults.push_back(fullMirResult);
        }
      }
    }
    status.set_progress_secondary(++progress, effectiveSamplePasses);
  }

  if (p_abort.is_aborting()) {
    m_diag.error_reason = "Aborted";
    return 0.0;
  }
  // MIR/primary owns selection. No Grid score, folding, route projection, or
  // legacy candidate may seed this decision state.
  double finalBpm = 0.0;
  double confidence = 0.0;
  PolicyProjectionReason policyProjectionReason = PolicyProjectionReason::none;
  DecisionSummaryLogSnapshot decisionLog;
  decisionLog.targetMinBpm = targetMinBpm;
  decisionLog.targetMaxBpm = targetMaxBpm;
  decisionLog.routeName = routeName;
  decisionLog.policyPathText = "HodgkinsonMIR";
  decisionLog.routingContext = routingContext;
  decisionLog.projected.reason = "mir-pending";
  decisionLog.policyReason = policyProjectionReason;

  // The production primary engine always collects the MIR evidence required by
  // the selector; verbose mode only controls how much of that evidence is logged.
  const bool primaryAnalysisNeeded =
      smart_tempo::experimental::kEnableMirPrimaryRuntime &&
      analysisSampleRate > 0;
  if (primaryAnalysisNeeded) {
      smart_tempo::HodgkinsonOnlyFamilyResolverShadow
          hodgkinsonOnlyResolverShadow;
      smart_tempo::HodgkinsonTatumProbeResult hodgkinsonFullMirOnlyResult;
      if (smart_tempo::experimental::kEnableHodgkinsonTatumProbe &&
          (verboseLogs ||
           smart_tempo::experimental::kEnableHodgkinsonMaterialRiskEvidence ||
           smart_tempo::experimental::
               kEnableMirPrimaryTelemetry)) {
        const auto hodgkinsonFullMirResult =
            smart_tempo::aggregate_hodgkinson_full_mir_results(
                "audacity_mir_full", finalBpm,
                std::span<const smart_tempo::HodgkinsonTatumProbeResult>(
                    hodgkinsonFullMirSegmentResults.data(),
                    hodgkinsonFullMirSegmentResults.size()));
        if (verboseLogs) {
          smart_tempo::log_hodgkinson_tatum_probe(trackLabel.get_ptr(), trackKey,
                                                  hodgkinsonFullMirResult);
        }
        hodgkinsonFullMirOnlyResult =
            smart_tempo::aggregate_hodgkinson_full_mir_only_results(
                "audacity_mir_full_only",
                std::span<const smart_tempo::HodgkinsonTatumProbeResult>(
                    hodgkinsonFullMirSegmentResults.data(),
                    hodgkinsonFullMirSegmentResults.size()));
        const bool standardMirCandidateSetLog =
            smart_tempo::experimental::kEnableHodgkinsonSegmentTopKTelemetry;
        if (verboseLogs || standardMirCandidateSetLog) {
          smart_tempo::log_hodgkinson_primary_candidate_set(
              trackLabel.get_ptr(), hodgkinsonFullMirOnlyResult);
        }
        if (verboseLogs ||
            smart_tempo::experimental::
                kEnableMirPrimaryTelemetry) {
          if (smart_tempo::experimental::kEnableHodgkinsonAliasPulseEvidence ||
              smart_tempo::experimental::
                  kEnableHodgkinsonContinuousRefinementEvidence ||
              smart_tempo::experimental::
                  kEnableMirPrimaryTelemetry ||
              smart_tempo::experimental::
                  kEnableMirPolicyPipeline) {
            mirPrimaryPhasePeaks.reserve(allMirOdfPeaksAbs.size() +
                                         allMirHighFrequencyOdfPeaksAbs.size());
            mirPrimaryPhasePeaks.insert(mirPrimaryPhasePeaks.end(),
                                        allMirOdfPeaksAbs.begin(),
                                        allMirOdfPeaksAbs.end());
            mirPrimaryPhasePeaks.insert(mirPrimaryPhasePeaks.end(),
                                        allMirHighFrequencyOdfPeaksAbs.begin(),
                                        allMirHighFrequencyOdfPeaksAbs.end());
            std::sort(mirPrimaryPhasePeaks.begin(), mirPrimaryPhasePeaks.end());
            mirPrimaryPhasePeaks.erase(
                std::unique(mirPrimaryPhasePeaks.begin(),
                            mirPrimaryPhasePeaks.end()),
                mirPrimaryPhasePeaks.end());

            const std::size_t familyRankCount =
                (std::min)(hodgkinsonFullMirOnlyResult.family_rank_count,
                           hodgkinsonFullMirOnlyResult.family_rank_bpm.size());
            for (std::size_t rank = 0; rank < familyRankCount; ++rank) {
              const double familyBpm =
                  hodgkinsonFullMirOnlyResult.family_rank_bpm[rank];
              const bool mirPolicyEnabled = smart_tempo::experimental::
                  kEnableMirPolicyPipeline;
              const auto aliases =
                  smart_tempo::route_valid_hodgkinson_family_aliases(
                      familyBpm, kMirProductionMinBpm, kMirProductionMaxBpm);
              for (const auto& alias : aliases) {
                const bool aliasRouteValid =
                    alias.bpm >= kMirProductionMinBpm &&
                    alias.bpm <= kMirProductionMaxBpm;
                if (smart_tempo::experimental::
                        kEnableHodgkinsonAliasPulseEvidence ||
                    smart_tempo::experimental::
                        kEnableMirPrimaryTelemetry ||
                    mirPolicyEnabled) {
                  const AliasPhaseMetrics phaseMetrics =
                      compute_alias_phase_metrics(
                          std::span<const uint64_t>(mirPrimaryPhasePeaks.data(),
                                                    mirPrimaryPhasePeaks.size()),
                          alias.bpm, static_cast<double>(analysisSampleRate));
                  const AliasSectionMetrics sectionMetrics =
                      compute_alias_section_metrics(
                          std::span<const uint64_t>(mirPrimaryPhasePeaks.data(),
                                                    mirPrimaryPhasePeaks.size()),
                          std::span<
                              const smart_tempo::HodgkinsonTatumProbeResult>(
                              hodgkinsonFullMirSegmentResults.data(),
                              hodgkinsonFullMirSegmentResults.size()),
                          alias.bpm, static_cast<double>(analysisSampleRate));
                  smart_tempo::HodgkinsonAliasPulseEvidence evidence;
                  evidence.enabled = true;
                  evidence.route_valid = aliasRouteValid;
                  evidence.family_rank = rank + 1;
                  evidence.family_bpm = familyBpm;
                  evidence.family_score =
                      hodgkinsonFullMirOnlyResult.family_rank_score[rank];
                  evidence.family_support =
                      hodgkinsonFullMirOnlyResult.family_rank_support[rank];
                  evidence.family_candidate_support =
                      hodgkinsonFullMirOnlyResult
                          .family_rank_candidate_support[rank];
                  evidence.alias_bpm = alias.bpm;
                  evidence.alias_multiplier = alias.multiplier;
                  evidence.alias_class = alias.multiplier_class;
                  evidence.phase_vs = phaseMetrics.phase_vs;
                  evidence.axial_phase_vs = phaseMetrics.axial_phase_vs;
                  evidence.beat_salience = phaseMetrics.phase_vs;
                  evidence.tatum_salience = phaseMetrics.axial_phase_vs;
                  evidence.beat_tatum_ratio =
                      phaseMetrics.axial_phase_vs > 0.0
                          ? phaseMetrics.phase_vs / phaseMetrics.axial_phase_vs
                          : 0.0;
                  evidence.normalized_onset_density =
                      phaseMetrics.normalized_onset_density;
                  evidence.section_support = sectionMetrics.section_support;
                  evidence.section_stability = sectionMetrics.section_stability;
                  evidence.pulse_score =
                      0.40 * phaseMetrics.phase_vs +
                      0.25 * phaseMetrics.axial_phase_vs +
                      0.15 *
                          (std::min)(1.0,
                                     phaseMetrics.normalized_onset_density) +
                      0.10 * sectionMetrics.section_support +
                      0.10 * sectionMetrics.section_stability;
                  evidence.reason = mirPrimaryPhasePeaks.size() >= 2
                                        ? "mir_segment_phase_initial"
                                        : "insufficient_onsets";
                  if (verboseLogs &&
                      smart_tempo::experimental::
                          kEnableHodgkinsonAliasPulseEvidence) {
                    smart_tempo::log_hodgkinson_alias_pulse_evidence(
                        trackLabel.get_ptr(), evidence);
                  }
                  if ((smart_tempo::experimental::
                           kEnableMirPrimaryTelemetry &&
                       aliasRouteValid) ||
                      mirPolicyEnabled) {
                    add_mir_alias_pulse_evidence(mirMeasuredCandidateBoard,
                                                 evidence);
                  }
                }
                if (smart_tempo::experimental::
                        kEnableHodgkinsonContinuousRefinementEvidence ||
                    smart_tempo::experimental::
                        kEnableMirPrimaryTelemetry ||
                    mirPolicyEnabled) {
                  const auto continuousEvidence =
                      compute_hodgkinson_continuous_refinement_evidence(
                          hodgkinsonFullMirOnlyResult, alias, rank + 1,
                          std::span<const uint64_t>(
                              mirPrimaryPhasePeaks.data(),
                              mirPrimaryPhasePeaks.size()),
                          std::span<
                              const smart_tempo::HodgkinsonTatumProbeResult>(
                              hodgkinsonFullMirSegmentResults.data(),
                              hodgkinsonFullMirSegmentResults.size()),
                          static_cast<double>(analysisSampleRate));
                  if (verboseLogs &&
                      smart_tempo::experimental::
                          kEnableHodgkinsonContinuousRefinementEvidence) {
                    smart_tempo::log_hodgkinson_continuous_refinement_evidence(
                        trackLabel.get_ptr(), continuousEvidence);
                  }
                  if ((smart_tempo::experimental::
                           kEnableMirPrimaryTelemetry &&
                       aliasRouteValid) ||
                      mirPolicyEnabled) {
                    add_mir_continuous_refinement_evidence(
                        mirMeasuredCandidateBoard, continuousEvidence);
                  }
                }
              }
            }
          }
          if (verboseLogs) {
            smart_tempo::log_hodgkinson_tatum_probe(trackLabel.get_ptr(), trackKey,
                                                    hodgkinsonFullMirOnlyResult);
          }
        }
        hodgkinsonOnlyResolverShadow =
            smart_tempo::evaluate_hodgkinson_only_family_resolver_shadow(
                hodgkinsonFullMirOnlyResult, kMirProductionMinBpm,
                kMirProductionMaxBpm);
        if (verboseLogs) {
          smart_tempo::log_hodgkinson_only_family_resolver_shadow(
              trackLabel.get_ptr(), hodgkinsonOnlyResolverShadow);
        }
        smart_tempo::HodgkinsonMaterialRiskEvidence materialRiskEvidence;
        if (smart_tempo::experimental::kEnableHodgkinsonMaterialRiskEvidence ||
            smart_tempo::experimental::
                kEnableMirPrimaryTelemetry) {
          materialRiskEvidence =
              compute_hodgkinson_material_risk_evidence(
                  materialRiskAccumulator, hodgkinsonOnlyResolverShadow,
                  targetMinBpm, targetMaxBpm,
                  routingContext != nullptr && routingContext->route_matched,
                  routingContext != nullptr &&
                      routingContext->is_generic_match);
        }
        if (smart_tempo::experimental::kEnableHodgkinsonMaterialRiskEvidence) {
          if (materialRiskEvidence.candidate) {
            runtime_counters().hodgkinson_material_risk_candidates.fetch_add(
                1, std::memory_order_relaxed);
          }
          if (materialRiskEvidence.review_hold_candidate) {
            runtime_counters()
                .hodgkinson_material_risk_review_holds.fetch_add(
                    1, std::memory_order_relaxed);
          }
          if (verboseLogs) {
            smart_tempo::log_hodgkinson_material_risk_evidence(
                trackLabel.get_ptr(), materialRiskEvidence);
          }
        }
        if (smart_tempo::experimental::
                kEnableMirPrimaryTelemetry) {
          MirLocalExactCache mirLocalExactCache(
              std::span<const uint64_t>(mirPrimaryPhasePeaks.data(),
                                        mirPrimaryPhasePeaks.size()),
              static_cast<double>(analysisSampleRate));
          const auto partialBarAggregateShadow =
              compute_partial_bar_aggregate_shadow(
                  partialBarAggregateAccumulator, mirLocalExactCache,
                  partialBarEnabled);
          if (verboseLogs && partialBarEnabled) {
            smart_tempo::log_hodgkinson_partial_bar_aggregate_shadow(
                trackLabel.get_ptr(), trackKey, partialBarAggregateShadow);
          }
          std::vector<MirPolicyMeasuredCandidate> mirPolicyMeasuredCandidates;
          if (verboseLogs && smart_tempo::experimental::
                                 kEnableMirCandidateBoardTelemetry) {
            const auto candidateBoard =
                collapse_mir_candidates(mirMeasuredCandidateBoard);
            for (std::size_t index = 0; index < candidateBoard.size(); ++index) {
              const auto& candidate = candidateBoard[index];
              const std::string aliasClasses = mir_alias_text(candidate.alias_bits);
              smart_tempo::log_mir_cluster_candidate_board_entry(
                  trackLabel.get_ptr(), index, candidate.bpm, aliasClasses.c_str(),
                  candidate.support, candidate.winner_support, candidate.score_sum,
                  candidate.best_combined_score, candidate.pulse_score,
                  candidate.pulse_section_support, candidate.pulse_phase_vs,
                  candidate.pulse_axial_phase_vs, candidate.continuous_score,
                  candidate.continuous_section_support,
                  candidate.continuous_score_ratio,
                  candidate.continuous_section_stability, candidate.rows,
                  mirMeasuredCandidateBoard.segment_count);
            }
          }
          if (smart_tempo::experimental::
                  kEnableMirPolicyPipeline) {
            mirPolicyMeasuredCandidates =
                prepare_mir_policy_fullboard_candidates(
                    mirMeasuredCandidateBoard, mirLocalExactCache);
            if (verboseLogs && smart_tempo::experimental::
                                   kEnableMirSoftCenterTelemetry) {
              for (std::size_t index = 0;
                   index < mirMeasuredCandidateBoard.raw_candidates.size(); ++index) {
                const auto& raw = mirMeasuredCandidateBoard.raw_candidates[index];
                const double aliasMultiplier =
                    mir_alias_multiplier(raw.alias_class);
                const double sourceBpm =
                    aliasMultiplier > 0.0 ? raw.bpm / aliasMultiplier : 0.0;
                const double rows =
                    raw.rows > 0 ? static_cast<double>(raw.rows) : 1.0;
                const MirLocalExactBpm localExact =
                    mirLocalExactCache.measure(raw.bpm);
                smart_tempo::
                    log_mir_raw_alias_board_entry(
                        trackLabel.get_ptr(), trackKey, index, raw.bpm,
                        localExact.bpm, localExact.score,
                        localExact.bpm - raw.bpm, raw.alias_class,
                        aliasMultiplier, sourceBpm,
                        mir_raw_support(
                            raw, mirMeasuredCandidateBoard.segment_count),
                        mir_raw_winner_support(
                            raw, mirMeasuredCandidateBoard.segment_count),
                        mir_segment_hit_count(raw.segments),
                        mir_segment_hit_count(raw.winner_segments),
                        mir_segment_mask(raw.segments),
                        mir_segment_mask(raw.winner_segments),
                        raw.score_sum, raw.quant_sum / rows,
                        raw.meter_sum / rows, raw.best_combined_score,
                        raw.best_quantization_score, raw.best_meter_score,
                        raw.rows, raw.winner_rows, raw.synthetic_support,
                        raw.synthetic_winner_support, raw.pulse_score,
                        raw.pulse_section_support, raw.pulse_phase_vs,
                        raw.pulse_axial_phase_vs, raw.continuous_score,
                        raw.continuous_section_support,
                        raw.continuous_score_ratio,
                        raw.continuous_section_stability,
                        mirMeasuredCandidateBoard.segment_count,
                        raw.segments.size() > 64 ||
                            raw.winner_segments.size() > 64);
              }
              for (std::size_t index = 0;
                   index < mirPolicyMeasuredCandidates.size();
                   ++index) {
                const auto& measured = mirPolicyMeasuredCandidates[index];
                const auto& candidate = measured.candidate;
                smart_tempo::
                    log_mir_candidate_board_entry(
                        trackLabel.get_ptr(), index, measured.cluster_bpm,
                        measured.bpm, measured.local_exact_score,
                        measured.bpm - measured.cluster_bpm,
                        measured.alias_classes.c_str(), candidate.support,
                        candidate.winner_support, candidate.score_sum,
                        candidate.best_combined_score, candidate.pulse_score,
                        candidate.pulse_section_support,
                        candidate.pulse_phase_vs,
                        candidate.pulse_axial_phase_vs,
                        candidate.continuous_score,
                        candidate.continuous_section_support,
                        candidate.continuous_score_ratio,
                        candidate.continuous_section_stability,
                        candidate.rows, mirMeasuredCandidateBoard.segment_count);
              }
            }

            if (verboseLogs && smart_tempo::experimental::
                                   kEnableMirSoftCenterTelemetry) {
              auto mirDspSelection =
                  compute_mir_dsp_selection(
                      mirMeasuredCandidateBoard, hodgkinsonFullMirOnlyResult,
                      materialRiskEvidence, true);
              apply_mir_local_exact_measurement(
                  mirDspSelection, mirLocalExactCache);
              const bool genericRoute =
                  routingContext != nullptr &&
                  routingContext->is_generic_match;
              smart_tempo::
                  log_mir_dsp_selection(
                      trackLabel.get_ptr(), mirDspSelection,
                      targetMinBpm, targetMaxBpm, genericRoute);

              auto mirSoftCenterSelection =
                  compute_mir_soft_center_selection(
                      mirMeasuredCandidateBoard, hodgkinsonFullMirOnlyResult,
                      materialRiskEvidence, targetMinBpm, targetMaxBpm,
                      routingContext != nullptr && routingContext->route_matched,
                      routingContext != nullptr &&
                          routingContext->is_generic_match,
                      true);
              apply_mir_local_exact_measurement(
                  mirSoftCenterSelection, mirLocalExactCache);
              smart_tempo::
                  log_mir_dsp_selection(
                      trackLabel.get_ptr(), mirSoftCenterSelection,
                      targetMinBpm, targetMaxBpm, genericRoute);
            }
          }
          MirRoutingContext mirRoutingContext;
          const MirRoutingContext* mirRoutingContextPtr = nullptr;
          if (routingContext != nullptr) {
            mirRoutingContext.source_genres = routingContext->source_genres;
            mirRoutingContext.normalized_genres =
                routingContext->normalized_genres;
            mirRoutingContext.matched_token = routingContext->matched_token;
            mirRoutingContext.route_matched = routingContext->route_matched;
            mirRoutingContext.is_generic_match =
                routingContext->is_generic_match;
            mirRoutingContextPtr = &mirRoutingContext;
          }
          auto mirPrimarySelection =
              compute_mir_primary_selection(
                  mirMeasuredCandidateBoard, hodgkinsonFullMirOnlyResult,
                  materialRiskEvidence, mirRoutingContextPtr,
                  kMirProductionMinBpm, kMirProductionMaxBpm,
                  smart_tempo::experimental::
                      kEnableMirPrimaryTelemetry);
          if (verboseLogs &&
              smart_tempo::experimental::
                  kEnableHodgkinsonContinuousRefinementEvidence &&
              mirPrimarySelection.auto_candidate &&
              mirPrimarySelection.selected_bpm >= 145.0 &&
              mirPrimarySelection.selected_bpm <= 190.0 &&
              mirPrimaryPhasePeaks.size() >= 2) {
            const smart_tempo::HodgkinsonFamilyAliasOption selectedAlias{
                mirPrimarySelection.selected_bpm, 1.0, "direct"};
            auto continuousEvidence =
                compute_hodgkinson_continuous_refinement_evidence(
                    hodgkinsonFullMirOnlyResult, selectedAlias, 0,
                    std::span<const uint64_t>(mirPrimaryPhasePeaks.data(),
                                              mirPrimaryPhasePeaks.size()),
                    std::span<const smart_tempo::HodgkinsonTatumProbeResult>(
                        hodgkinsonFullMirSegmentResults.data(),
                        hodgkinsonFullMirSegmentResults.size()),
                    static_cast<double>(analysisSampleRate));
            refine_hodgkinson_continuous_evidence_exact(
                continuousEvidence,
                std::span<const uint64_t>(mirPrimaryPhasePeaks.data(),
                                          mirPrimaryPhasePeaks.size()),
                static_cast<double>(analysisSampleRate));
            compute_hodgkinson_continuous_range_segment_consensus(
                continuousEvidence,
                std::span<const uint64_t>(mirPrimaryPhasePeaks.data(),
                                          mirPrimaryPhasePeaks.size()),
                std::span<const smart_tempo::HodgkinsonTatumProbeResult>(
                    hodgkinsonFullMirSegmentResults.data(),
                    hodgkinsonFullMirSegmentResults.size()),
                static_cast<double>(analysisSampleRate));
            compute_hodgkinson_interval_recurrence_evidence(
                continuousEvidence,
                std::span<const uint64_t>(mirPrimaryPhasePeaks.data(),
                                          mirPrimaryPhasePeaks.size()),
                std::span<const smart_tempo::HodgkinsonTatumProbeResult>(
                    hodgkinsonFullMirSegmentResults.data(),
                    hodgkinsonFullMirSegmentResults.size()),
                static_cast<double>(analysisSampleRate));
            compute_hodgkinson_continuous_segment_consensus(
                continuousEvidence,
                std::span<const uint64_t>(mirPrimaryPhasePeaks.data(),
                                          mirPrimaryPhasePeaks.size()),
                std::span<const smart_tempo::HodgkinsonTatumProbeResult>(
                    hodgkinsonFullMirSegmentResults.data(),
                    hodgkinsonFullMirSegmentResults.size()),
                static_cast<double>(analysisSampleRate));
            continuousEvidence.reason = "mir_high_pulse_auto_interval_scan";
            smart_tempo::log_hodgkinson_continuous_refinement_evidence(
                trackLabel.get_ptr(), continuousEvidence);
            const auto intervalConflictShadow =
                evaluate_mir_high_pulse_interval_conflict_shadow(
                    continuousEvidence, mirPrimarySelection.selected_bpm, true,
                    false);
            smart_tempo::log_mir_high_pulse_interval_conflict_shadow(
                trackLabel.get_ptr(), intervalConflictShadow);
          }
          finalize_mir_selector_shadow(
              mirPrimarySelection, hodgkinsonFullMirOnlyResult,
              mirLocalExactCache);
          smart_tempo::HodgkinsonMaterialRiskLocalExactProbe
              materialRiskLocalExactProbe;
          if (materialRiskEvidence.review_hold_candidate &&
              mirPrimaryPhasePeaks.size() >= 2) {
            auto& probe = materialRiskLocalExactProbe;
            probe.enabled = true;
            probe.candidate = true;
            probe.review_hold_candidate =
                materialRiskEvidence.review_hold_candidate;
            probe.selected_context_bpm =
                materialRiskEvidence.selected_context_bpm;
            probe.current_bpm = mirPrimarySelection.selected_local_exact_bpm;
            probe.current_local_exact_score =
                mirPrimarySelection.selected_local_exact_score;
            probe.scan_start_bpm =
                probe.selected_context_bpm -
                kMirPolicyMaterialRiskProbeScanBelowStartBpm;
            probe.scan_end_bpm =
                probe.selected_context_bpm -
                kMirPolicyMaterialRiskProbeScanBelowEndBpm;
            probe.scan_step_bpm = kMirPolicyMaterialRiskProbeScanStepBpm;
            probe.peak_count = mirPrimaryPhasePeaks.size();
            probe.support_136 = materialRiskEvidence.support_136;
            probe.support_144 = materialRiskEvidence.support_144;
            probe.winner_support_144 =
                materialRiskEvidence.winner_support_144;

            const double currentScore =
                (std::max)(probe.current_local_exact_score, 0.000001);
            auto isBoundaryHit = [](double anchorBpm, double exactBpm) {
              constexpr double kWindow = 0.75;
              constexpr double kTolerance = 0.0001;
              return std::abs(exactBpm - (anchorBpm - kWindow)) <=
                         kTolerance ||
                     std::abs(exactBpm - (anchorBpm + kWindow)) <=
                         kTolerance;
            };
            auto updateTopPeaks =
                [&](double anchorBpm, double exactBpm, double score,
                    bool boundaryHit) {
                  if (!std::isfinite(score)) {
                    return;
                  }
                  if (score > probe.top1_score) {
                    probe.top3_bpm = probe.top2_bpm;
                    probe.top3_score = probe.top2_score;
                    probe.top3_anchor_bpm = probe.top2_anchor_bpm;
                    probe.top3_boundary_hit = probe.top2_boundary_hit;
                    probe.top2_bpm = probe.top1_bpm;
                    probe.top2_score = probe.top1_score;
                    probe.top2_anchor_bpm = probe.top1_anchor_bpm;
                    probe.top2_boundary_hit = probe.top1_boundary_hit;
                    probe.top1_bpm = exactBpm;
                    probe.top1_score = score;
                    probe.top1_anchor_bpm = anchorBpm;
                    probe.top1_boundary_hit = boundaryHit;
                  } else if (score > probe.top2_score) {
                    probe.top3_bpm = probe.top2_bpm;
                    probe.top3_score = probe.top2_score;
                    probe.top3_anchor_bpm = probe.top2_anchor_bpm;
                    probe.top3_boundary_hit = probe.top2_boundary_hit;
                    probe.top2_bpm = exactBpm;
                    probe.top2_score = score;
                    probe.top2_anchor_bpm = anchorBpm;
                    probe.top2_boundary_hit = boundaryHit;
                  } else if (score > probe.top3_score) {
                    probe.top3_bpm = exactBpm;
                    probe.top3_score = score;
                    probe.top3_anchor_bpm = anchorBpm;
                    probe.top3_boundary_hit = boundaryHit;
                  }
                };

            double bestScore = -std::numeric_limits<double>::infinity();
            double bestBpm = 0.0;
            double bestAnchorBpm = 0.0;
            bool bestBoundaryHit = false;
            for (double clusterBpm = probe.scan_start_bpm;
                 clusterBpm <= probe.scan_end_bpm + 0.0001;
                 clusterBpm += probe.scan_step_bpm) {
              const auto exact = mirLocalExactCache.measure(clusterBpm);
              ++probe.scan_count;
              const bool boundaryHit = isBoundaryHit(clusterBpm, exact.bpm);
              updateTopPeaks(clusterBpm, exact.bpm, exact.score, boundaryHit);
              const double targetMin =
                  probe.selected_context_bpm -
                  kMirPolicyMaterialRiskProbeTargetBelowMinBpm;
              const double targetMax =
                  probe.selected_context_bpm -
                  kMirPolicyMaterialRiskProbeTargetBelowMaxBpm;
              if (exact.bpm >= targetMin && exact.bpm <= targetMax &&
                  exact.score >= currentScore) {
                ++probe.target_family_candidate_count;
              }
              if (std::isfinite(exact.score) && exact.score > bestScore) {
                bestScore = exact.score;
                bestBpm = exact.bpm;
                bestAnchorBpm = clusterBpm;
                bestBoundaryHit = boundaryHit;
              }
              if (std::abs(clusterBpm - 138.0) < 0.0001) {
                probe.probe_138_bpm = exact.bpm;
                probe.probe_138_score = exact.score;
                probe.probe_138_boundary_hit = boundaryHit;
              }
            }

            if (std::isfinite(bestScore)) {
              probe.best_bpm = bestBpm;
              probe.best_score = bestScore;
              probe.best_anchor_bpm = bestAnchorBpm;
              probe.best_boundary_hit = bestBoundaryHit;
            }
            probe.best_score_ratio_to_current =
                probe.best_score > 0.0 ? probe.best_score / currentScore : 0.0;
            probe.probe_138_score_ratio_to_current =
                probe.probe_138_score > 0.0
                    ? probe.probe_138_score / currentScore
                    : 0.0;
            probe.non_boundary_top_count =
                static_cast<std::size_t>(!probe.top1_boundary_hit) +
                static_cast<std::size_t>(!probe.top2_boundary_hit) +
                static_cast<std::size_t>(!probe.top3_boundary_hit);
            probe.top_span_bpm =
                (std::max)({probe.top1_bpm, probe.top2_bpm, probe.top3_bpm}) -
                (std::min)({probe.top1_bpm, probe.top2_bpm, probe.top3_bpm});
            probe.release_candidate =
                is_mir_policy_material_risk_local_exact_release_candidate(probe);
            const double targetMin =
                probe.selected_context_bpm -
                kMirPolicyMaterialRiskProbeTargetBelowMinBpm;
            const double targetMax =
                probe.selected_context_bpm -
                kMirPolicyMaterialRiskProbeTargetBelowMaxBpm;
            const bool bestNearTarget =
                probe.best_bpm >= targetMin && probe.best_bpm <= targetMax;
            const bool strongRelativeEvidence =
                probe.best_score_ratio_to_current >= 1.20;
            const bool nearThresholdEvidence =
                probe.best_score_ratio_to_current >= 1.15;
            probe.decision_class =
                probe.release_candidate
                    ? "target_family_release_candidate"
                    : bestNearTarget && strongRelativeEvidence
                    ? "target_family_visible"
                    : bestNearTarget && nearThresholdEvidence
                          ? "target_family_near_threshold"
                          : "target_family_not_visible";
            probe.reason =
                "read_only_material_risk_local_exact_probe";
            if (verboseLogs) {
              smart_tempo::log_hodgkinson_material_risk_local_exact_probe(
                  trackLabel.get_ptr(), probe);
            }
          }
          const auto& mirPolicyCandidates = mirPolicyMeasuredCandidates;
          // The policy board already exists for runtime selection. Emitting it
          // in Verbose mode adds no DSP work and makes the same run exactly
          // replayable through the shared native policy.
          if (verboseLogs) {
            for (std::size_t index = 0;
                 index < mirPolicyCandidates.size(); ++index) {
              const auto& measured = mirPolicyCandidates[index];
              const auto& candidate = measured.candidate;
              smart_tempo::
                  log_mir_policy_candidate_board_entry(
                      trackLabel.get_ptr(), trackKey, index,
                      "primary_board",
                      measured.cluster_bpm, measured.bpm,
                      measured.local_exact_score, measured.base_score,
                      measured.alias_classes.c_str(), candidate.support,
                      candidate.winner_support, candidate.score_sum,
                      candidate.best_combined_score, candidate.pulse_score,
                      candidate.pulse_section_support,
                      candidate.pulse_phase_vs,
                      candidate.pulse_axial_phase_vs,
                      candidate.continuous_score,
                      candidate.continuous_section_support,
                      candidate.continuous_score_ratio,
                      candidate.continuous_section_stability,
                      candidate.rows,
                      mirMeasuredCandidateBoard.segment_count);
            }
          }
          auto mirPolicyDecision =
              compute_mir_policy_decision(
                   std::span<const MirPolicyMeasuredCandidate>(
                       mirPolicyCandidates.data(),
                       mirPolicyCandidates.size()),
                   mirPrimarySelection,
                   hodgkinsonFullMirOnlyResult,
                   materialRiskLocalExactProbe.candidate
                      ? &materialRiskLocalExactProbe
                      : nullptr,
                  partialBarAggregateShadow.candidate
                      ? &partialBarAggregateShadow
                      : nullptr,
                  routingContext != nullptr
                      ? routingContext->primary_center_bpm
                      : 0.0,
                  routingContext != nullptr
                      ? routingContext->primary_spread_bpm
                      : 0.0,
                  routingContext != nullptr &&
                      routingContext->route_matched,
                  routingContext != nullptr &&
                      routingContext->is_generic_match,
                  smart_tempo::experimental::
                      kEnableMirPolicyPipeline);
          std::vector<smart_tempo::HodgkinsonPartialBarTopKExactCandidate>
              topKExactRows;
          const bool residualTopKResearch =
              verboseLogs &&
              smart_tempo::experimental::
                  kEnableHodgkinsonPartialBarTopKExactTelemetry &&
              (std::strcmp(
                   mirPolicyDecision.reason,
                   "measured_residual_downward_family_recovery_gate") == 0 ||
               std::strcmp(
                   mirPolicyDecision.reason,
                   "measured_exact_half_family_recovery_gate") == 0);
          const bool crossViewBridgeCandidate =
              has_partial_bar_cross_view_coarse_family(
                  partialBarAggregateAccumulator,
                  mirPolicyDecision,
                  std::span<const MirPolicyMeasuredCandidate>(
                      mirPolicyCandidates.data(), mirPolicyCandidates.size()),
                  kEnableMirPolicyPartialBarTopKExactRelease);
          if ((mirPolicyDecision.family_conflict_review_hold_candidate ||
               residualTopKResearch || crossViewBridgeCandidate) &&
              (kEnableMirPolicyPartialBarTopKExactRelease ||
               (verboseLogs && smart_tempo::experimental::
                                   kEnableHodgkinsonPartialBarTopKExactTelemetry))) {
            constexpr std::size_t kPartialBarTopKExactLimit = 16;
            topKExactRows = compute_partial_bar_topk_exact_telemetry(
                partialBarAggregateAccumulator, mirLocalExactCache,
                mirPolicyDecision.measured_winner_bpm,
                kPartialBarTopKExactLimit);
            try_apply_mir_policy_partial_bar_cross_view_release(
                mirPolicyDecision,
                std::span<const MirPolicyMeasuredCandidate>(
                    mirPolicyCandidates.data(), mirPolicyCandidates.size()),
                std::span<const smart_tempo::
                    HodgkinsonPartialBarTopKExactCandidate>(
                    topKExactRows.data(), topKExactRows.size()),
                kEnableMirPolicyPartialBarTopKExactRelease);
            try_apply_mir_policy_partial_bar_topk_exact_release(
                mirPolicyDecision,
                std::span<const smart_tempo::
                    HodgkinsonPartialBarTopKExactCandidate>(
                    topKExactRows.data(), topKExactRows.size()),
                routingContext != nullptr
                    ? routingContext->primary_center_bpm
                    : 0.0,
                routingContext != nullptr
                    ? routingContext->primary_spread_bpm
                    : 0.0,
                routingContext != nullptr && routingContext->route_matched,
                routingContext != nullptr && routingContext->is_generic_match,
                kEnableMirPolicyPartialBarTopKExactRelease);
          }
          // Release policy may consume these rows even when no Research
          // feature is enabled. Verbose logs must therefore serialize every
          // already-computed row so the terminal decision remains replayable.
          // This does not compute additional candidates or change policy.
          if (verboseLogs) {
            for (const auto& row : topKExactRows) {
              smart_tempo::log_hodgkinson_partial_bar_topk_exact_candidate(
                  trackLabel.get_ptr(), trackKey, row);
            }
          }
          enforce_mir_measured_candidate_invariant(
              mirPolicyDecision,
              std::span<const MirPolicyMeasuredCandidate>(
                  mirPolicyCandidates.data(), mirPolicyCandidates.size()),
              mirPrimarySelection,
              materialRiskLocalExactProbe.candidate
                  ? &materialRiskLocalExactProbe
                  : nullptr,
              partialBarAggregateShadow.candidate
                  ? &partialBarAggregateShadow
                  : nullptr,
              std::span<const smart_tempo::
                  HodgkinsonPartialBarTopKExactCandidate>(
                  topKExactRows.data(), topKExactRows.size()));
          if (verboseLogs &&
              mirPolicyDecision.enabled) {
            smart_tempo::
                log_mir_policy_decision(
                    trackLabel.get_ptr(),
                    mirPolicyDecision);
            smart_tempo::log_mir_local_exact_cache(
                trackLabel.get_ptr(), mirLocalExactCache.result_hits(),
                mirLocalExactCache.result_misses(),
                mirLocalExactCache.score_hits(),
                mirLocalExactCache.score_misses());
          }
          if (mirPrimarySelection.auto_candidate) {
            runtime_counters()
                .mir_primary_auto_candidates.fetch_add(
                    1, std::memory_order_relaxed);
          }
          if (mirPrimarySelection.review_hold) {
            runtime_counters()
                .mir_primary_review_holds.fetch_add(
                    1, std::memory_order_relaxed);
          }
          if (mirPrimarySelection.sparse_acapella_review_hold) {
            runtime_counters()
                .mir_primary_sparse_acapella_review_holds
                .fetch_add(1, std::memory_order_relaxed);
          }
          if (verboseLogs || mirPrimarySelection.review_hold) {
            smart_tempo::log_mir_primary_selection(
                trackLabel.get_ptr(), mirPrimarySelection);
          }
          if (smart_tempo::experimental::kEnableMirPrimaryRuntime) {
            if (mirPrimarySelection.auto_candidate &&
                !mirPrimarySelection.review_hold &&
                mirPrimarySelection.selected_bpm > 0.0 &&
                std::isfinite(mirPrimarySelection.selected_bpm)) {
              runtime_counters()
                  .mir_primary_runtime_applied.fetch_add(
                      1, std::memory_order_relaxed);
              finalBpm = mirPrimarySelection.selected_bpm;
              decisionLog.finalBpm = finalBpm;
              decisionLog.projectedBpm = finalBpm;
              decisionLog.projected.bpm = finalBpm;
              decisionLog.projected.multiplier = 1.0;
              decisionLog.projected.reason = "mir-primary";
              const double mirConfidence =
                  compute_mir_diagnostic_confidence(mirPrimarySelection);
              confidence = mirConfidence;
              m_diag.confidence = mirConfidence;
              decisionLog.confidence = mirConfidence;
              m_diag.method = "Hodgkinson/MIR";
              m_diag.decision_class = "mir-primary";
              m_diag.decision_note =
                  "Hodgkinson/MIR selected a measured primary candidate";
              m_diag.policy_reason = "mir_primary";
              m_diag.is_uncertain = false;
              m_diag.suppress_write = false;
            } else {
              finalBpm = 0.0;
              decisionLog.finalBpm = 0.0;
              decisionLog.projectedBpm = 0.0;
              decisionLog.projected.bpm = 0.0;
              decisionLog.projected.multiplier = 1.0;
              decisionLog.projected.reason =
                  mirPrimarySelection.review_hold
                      ? "mir-review-hold"
                      : "mir-no-candidate";
              m_diag.is_uncertain = true;
              m_diag.suppress_write = true;
              m_diag.method = "Hodgkinson/MIR review hold";
              m_diag.decision_class = mirPrimarySelection.review_hold
                                          ? "mir-review-hold"
                                          : "mir-no-candidate";
              m_diag.decision_note =
                  "Hodgkinson/MIR did not produce a writable measured candidate; BPM tag write suppressed";
              m_diag.policy_reason =
                  mirPrimarySelection.review_hold
                      ? "mir_review_hold"
                      : "mir_no_candidate";
              if (m_diag.error_reason.is_empty()) {
                m_diag.error_reason =
                    "Hodgkinson/MIR review hold: no writable measured candidate";
              }
            }
            if (smart_tempo::experimental::
                    kEnableMirPolicyRuntime &&
                mirPolicyDecision.enabled) {
              if (mirPolicyDecision
                      .family_conflict_review_hold_candidate ||
                  mirPolicyDecision.output_would_review_hold) {
                runtime_counters()
                    .mir_policy_review_holds.fetch_add(
                        1, std::memory_order_relaxed);
                if (mirPolicyDecision
                        .family_conflict_review_hold_candidate) {
                  runtime_counters()
                      .mir_policy_family_conflict_review_holds
                      .fetch_add(1, std::memory_order_relaxed);
                }
                finalBpm = 0.0;
                decisionLog.finalBpm = 0.0;
                decisionLog.projectedBpm = 0.0;
                decisionLog.projected.bpm = 0.0;
                decisionLog.projected.multiplier = 1.0;
                decisionLog.projected.reason =
                    "mir-review-hold";
                confidence = 0.0;
                m_diag.confidence = 0.0;
                decisionLog.confidence = 0.0;
                m_diag.method = "Hodgkinson/MIR";
                m_diag.is_uncertain = true;
                m_diag.suppress_write = true;
                m_diag.decision_class =
                    "mir-review-hold";
                m_diag.decision_note =
                    "Hodgkinson/MIR review hold: ";
                m_diag.decision_note +=
                    mirPolicyDecision.reason;
                m_diag.policy_reason =
                    "mir_review_hold";
                if (m_diag.error_reason.is_empty()) {
                  m_diag.error_reason =
                      "Hodgkinson/MIR policy review hold";
                }
              } else if ((mirPolicyDecision
                              .writer_override_candidate ||
                          mirPolicyDecision
                              .review_hold_promotion_candidate ||
                          is_mir_policy_keep_current_micro_upgrade(
                              mirPolicyDecision)) &&
                         mirPolicyDecision.output_would_write &&
                         ((mirPolicyDecision
                               .writer_override_candidate ||
                           mirPolicyDecision
                               .review_hold_promotion_candidate)
                              ? (mirPolicyDecision.output_bpm >
                                     0.0 &&
                                 std::isfinite(
                                     mirPolicyDecision
                                         .output_bpm))
                              : true)) {
                const bool keepCurrentMicroUpgrade =
                    is_mir_policy_keep_current_micro_upgrade(
                        mirPolicyDecision);
                const bool reviewHoldRelease =
                    mirPolicyDecision.current_review_hold &&
                    (mirPolicyDecision
                         .writer_override_candidate ||
                     mirPolicyDecision
                         .review_hold_promotion_candidate);
                if (mirPolicyDecision.writer_override_candidate) {
                  runtime_counters()
                      .mir_policy_writer_overrides
                      .fetch_add(1, std::memory_order_relaxed);
                }
                if (reviewHoldRelease) {
                  runtime_counters()
                      .mir_policy_review_hold_promotions
                      .fetch_add(1, std::memory_order_relaxed);
                }
                if (keepCurrentMicroUpgrade) {
                  runtime_counters()
                      .mir_policy_keep_current_micro_upgrades
                      .fetch_add(1, std::memory_order_relaxed);
                }
                finalBpm = effective_mir_policy_output_bpm(
                    mirPolicyDecision);
                decisionLog.finalBpm = finalBpm;
                decisionLog.projectedBpm = finalBpm;
                decisionLog.projected.bpm = finalBpm;
                decisionLog.projected.multiplier = 1.0;
                decisionLog.projected.reason =
                    "mir-policy";
                const double policyConfidence = (std::min)(
                    99.0, (std::max)(
                              50.0, 45.0 +
                                        mirPolicyDecision
                                                .candidate_support *
                                            45.0 +
                                        mirPolicyDecision
                                                .candidate_local_exact_score *
                                            20.0));
                confidence = policyConfidence;
                m_diag.confidence = policyConfidence;
                decisionLog.confidence = policyConfidence;
                m_diag.method = "Hodgkinson/MIR";
                m_diag.decision_class =
                    reviewHoldRelease
                        ? "mir-review-hold-release"
                        : mirPolicyDecision
                                  .writer_override_candidate
                        ? "mir-writer-override"
                        : keepCurrentMicroUpgrade
                              ? "mir-local-exact-upgrade"
                              : "mir-review-hold-release";
                m_diag.decision_note =
                    "Hodgkinson/MIR measured policy: ";
                m_diag.decision_note +=
                    mirPolicyDecision.reason;
                m_diag.policy_reason =
                    reviewHoldRelease
                        ? "mir_review_hold_release"
                        : mirPolicyDecision
                                  .writer_override_candidate
                        ? "mir_writer_override"
                        : keepCurrentMicroUpgrade
                              ? "mir_local_exact_upgrade"
                              : "mir_review_hold_release";
                m_diag.is_uncertain = false;
                m_diag.suppress_write = false;
              }
            }
          }
          if (!m_diag.suppress_write && finalBpm > 0.0 &&
              !mir_output_has_measured_candidate_witness(
                  finalBpm,
                  std::span<const MirPolicyMeasuredCandidate>(
                      mirPolicyCandidates.data(), mirPolicyCandidates.size()),
                  mirPrimarySelection,
                  materialRiskLocalExactProbe.candidate
                      ? &materialRiskLocalExactProbe
                      : nullptr,
                  partialBarAggregateShadow.candidate
                      ? &partialBarAggregateShadow
                      : nullptr,
                  std::span<const smart_tempo::
                      HodgkinsonPartialBarTopKExactCandidate>(
                      topKExactRows.data(), topKExactRows.size()))) {
            finalBpm = 0.0;
            decisionLog.finalBpm = 0.0;
            decisionLog.projectedBpm = 0.0;
            decisionLog.projected.bpm = 0.0;
            decisionLog.projected.multiplier = 1.0;
            decisionLog.projected.reason = "mir-writer-invariant-hold";
            confidence = 0.0;
            m_diag.confidence = 0.0;
            decisionLog.confidence = 0.0;
            m_diag.method = "Hodgkinson/MIR";
            m_diag.is_uncertain = true;
            m_diag.suppress_write = true;
            m_diag.decision_class = "mir-review-hold";
            m_diag.decision_note =
                "Hodgkinson/MIR final writer hold: no same-run measured "
                "candidate witness";
            m_diag.policy_reason = "mir_writer_invariant_hold";
            m_diag.error_reason =
                "Hodgkinson/MIR final writer invariant rejected an "
                "unwitnessed BPM";
          }
          if (m_diag.suppress_write || captureMeasuredCandidates) {
            const auto manualCandidates =
                prepare_mir_manual_review_candidates(
                    std::span<const MirPolicyMeasuredCandidate>(
                        mirPolicyCandidates.data(),
                        mirPolicyCandidates.size()),
                    mirPrimarySelection,
                    materialRiskLocalExactProbe.candidate
                        ? &materialRiskLocalExactProbe
                        : nullptr,
                    partialBarAggregateShadow.candidate
                        ? &partialBarAggregateShadow
                        : nullptr,
                    std::span<const smart_tempo::
                        HodgkinsonPartialBarTopKExactCandidate>(
                        topKExactRows.data(), topKExactRows.size()));
            m_diag.measured_bpm_candidates.reserve(
                manualCandidates.size());
            for (const auto& candidate : manualCandidates) {
              IBpmAnalyzer::MeasuredBpmCandidate item;
              item.bpm = candidate.bpm;
              item.evidence_score = candidate.evidence_score;
              item.support = candidate.support;
              item.winner_support = candidate.winner_support;
              item.local_exact_score = candidate.local_exact_score;
              item.evidence_rank = candidate.evidence_rank;
              item.alias_classes = candidate.alias_classes.c_str();
              item.sources = candidate.sources.c_str();
              m_diag.measured_bpm_candidates.push_back(std::move(item));
            }
          }
        }
      }

  }

  if (verboseLogs) {
    log_decision_summary(trackLabel.get_ptr(), decisionLog);
  }

  log_standard_decision(
      trackLabel.get_ptr(), decisionLog, m_diag.is_uncertain,
      m_diag.decision_class.is_empty() ? "unknown" : m_diag.decision_class.get_ptr());

  if (verboseLogs) {
    log_track_timing(
        trackLabel.get_ptr(),
        make_track_timing_log(metrics.decodeSec, metrics.onsetSec,
                              metrics.elapsed_seconds()));
  }

  return finalBpm;
}

void ModernBpmAnalyzer::reset_runtime_stats() noexcept {
  reset_runtime_counters();
}

ModernBpmAnalyzer::RuntimeStats ModernBpmAnalyzer::query_runtime_stats() noexcept {
  const auto snapshot = query_runtime_stats_snapshot();
  RuntimeStats out;
  out.tempo_allocations = snapshot.tempo_allocations;
  out.analyze_calls = snapshot.analyze_calls;
  out.hodgkinson_material_risk_candidates =
      snapshot.hodgkinson_material_risk_candidates;
  out.hodgkinson_material_risk_review_holds =
      snapshot.hodgkinson_material_risk_review_holds;
  out.mir_primary_auto_candidates =
      snapshot.mir_primary_auto_candidates;
  out.mir_primary_review_holds =
      snapshot.mir_primary_review_holds;
  out.mir_primary_sparse_acapella_review_holds =
      snapshot.mir_primary_sparse_acapella_review_holds;
  out.mir_primary_runtime_applied =
      snapshot.mir_primary_runtime_applied;
  out.mir_policy_writer_overrides =
      snapshot.mir_policy_writer_overrides;
  out.mir_policy_review_hold_promotions =
      snapshot.mir_policy_review_hold_promotions;
  out.mir_policy_keep_current_micro_upgrades =
      snapshot.mir_policy_keep_current_micro_upgrades;
  out.mir_policy_review_holds =
      snapshot.mir_policy_review_holds;
  out.mir_policy_family_conflict_review_holds =
      snapshot.mir_policy_family_conflict_review_holds;
  out.decode_seconds = snapshot.decode_seconds;
  out.onset_seconds = snapshot.onset_seconds;
  out.total_seconds = snapshot.total_seconds;
  return out;
}
