#include "stdafx.h"

#include "hodgkinson_tatum_probe.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace smart_tempo {
namespace {

constexpr double kMinCandidateBpm = 50.0;
constexpr double kMaxCandidateBpm = 220.0;
constexpr double kCandidateStepBpm = 0.5;
constexpr std::size_t kMinOdfSamples = 8;
constexpr std::size_t kMinPeakCount = 4;
constexpr std::array<int, 5> kTatumsPerBeat = {1, 2, 3, 4, 6};
constexpr std::array<HodgkinsonFamilyAliasOption, 11> kPulseFamilyAliases = {
    HodgkinsonFamilyAliasOption{0.0, 0.25, "quarter"},
    HodgkinsonFamilyAliasOption{0.0, 1.0 / 3.0, "one_third"},
    HodgkinsonFamilyAliasOption{0.0, 0.5, "half"},
    HodgkinsonFamilyAliasOption{0.0, 2.0 / 3.0, "two_thirds"},
    HodgkinsonFamilyAliasOption{0.0, 0.75, "three_quarters"},
    HodgkinsonFamilyAliasOption{0.0, 1.0, "direct"},
    HodgkinsonFamilyAliasOption{0.0, 4.0 / 3.0, "four_thirds"},
    HodgkinsonFamilyAliasOption{0.0, 1.5, "three_halves"},
    HodgkinsonFamilyAliasOption{0.0, 2.0, "double"},
    HodgkinsonFamilyAliasOption{0.0, 3.0, "triple"},
    HodgkinsonFamilyAliasOption{0.0, 4.0, "quadruple"},
};

[[nodiscard]] bool is_valid_positive(double value) noexcept {
  return value > 0.0 && std::isfinite(value);
}

[[nodiscard]] bool bpm_inside_range(double bpm,
                                    double minBpm,
                                    double maxBpm) noexcept {
  constexpr double kEpsilon = 0.05;
  return is_valid_positive(bpm) && is_valid_positive(minBpm) &&
         is_valid_positive(maxBpm) && maxBpm >= minBpm &&
         bpm + kEpsilon >= minBpm && bpm - kEpsilon <= maxBpm;
}

struct TatumFitCandidate {
  double bpm = 0.0;
  double error = std::numeric_limits<double>::infinity();
  double score = 0.0;
  std::size_t tatum_count = 0;
};

[[nodiscard]] double circular_distance(double normalizedPhase) noexcept {
  const double wrapped = normalizedPhase - std::floor(normalizedPhase);
  return std::min(wrapped, 1.0 - wrapped);
}

[[nodiscard]] double estimate_weighted_phase_offset(
    std::span<const HodgkinsonTatumPeak> peaks,
    double samplesPerTatum) noexcept {
  constexpr double kTwoPi = 6.283185307179586476925286766559;
  double sumSin = 0.0;
  double sumCos = 0.0;
  double weightSum = 0.0;

  for (const HodgkinsonTatumPeak& peak : peaks) {
    const double weight = std::max(0.0, peak.weight);
    if (weight <= 0.0) {
      continue;
    }
    const double phase =
        std::fmod(static_cast<double>(peak.odf_index), samplesPerTatum) /
        samplesPerTatum;
    const double angle = phase * kTwoPi;
    sumSin += weight * std::sin(angle);
    sumCos += weight * std::cos(angle);
    weightSum += weight;
  }

  if (weightSum <= 0.0) {
    return 0.0;
  }

  double angle = std::atan2(sumSin, sumCos);
  if (angle < 0.0) {
    angle += kTwoPi;
  }
  return (angle / kTwoPi) * samplesPerTatum;
}

[[nodiscard]] TatumFitCandidate evaluate_candidate(
    std::span<const HodgkinsonTatumPeak> peaks,
    std::size_t odfSampleCount,
    double candidateBpm,
    double segmentDurationSec,
    int tatumsPerBeat) noexcept {
  TatumFitCandidate candidate;
  candidate.bpm = candidateBpm;
  const double beats = (candidateBpm / 60.0) * segmentDurationSec;
  const double tatums = beats * static_cast<double>(tatumsPerBeat);
  if (!(tatums >= 1.0) || !std::isfinite(tatums)) {
    return candidate;
  }

  candidate.tatum_count = static_cast<std::size_t>(std::llround(tatums));
  if (candidate.tatum_count == 0) {
    return candidate;
  }

  const double samplesPerTatum =
      static_cast<double>(odfSampleCount) /
      static_cast<double>(candidate.tatum_count);
  if (!is_valid_positive(samplesPerTatum)) {
    return candidate;
  }

  const double offset = estimate_weighted_phase_offset(peaks, samplesPerTatum);
  double weightedDistance = 0.0;
  double weightSum = 0.0;

  for (const HodgkinsonTatumPeak& peak : peaks) {
    const double weight = std::max(0.0, peak.weight);
    if (weight <= 0.0) {
      continue;
    }
    const double normalized =
        (static_cast<double>(peak.odf_index) - offset) / samplesPerTatum;
    weightedDistance += weight * (2.0 * circular_distance(normalized));
    weightSum += weight;
  }

  if (weightSum <= 0.0) {
    return candidate;
  }

  candidate.error = weightedDistance / weightSum;
  candidate.score = std::clamp(1.0 - candidate.error, 0.0, 1.0);
  return candidate;
}

}  // namespace

HodgkinsonTatumProbeResult evaluate_hodgkinson_tatum_probe_gate(
    const HodgkinsonTatumProbeInput& input) noexcept {
  HodgkinsonTatumProbeResult result;
  result.enabled = input.enabled;
  result.input_bpm = input.input_bpm;
  result.candidate_bpm = input.input_bpm;
  result.segment_index = input.segment_index;
  result.segment_count = input.segment_count;
  result.segment_start_sec = input.segment_start_sec;
  result.segment_duration_sec = input.segment_duration_sec;
  result.onset_count = input.onset_count;
  result.odf_peak_count = input.odf_peak_count;

  if (!input.license_gate_cleared) {
    result.reason = "license_gate_not_cleared";
    return result;
  }
  if (!input.enabled) {
    result.reason = "disabled";
    return result;
  }
  if (!(input.input_bpm > 0.0) || !std::isfinite(input.input_bpm) ||
      !(input.sample_rate > 0.0) || !std::isfinite(input.sample_rate)) {
    result.reason = "invalid_input";
    return result;
  }
  if (input.onset_count < 8) {
    result.reason = "insufficient_onsets";
    return result;
  }

  result.reason = "not_evaluated";
  return result;
}

HodgkinsonTatumProbeResult evaluate_hodgkinson_tatum_segment_window(
    const HodgkinsonTatumSegmentInput& input) noexcept {
  HodgkinsonTatumProbeResult result;
  result.enabled = input.enabled;
  result.input_bpm = input.input_bpm;
  result.candidate_bpm = input.input_bpm;
  result.segment_index = input.segment_index;
  result.segment_count = input.segment_count;
  result.segment_start_sec = input.segment_start_sec;
  result.segment_duration_sec = input.segment_duration_sec;
  result.odf_peak_count = input.odf_peaks.size();

  if (!input.license_gate_cleared) {
    result.reason = "license_gate_not_cleared";
    return result;
  }
  if (!input.enabled) {
    result.reason = "disabled";
    return result;
  }
  if (!is_valid_positive(input.input_bpm) ||
      !is_valid_positive(input.segment_duration_sec) ||
      input.odf_sample_count < kMinOdfSamples) {
    result.reason = "invalid_input";
    return result;
  }
  if (input.odf_peaks.size() < kMinPeakCount) {
    result.reason = "insufficient_onsets";
    return result;
  }

  TatumFitCandidate best;
  for (double bpm = kMinCandidateBpm; bpm <= kMaxCandidateBpm;
       bpm += kCandidateStepBpm) {
    for (const int tatumsPerBeat : kTatumsPerBeat) {
      const TatumFitCandidate candidate = evaluate_candidate(
          input.odf_peaks, input.odf_sample_count, bpm,
          input.segment_duration_sec, tatumsPerBeat);
      if (candidate.score > best.score) {
        best = candidate;
      }
    }
  }

  if (!std::isfinite(best.score) || best.score <= 0.0 ||
      !std::isfinite(best.bpm) || best.bpm <= 0.0) {
    result.reason = "ambiguous_tatum_fit";
    return result;
  }

  result.candidate = true;
  result.candidate_bpm = best.bpm;
  result.loop_fit_score = best.score;
  result.loop_fit_confidence = best.score;
  result.tatum_count = best.tatum_count;
  result.consensus_support = input.odf_peaks.size();
  result.onset_count = input.odf_peaks.size();
  result.meter = "unknown";
  result.reason = "segment_window_probe";
  return result;
}

HodgkinsonOnlyFamilyResolverShadow
evaluate_hodgkinson_only_family_resolver_shadow(
    const HodgkinsonTatumProbeResult& result,
    double routeMinBpm,
    double routeMaxBpm) noexcept {
  HodgkinsonOnlyFamilyResolverShadow shadow;
  shadow.enabled = result.enabled;
  shadow.input_bpm =
      is_valid_positive(result.family_bpm) ? result.family_bpm
                                           : result.candidate_bpm;
  shadow.route_min_bpm = routeMinBpm;
  shadow.route_max_bpm = routeMaxBpm;
  shadow.family_score = result.family_score;
  shadow.family_score_dominance =
      result.runner_up_family_score > 0.0
          ? result.family_score / result.runner_up_family_score
          : 0.0;
  shadow.runner_up_score_ratio =
      result.family_score > 0.0
          ? result.runner_up_family_score / result.family_score
          : 0.0;
  shadow.consensus_support =
      result.segment_count > 0
          ? static_cast<double>(result.consensus_support) /
                static_cast<double>(result.segment_count)
          : 0.0;

  if (!result.enabled) {
    shadow.reason = "disabled";
    shadow.decision_class = "disabled";
    return shadow;
  }
  if (!result.candidate || !is_valid_positive(shadow.input_bpm)) {
    shadow.reason = "invalid_input";
    shadow.decision_class = "invalid_input";
    return shadow;
  }
  shadow.input_valid = true;
  if (!is_valid_positive(routeMinBpm) || !is_valid_positive(routeMaxBpm) ||
      routeMaxBpm < routeMinBpm) {
    shadow.reason = "invalid_route";
    shadow.decision_class = "invalid_route";
    return shadow;
  }

  const double routeCenter = (routeMinBpm + routeMaxBpm) * 0.5;
  double bestCenterDistance = std::numeric_limits<double>::infinity();
  for (const HodgkinsonFamilyAliasOption& alias : kPulseFamilyAliases) {
    const double projectedBpm = shadow.input_bpm * alias.multiplier;
    if (!bpm_inside_range(projectedBpm, routeMinBpm, routeMaxBpm)) {
      continue;
    }
    if (shadow.route_alias_option_count <
        shadow.route_alias_options.size()) {
      shadow.route_alias_options[shadow.route_alias_option_count] =
          HodgkinsonFamilyAliasOption{projectedBpm, alias.multiplier,
                                      alias.multiplier_class};
    }
    ++shadow.route_alias_option_count;

    const double centerDistance = std::abs(projectedBpm - routeCenter);
    if (centerDistance < bestCenterDistance) {
      bestCenterDistance = centerDistance;
      shadow.center_alias_bpm = projectedBpm;
      shadow.center_alias_multiplier = alias.multiplier;
      shadow.center_alias_class = alias.multiplier_class;
      shadow.center_distance_bpm = centerDistance;
    }
  }

  if (shadow.route_alias_option_count == 0) {
    shadow.reason = "no_route_alias";
    shadow.decision_class = "no_route_alias";
    return shadow;
  }

  if (shadow.route_alias_option_count == 1) {
    const HodgkinsonFamilyAliasOption& option = shadow.route_alias_options[0];
    shadow.unique_alias_bpm = option.bpm;
    shadow.unique_alias_multiplier = option.multiplier;
    shadow.unique_alias_class = option.multiplier_class;
  } else {
    shadow.reason = "ambiguous_route_alias";
    shadow.decision_class = "ambiguous_route_alias";
    return shadow;
  }

  const bool sourceEvidenceStrong =
      shadow.family_score > 0.0 &&
      (shadow.consensus_support >= 0.25 ||
       shadow.family_score_dominance >= 1.25 ||
       (shadow.runner_up_score_ratio > 0.0 &&
        shadow.runner_up_score_ratio <= 0.80));
  if (!sourceEvidenceStrong) {
    shadow.reason = "weak_source_evidence";
    shadow.decision_class = "weak_source_evidence";
    return shadow;
  }

  shadow.candidate = true;
  shadow.reason = "unique_route_alias_with_source_evidence";
  shadow.decision_class = "unique_route_candidate";
  return shadow;
}

std::vector<HodgkinsonFamilyAliasOption>
route_valid_hodgkinson_family_aliases(double familyBpm,
                                      double routeMinBpm,
                                      double routeMaxBpm) {
  std::vector<HodgkinsonFamilyAliasOption> aliases;
  if (!is_valid_positive(familyBpm) || !is_valid_positive(routeMinBpm) ||
      !is_valid_positive(routeMaxBpm) || routeMaxBpm < routeMinBpm) {
    return aliases;
  }

  for (const HodgkinsonFamilyAliasOption& alias : kPulseFamilyAliases) {
    const double projectedBpm = familyBpm * alias.multiplier;
    if (!bpm_inside_range(projectedBpm, routeMinBpm, routeMaxBpm)) {
      continue;
    }
    aliases.push_back(HodgkinsonFamilyAliasOption{
        projectedBpm, alias.multiplier, alias.multiplier_class});
  }
  return aliases;
}

}  // namespace smart_tempo
