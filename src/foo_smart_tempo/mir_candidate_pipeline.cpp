#include "mir_candidate_pipeline.h"
#include "mir_pulse_family_contract.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace smart_tempo::mir_pipeline {
namespace {
constexpr double kMirProductionMinBpm = 40.0;
constexpr double kMirProductionMaxBpm = 220.0;
constexpr double kMirPolicyCenterGateNormalizedLimit = 0.625;
constexpr double kMirPolicyCenterGateAbsoluteCapBpm = 15.0;
constexpr double kMirPolicyCenterChallengerMaxNormalizedDistance = 0.50;

[[nodiscard]] double mir_policy_center_gate_limit_bpm(
    double priorSpreadBpm) noexcept {
  if (std::isfinite(priorSpreadBpm) && priorSpreadBpm > 0.0) {
    return (std::min)(kMirPolicyCenterGateAbsoluteCapBpm,
                      priorSpreadBpm * kMirPolicyCenterGateNormalizedLimit);
  }
  return kMirPolicyCenterGateAbsoluteCapBpm;
}

[[nodiscard]] bool mir_policy_within_center_gate(
    double distanceBpm, double priorSpreadBpm) noexcept {
  return std::isfinite(distanceBpm) &&
         distanceBpm < mir_policy_center_gate_limit_bpm(priorSpreadBpm);
}
}
struct MirAliasOption {
  double multiplier = 1.0;
  const char* name = "direct";
  MirAlias bit = MirAlias::direct;
};

constexpr std::array<MirAliasOption, 11> kMirAliases{{
    {0.25, "quarter", MirAlias::quarter},
    {1.0 / 3.0, "one_third", MirAlias::one_third},
    {0.5, "half", MirAlias::half},
    {2.0 / 3.0, "two_thirds", MirAlias::two_thirds},
    {0.75, "three_quarters", MirAlias::three_quarters},
    {1.0, "direct", MirAlias::direct},
    {4.0 / 3.0, "four_thirds", MirAlias::four_thirds},
    {1.5, "three_halves", MirAlias::three_halves},
    {2.0, "double", MirAlias::double_time},
    {3.0, "triple", MirAlias::triple},
    {4.0, "quadruple", MirAlias::quadruple},
}};

static_assert([] {
  if (kMirAliases.size() != kMirPulseRelations.size()) {
    return false;
  }
  for (std::size_t index = 0; index < kMirAliases.size(); ++index) {
    if (kMirAliases[index].multiplier !=
        mir_pulse_relation_multiplier(kMirPulseRelations[index].relation)) {
      return false;
    }
  }
  return true;
}(), "MIR alias and measured pulse-family contracts diverged");

[[nodiscard]] double mir_alias_multiplier(const char* aliasClass) noexcept {
  if (aliasClass == nullptr) {
    return 0.0;
  }
  for (const auto& alias : kMirAliases) {
    if (std::strcmp(alias.name, aliasClass) == 0) {
      return alias.multiplier;
    }
  }
  return 0.0;
}

[[nodiscard]] std::size_t mir_segment_hit_count(
    const std::vector<uint8_t>& flags) noexcept {
  return static_cast<std::size_t>(
      std::count(flags.begin(), flags.end(), uint8_t{1}));
}

[[nodiscard]] uint64_t mir_segment_mask(
    const std::vector<uint8_t>& flags) noexcept {
  uint64_t mask = 0;
  const std::size_t count = (std::min)(flags.size(), std::size_t{64});
  for (std::size_t index = 0; index < count; ++index) {
    if (flags[index] != 0) {
      mask |= uint64_t{1} << index;
    }
  }
  return mask;
}

[[nodiscard]] char ascii_lower(char c) noexcept {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
}

[[nodiscard]] bool contains_ascii_case_insensitive(
    const char* haystack, const char* needle) noexcept {
  if (haystack == nullptr || needle == nullptr || *needle == '\0') {
    return false;
  }
  for (const char* h = haystack; *h != '\0'; ++h) {
    const char* hp = h;
    const char* np = needle;
    while (*hp != '\0' && *np != '\0' &&
           ascii_lower(*hp) == ascii_lower(*np)) {
      ++hp;
      ++np;
    }
    if (*np == '\0') {
      return true;
    }
  }
  return false;
}

[[nodiscard]] double cluster_material_bpm(double bpm) noexcept {
  constexpr double kClusterStep = 0.25;
  if (!(bpm > 0.0) || !std::isfinite(bpm)) {
    return 0.0;
  }
  return std::round(bpm / kClusterStep) * kClusterStep;
}

[[nodiscard]] bool mir_alias_is_production(uint16_t bit) noexcept {
  constexpr uint16_t kProduction =
      static_cast<uint16_t>(MirAlias::direct) |
      static_cast<uint16_t>(MirAlias::half) |
      static_cast<uint16_t>(MirAlias::double_time) |
      static_cast<uint16_t>(MirAlias::two_thirds) |
      static_cast<uint16_t>(MirAlias::three_halves) |
      static_cast<uint16_t>(MirAlias::three_quarters) |
      static_cast<uint16_t>(MirAlias::four_thirds);
  return (bit & kProduction) != 0;
}

[[nodiscard]] bool mir_alias_is_extreme(uint16_t bit) noexcept {
  constexpr uint16_t kExtreme =
      static_cast<uint16_t>(MirAlias::one_third) |
      static_cast<uint16_t>(MirAlias::triple) |
      static_cast<uint16_t>(MirAlias::quarter) |
      static_cast<uint16_t>(MirAlias::quadruple);
  return (bit & kExtreme) != 0;
}

[[nodiscard]] bool mir_has_alias(uint16_t bits,
                                     MirAlias alias) noexcept {
  return (bits & static_cast<uint16_t>(alias)) != 0;
}

[[nodiscard]] std::string mir_alias_text(uint16_t bits) {
  std::string out;
  for (const auto& alias : kMirAliases) {
    if ((bits & static_cast<uint16_t>(alias.bit)) == 0) {
      continue;
    }
    if (!out.empty()) {
      out += "|";
    }
    out += alias.name;
  }
  return out.empty() ? std::string("none") : out;
}

void initialize_mir_candidate_board(MirCandidateBoard& board,
                                        std::size_t segmentCount) {
  board.segment_count = segmentCount;
  board.raw_candidates.clear();
  board.raw_candidates.reserve(segmentCount > 0 ? segmentCount * 8 : 64);
  board.raw_candidate_index.clear();
  board.raw_candidate_index.reserve(segmentCount > 0 ? segmentCount * 8 : 64);
}

[[nodiscard]] uint64_t mir_raw_candidate_key(double clusteredBpm,
                                             uint16_t aliasBit) noexcept {
  const auto bpmTicks = static_cast<uint64_t>(
      std::llround(clusteredBpm * 4.0));
  return (bpmTicks << 16) | static_cast<uint64_t>(aliasBit);
}

MirRawCandidate& upsert_mir_raw_candidate(
    MirCandidateBoard& board,
    double bpm,
    const char* aliasClass,
    uint16_t aliasBit) {
  const double clustered = cluster_material_bpm(bpm);
  if (aliasBit != 0) {
    const auto found = board.raw_candidate_index.find(
        mir_raw_candidate_key(clustered, aliasBit));
    if (found != board.raw_candidate_index.end()) {
      return board.raw_candidates[found->second];
    }
  }
  for (auto& candidate : board.raw_candidates) {
    if (std::abs(candidate.bpm - clustered) < 0.001 &&
        std::strcmp(candidate.alias_class, aliasClass) == 0) {
      return candidate;
    }
  }
  MirRawCandidate candidate;
  candidate.bpm = clustered;
  candidate.alias_class = aliasClass;
  candidate.alias_bit = aliasBit;
  candidate.segments.assign(board.segment_count, uint8_t{0});
  candidate.winner_segments.assign(board.segment_count, uint8_t{0});
  board.raw_candidates.push_back(std::move(candidate));
  if (aliasBit != 0) {
    board.raw_candidate_index.emplace(
        mir_raw_candidate_key(clustered, aliasBit),
        board.raw_candidates.size() - 1);
  }
  return board.raw_candidates.back();
}

void add_mir_topk_candidate(
    MirCandidateBoard& board,
    const smart_tempo::HodgkinsonFullMirSegmentCandidate& candidate) {
  if (board.segment_count == 0 || !(candidate.candidate_bpm > 0.0) ||
      !std::isfinite(candidate.candidate_bpm) ||
      candidate.segment_index >= board.segment_count) {
    return;
  }
  for (const auto& alias : kMirAliases) {
    const double aliasBpm = candidate.candidate_bpm * alias.multiplier;
    if (!(aliasBpm >= 40.0 && aliasBpm <= 220.0) ||
        !std::isfinite(aliasBpm)) {
      continue;
    }
    auto& raw = upsert_mir_raw_candidate(
        board, aliasBpm, alias.name, static_cast<uint16_t>(alias.bit));
    if (raw.segments[candidate.segment_index] == 0) {
      raw.segments[candidate.segment_index] = 1;
      ++raw.segment_hits;
    }
    if (candidate.winner) {
      if (raw.winner_segments[candidate.segment_index] == 0) {
        raw.winner_segments[candidate.segment_index] = 1;
        ++raw.winner_segment_hits;
      }
      raw.winner_rows += 1;
    }
    raw.rows += 1;
    raw.score_sum += candidate.combined_score;
    raw.quant_sum += candidate.quantization_score;
    raw.meter_sum += candidate.meter_score;
    raw.best_combined_score =
        (std::max)(raw.best_combined_score, candidate.combined_score);
    raw.best_quantization_score =
        (std::max)(raw.best_quantization_score, candidate.quantization_score);
    raw.best_meter_score =
        (std::max)(raw.best_meter_score, candidate.meter_score);
  }
}

void add_mir_alias_pulse_evidence(
    MirCandidateBoard& board,
    const smart_tempo::HodgkinsonAliasPulseEvidence& evidence) {
  if (!(evidence.alias_bpm >= 40.0 && evidence.alias_bpm <= 220.0) ||
      !std::isfinite(evidence.alias_bpm)) {
    return;
  }
  uint16_t aliasBit = 0;
  for (const auto& alias : kMirAliases) {
    if (std::strcmp(alias.name, evidence.alias_class) == 0) {
      aliasBit = static_cast<uint16_t>(alias.bit);
      break;
    }
  }
  auto& raw = upsert_mir_raw_candidate(
      board, evidence.alias_bpm, evidence.alias_class, aliasBit);
  raw.pulse_score = (std::max)(raw.pulse_score, evidence.pulse_score);
  raw.pulse_section_support =
      (std::max)(raw.pulse_section_support, evidence.section_support);
  raw.pulse_phase_vs = (std::max)(raw.pulse_phase_vs, evidence.phase_vs);
  raw.pulse_axial_phase_vs =
      (std::max)(raw.pulse_axial_phase_vs, evidence.axial_phase_vs);
}

void add_mir_continuous_refinement_evidence(
    MirCandidateBoard& board,
    const smart_tempo::HodgkinsonContinuousRefinementEvidence& evidence) {
  if (!(evidence.best_bpm >= 40.0 && evidence.best_bpm <= 220.0) ||
      !std::isfinite(evidence.best_bpm)) {
    return;
  }
  uint16_t aliasBit = 0;
  for (const auto& alias : kMirAliases) {
    if (std::strcmp(alias.name, evidence.alias_class) == 0) {
      aliasBit = static_cast<uint16_t>(alias.bit);
      break;
    }
  }
  auto& raw = upsert_mir_raw_candidate(
      board, evidence.best_bpm, evidence.alias_class, aliasBit);
  raw.rows += 1;
  raw.score_sum += evidence.best_score;
  raw.best_combined_score =
      (std::max)(raw.best_combined_score, evidence.best_score);
  raw.best_quantization_score =
      (std::max)(raw.best_quantization_score, evidence.score_ratio);
  raw.best_meter_score =
      (std::max)(raw.best_meter_score, evidence.section_stability);
  raw.synthetic_support =
      (std::max)(raw.synthetic_support, evidence.section_support);
  raw.continuous_score =
      (std::max)(raw.continuous_score, evidence.best_score);
  raw.continuous_section_support =
      (std::max)(raw.continuous_section_support, evidence.section_support);
  raw.continuous_score_ratio =
      (std::max)(raw.continuous_score_ratio, evidence.score_ratio);
  raw.continuous_section_stability =
      (std::max)(raw.continuous_section_stability,
                 evidence.section_stability);
}

[[nodiscard]] smart_tempo::MirHighPulseIntervalConflictShadow
evaluate_mir_high_pulse_interval_conflict_shadow(
    const smart_tempo::HodgkinsonContinuousRefinementEvidence& evidence,
    double runtimeBpm,
    bool runtimeAvailable,
    bool reviewHoldContext) {
  smart_tempo::MirHighPulseIntervalConflictShadow shadow;
  shadow.enabled = true;
  shadow.runtime_available = runtimeAvailable;
  shadow.runtime_bpm = runtimeAvailable ? runtimeBpm : 0.0;
  shadow.phase_bpm =
      evidence.fine_best_bpm > 0.0 ? evidence.fine_best_bpm : evidence.best_bpm;
  shadow.interval_bpm = evidence.interval_fine_best_bpm;
  shadow.interval_iqr_bpm = evidence.interval_segment_iqr_bpm;
  shadow.interval_cluster_support =
      evidence.interval_segment_cluster1_support;
  shadow.interval_alias_support_050 =
      evidence.interval_segment_support_alias_050;

  const bool intervalValid = shadow.interval_bpm >= 40.0 &&
                             shadow.interval_bpm <= 220.0 &&
                             std::isfinite(shadow.interval_bpm);
  const bool phaseValid = shadow.phase_bpm >= 40.0 &&
                          shadow.phase_bpm <= 220.0 &&
                          std::isfinite(shadow.phase_bpm);
  if (!intervalValid) {
    shadow.decision_class = "no_candidate";
    shadow.reason = "no_interval_candidate";
    return shadow;
  }

  const bool stableInterval = shadow.interval_cluster_support >= 0.75 &&
                              shadow.interval_iqr_bpm <= 0.05;
  if (!stableInterval) {
    shadow.decision_class = "no_candidate";
    shadow.reason = "weak_interval_cluster";
    return shadow;
  }

  if (!runtimeAvailable) {
    if (reviewHoldContext) {
      shadow.candidate = true;
      shadow.review_candidate = true;
      shadow.decision_class = "review_candidate";
      shadow.reason = "stable_interval_review_candidate";
    } else {
      shadow.decision_class = "no_candidate";
      shadow.reason = "runtime_unavailable";
    }
    return shadow;
  }

  shadow.runtime_interval_delta =
      std::abs(shadow.interval_bpm - shadow.runtime_bpm);
  if (phaseValid) {
    shadow.runtime_phase_delta =
        std::abs(shadow.phase_bpm - shadow.runtime_bpm);
    const double intervalDirection = shadow.interval_bpm - shadow.runtime_bpm;
    const double phaseDirection = shadow.phase_bpm - shadow.runtime_bpm;
    shadow.same_direction =
        (intervalDirection > 0.0 && phaseDirection > 0.0) ||
        (intervalDirection < 0.0 && phaseDirection < 0.0);
  }

  if (shadow.interval_alias_support_050 > 0.05) {
    shadow.decision_class = "no_candidate";
    shadow.reason = "alias_supports_runtime";
    return shadow;
  }
  if (shadow.runtime_interval_delta < 0.50) {
    shadow.decision_class = "no_candidate";
    shadow.reason = "runtime_interval_delta_too_small";
    return shadow;
  }
  if (!phaseValid || shadow.runtime_phase_delta < 0.50) {
    shadow.decision_class = "no_candidate";
    shadow.reason = "runtime_phase_delta_too_small";
    return shadow;
  }
  if (!shadow.same_direction) {
    shadow.decision_class = "no_candidate";
    shadow.reason = "phase_interval_direction_mismatch";
    return shadow;
  }

  shadow.candidate = true;
  shadow.write_candidate = true;
  shadow.decision_class = "write_candidate_shadow";
  shadow.reason = "stable_interval_phase_runtime_conflict";
  return shadow;
}

[[nodiscard]] std::vector<MirCandidate> collapse_mir_candidates(
    const MirCandidateBoard& board) {
  std::vector<MirCandidate> out;
  out.reserve(board.raw_candidates.size());
  for (const auto& raw : board.raw_candidates) {
    auto existing = std::find_if(
        out.begin(), out.end(), [&](const MirCandidate& candidate) {
          return std::abs(candidate.bpm - raw.bpm) < 0.001;
        });
    if (existing == out.end()) {
      MirCandidate candidate;
      candidate.bpm = raw.bpm;
      out.push_back(candidate);
      existing = std::prev(out.end());
    }
    MirCandidate& candidate = *existing;
    candidate.alias_bits |= raw.alias_bit;
    const double segmentSupport =
        board.segment_count > 0
            ? static_cast<double>(raw.segment_hits) /
                  static_cast<double>(board.segment_count)
            : 0.0;
    const double winnerSupport =
        board.segment_count > 0
            ? static_cast<double>(raw.winner_segment_hits) /
                  static_cast<double>(board.segment_count)
            : 0.0;
    candidate.support =
        (std::max)(candidate.support,
                   (std::max)(segmentSupport, raw.synthetic_support));
    candidate.winner_support =
        (std::max)(candidate.winner_support,
                   (std::max)(winnerSupport, raw.synthetic_winner_support));
    candidate.score_sum += (std::min)(2.0, raw.score_sum);
    candidate.best_combined_score =
        (std::max)(candidate.best_combined_score, raw.best_combined_score);
    const double rows = raw.rows > 0 ? static_cast<double>(raw.rows) : 1.0;
    candidate.avg_quant =
        (std::max)(candidate.avg_quant, raw.quant_sum / rows);
    candidate.pulse_score =
        (std::max)(candidate.pulse_score, raw.pulse_score);
    candidate.pulse_section_support =
        (std::max)(candidate.pulse_section_support,
                   raw.pulse_section_support);
    candidate.pulse_phase_vs =
        (std::max)(candidate.pulse_phase_vs, raw.pulse_phase_vs);
    candidate.pulse_axial_phase_vs =
        (std::max)(candidate.pulse_axial_phase_vs,
                   raw.pulse_axial_phase_vs);
    candidate.continuous_score =
        (std::max)(candidate.continuous_score, raw.continuous_score);
    candidate.continuous_section_support =
        (std::max)(candidate.continuous_section_support,
                   raw.continuous_section_support);
    candidate.continuous_score_ratio =
        (std::max)(candidate.continuous_score_ratio,
                   raw.continuous_score_ratio);
    candidate.continuous_section_stability =
        (std::max)(candidate.continuous_section_stability,
                   raw.continuous_section_stability);
    candidate.rows += raw.rows;
  }
  return out;
}

[[nodiscard]] double mir_evidence_score(
    const MirCandidate& candidate) noexcept {
  return candidate.support * 4.0 + candidate.winner_support * 1.0 +
         candidate.best_combined_score * 10.0 +
         candidate.pulse_score * 4.0 +
         candidate.pulse_section_support * 4.0 +
         std::log1p((std::max)(0.0, candidate.score_sum)) * 1.5 +
         (mir_has_alias(candidate.alias_bits, MirAlias::direct) ? 0.7
                                                                       : 0.0);
}

[[nodiscard]] bool is_mir_high_144_material_risk_review_hold(
    const MirCandidate& selected,
    const char* lane,
    double routeMin,
    double routeMax,
    double routeCenter) noexcept {
  if (lane == nullptr || std::strcmp(lane, "general") != 0) {
    return false;
  }
  if (!(routeMin <= 144.0 && routeMax >= 144.0 && routeCenter >= 124.0 &&
        routeCenter <= 154.0)) {
    return false;
  }
  if (!(selected.bpm >= 143.0 && selected.bpm <= 145.0)) {
    return false;
  }
  if (!mir_has_alias(selected.alias_bits, MirAlias::direct)) {
    return false;
  }
  const bool harmonicAlias =
      mir_has_alias(selected.alias_bits, MirAlias::four_thirds) ||
      mir_has_alias(selected.alias_bits, MirAlias::three_halves);
  if (!harmonicAlias) {
    return false;
  }

  // primary can over-select a weak 144 BPM family when the underlying MIR material
  // has no independent pulse/continuous support. Treat this as review-only
  // rather than auto-writing a known ambiguous 144 BPM result.
  return selected.support >= 0.45 && selected.support <= 0.75 &&
         selected.winner_support <= 0.12 &&
         selected.best_combined_score <= 0.08 &&
         selected.pulse_score <= 0.05 &&
         selected.pulse_section_support <= 0.05 &&
         selected.continuous_score <= 0.05 &&
         selected.continuous_section_support <= 0.05;
}

[[nodiscard]] bool is_mir_high_family_conflict_review_hold(
    const MirCandidate& selected,
    const smart_tempo::HodgkinsonTatumProbeResult& result,
    double routeMin,
    double routeMax) noexcept {
  if (selected.bpm < 118.0 || selected.bpm > 122.0) {
    return false;
  }
  if (result.family_bpm < 165.0 || result.family_bpm > 176.0) {
    return false;
  }
  const double fitSegments =
      result.fit_segment_count > 0
          ? static_cast<double>(result.fit_segment_count)
          : (result.total_segment_count > 0
                 ? static_cast<double>(result.total_segment_count)
                 : static_cast<double>(result.segment_count));
  const double consensusSupport =
      fitSegments > 0.0
          ? static_cast<double>(result.consensus_support) / fitSegments
          : 0.0;

  const double runnerUpRatio =
      result.family_score > 0.0
          ? result.runner_up_family_score / result.family_score
          : 1.0;
  const double familyDominance =
      result.runner_up_family_score > 0.0
          ? result.family_score / result.runner_up_family_score
          : (result.family_score > 0.0 ? 99.0 : 0.0);

  // The MIR material points at a high-pulse family, but the selector chose a
  // route-compatible 120 BPM alias. That is not enough proof for correction,
  // but it is enough to avoid an automatic write with high confidence.
  return familyDominance >= 1.25 && runnerUpRatio <= 0.80 &&
         consensusSupport >= 0.30;
}

[[nodiscard]] bool is_mir_soft_center_conflict_review_hold(
    const MirCandidate& selected,
    const MirRoutingContext* routingContext,
    double routeMin,
    double routeMax,
    double routeCenter) noexcept {
  if (routingContext == nullptr || !routingContext->route_matched ||
      !(routeMin > 0.0 && routeMax > routeMin) ||
      !(selected.bpm >= kMirProductionMinBpm &&
        selected.bpm <= kMirProductionMaxBpm) ||
      !std::isfinite(selected.bpm)) {
    return false;
  }

  // Fallback-style and deliberately broad routes are not genre priors. Keep
  // them route-independent so untagged material is judged by MIR evidence only.
  const double routeWidth = routeMax - routeMin;
  if (routeWidth >= 100.0) {
    return false;
  }

  constexpr double kSoftRouteSlackBpm = 2.0;
  if (selected.bpm >= routeMin - kSoftRouteSlackBpm &&
      selected.bpm <= routeMax + kSoftRouteSlackBpm) {
    return false;
  }

  const double centerDistance =
      routeCenter > 0.0 ? std::abs(selected.bpm - routeCenter) : 0.0;
  const double edgeDistance =
      selected.bpm < routeMin ? routeMin - selected.bpm : selected.bpm - routeMax;

  const bool highPulseImmune =
      selected.bpm >= 155.0 && selected.support >= 0.70 &&
      selected.winner_support >= 0.55 &&
      (mir_has_alias(selected.alias_bits, MirAlias::direct) ||
       mir_has_alias(selected.alias_bits, MirAlias::double_time) ||
       mir_has_alias(selected.alias_bits, MirAlias::four_thirds));
  if (highPulseImmune) {
    return false;
  }

  // This is a no-write safety gate, not a hard candidate filter. It catches the
  // broad 65/130, 180/135 and 160/128 alias failures exposed by the focused
  // route-decoupling test while still allowing the measured candidate to remain
  // visible in telemetry for the future Soft Center selector.
  const bool strongOutOfFamily =
      edgeDistance >= 4.0 && centerDistance >= 18.0 &&
      selected.support >= 0.50 &&
      (selected.winner_support >= 0.18 ||
       selected.continuous_section_support >= 0.70 ||
       selected.pulse_section_support >= 0.40 ||
       selected.best_combined_score >= 0.08);

  const bool extremeOutlier =
      edgeDistance >= 18.0 && centerDistance >= 28.0 &&
      selected.support >= 0.35;

  return strongOutOfFamily || extremeOutlier;
}

[[nodiscard]] bool mir_shadow_has_alias(
    const smart_tempo::MirPrimarySelection& shadow,
    const char* aliasClass) noexcept {
  return contains_ascii_case_insensitive(shadow.alias_classes.c_str(),
                                         aliasClass);
}

[[nodiscard]] bool is_mir_soft_center_alias_review_hold(
    const smart_tempo::MirPrimarySelection& shadow) noexcept {
  if (!shadow.candidate || shadow.review_hold || !shadow.route_valid ||
      !(shadow.route_min_bpm > 0.0 &&
        shadow.route_max_bpm > shadow.route_min_bpm) ||
      !(shadow.route_center_bpm > 0.0) ||
      !(shadow.selected_local_exact_bpm >= kMirProductionMinBpm &&
        shadow.selected_local_exact_bpm <= kMirProductionMaxBpm) ||
      !std::isfinite(shadow.selected_local_exact_bpm)) {
    return false;
  }

  const double routeWidth = shadow.route_max_bpm - shadow.route_min_bpm;
  if (routeWidth >= 100.0) {
    return false;
  }

  const bool hasDirect = mir_shadow_has_alias(shadow, "direct");
  const bool hasHalf = mir_shadow_has_alias(shadow, "half");
  const bool hasQuarter = mir_shadow_has_alias(shadow, "quarter");
  const bool hasOneThird = mir_shadow_has_alias(shadow, "one_third");
  const bool hasTwoThirds = mir_shadow_has_alias(shadow, "two_thirds");
  const bool hasThreeQuarters =
      mir_shadow_has_alias(shadow, "three_quarters");
  const bool hasThreeHalves =
      mir_shadow_has_alias(shadow, "three_halves");
  const bool hasFourThirds =
      mir_shadow_has_alias(shadow, "four_thirds");
  const bool hasDouble = mir_shadow_has_alias(shadow, "double");

  const bool highPulseImmune =
      shadow.selected_local_exact_bpm >= 155.0 && shadow.support >= 0.70 &&
      shadow.winner_support >= 0.55 &&
      (hasDirect || hasDouble || hasFourThirds);

  const bool narrowUpperAliasConflict =
      highPulseImmune && routeWidth <= 55.0 &&
      shadow.route_center_bpm <= 145.0 &&
      shadow.selected_local_exact_bpm >= 168.0 &&
      shadow.selected_local_exact_bpm > shadow.route_max_bpm + 8.0 &&
      shadow.best_combined_score < 0.190 &&
      shadow.pulse_section_support <= 0.05 &&
      shadow.continuous_section_support <= 0.05 &&
      (hasDirect || hasFourThirds || hasThreeHalves);
  if (narrowUpperAliasConflict) {
    return true;
  }

  if (highPulseImmune) {
    return false;
  }

  const bool lowHarmonicAlias =
      shadow.selected_local_exact_bpm < 70.0 &&
      shadow.route_center_bpm >= 120.0 && shadow.support >= 0.50 &&
      (hasHalf || hasQuarter || hasOneThird);

  const bool weakLowerEdgeAlias =
      shadow.selected_local_exact_bpm < shadow.route_center_bpm - 14.0 &&
      shadow.selected_local_exact_bpm <= shadow.route_min_bpm + 1.0 &&
      shadow.support >= 0.65 && shadow.selected_local_exact_score > 0.0 &&
      shadow.selected_local_exact_score < 0.170 &&
      shadow.best_combined_score < 0.090 &&
      shadow.pulse_section_support <= 0.05 &&
      shadow.continuous_section_support <= 0.05 &&
      (hasDirect || hasTwoThirds || hasThreeQuarters || hasDouble);

  const bool highHarmonicAlias =
      shadow.selected_local_exact_bpm >= 155.0 &&
      shadow.route_center_bpm <= 145.0 && shadow.support >= 0.75 &&
      (hasThreeHalves || hasDirect) &&
      shadow.pulse_section_support <= 0.60 &&
      shadow.selected_local_exact_score > 0.0 &&
      shadow.selected_local_exact_score < 0.235 &&
      shadow.best_combined_score < 0.450;

  return lowHarmonicAlias || weakLowerEdgeAlias || highHarmonicAlias;
}

[[nodiscard]] double compute_mir_diagnostic_confidence(
    const smart_tempo::MirPrimarySelection& shadow) noexcept {
  if (!shadow.auto_candidate || shadow.review_hold || shadow.selected_bpm <= 0.0 ||
      !std::isfinite(shadow.selected_bpm)) {
    return 0.0;
  }

  const double support =
      std::clamp((std::max)({shadow.support, shadow.lane_best_support,
                             shadow.continuous_section_support,
                             shadow.pulse_section_support}),
                 0.0, 1.0);
  const double score =
      std::clamp(shadow.best_combined_score / 0.45, 0.0, 1.0);
  const double pulse = std::clamp(shadow.pulse_score / 0.40, 0.0, 1.0);
  const double continuous =
      std::clamp(shadow.continuous_score / 0.35, 0.0, 1.0);

  double confidence =
      30.0 + support * 50.0 + score * 8.0 + pulse * 6.0 + continuous * 6.0;
  if (shadow.winner_support >= 0.50) {
    confidence += 4.0;
  }
  return std::clamp(confidence, 0.0, 99.0);
}

[[nodiscard]] const char* classify_mir_lane(
    const MirCandidateBoard& board,
    const MirRoutingContext* routingContext,
    double routeCenter,
    double routeMax,
    MirLaneStats* laneStats) noexcept {
  MirLaneStats stats;
  for (const auto& raw : board.raw_candidates) {
    const auto segmentSupport =
        [&raw, &board]() noexcept {
          if (board.segment_count == 0 || raw.segments.empty()) {
            return 0.0;
          }
          const auto count = static_cast<std::size_t>(
              std::count(raw.segments.begin(), raw.segments.end(), uint8_t{1}));
          return static_cast<double>(count) /
                 static_cast<double>(board.segment_count);
        }();
    const double support = (std::max)(segmentSupport, raw.synthetic_support);
    stats.best_support = (std::max)(stats.best_support, support);
    if (raw.alias_bit == static_cast<uint16_t>(MirAlias::direct) &&
        raw.bpm >= 150.0) {
      stats.high_direct_support =
          (std::max)(stats.high_direct_support, support);
    }
    if (raw.bpm <= 90.0) {
      stats.low_support = (std::max)(stats.low_support, support);
    } else if (raw.bpm < 145.0) {
      stats.mid_support = (std::max)(stats.mid_support, support);
    } else {
      stats.high_support = (std::max)(stats.high_support, support);
    }
  }
  if (laneStats != nullptr) {
    *laneStats = stats;
  }

  const bool metadataLaneAllowed =
      routingContext != nullptr && routingContext->route_matched &&
      !routingContext->is_generic_match;
  const bool acapellaContext =
      metadataLaneAllowed &&
      (contains_ascii_case_insensitive(routingContext->source_genres,
                                       "acapella") ||
       contains_ascii_case_insensitive(routingContext->normalized_genres,
                                       "acapella") ||
       contains_ascii_case_insensitive(routingContext->matched_token,
                                       "acapella"));
  if (acapellaContext) {
    return "sparse_or_acapella";
  }
  const char* const matchedToken =
      metadataLaneAllowed ? routingContext->matched_token : nullptr;
  const bool hasMatchedToken =
      matchedToken != nullptr && matchedToken[0] != '\0' &&
      std::strcmp(matchedToken, "<none>") != 0;
  const char* const laneGenreContext =
      hasMatchedToken
          ? matchedToken
          : (metadataLaneAllowed ? routingContext->source_genres : nullptr);
  const bool lowGenreContext =
      laneGenreContext != nullptr &&
      (contains_ascii_case_insensitive(laneGenreContext, "chill") ||
       contains_ascii_case_insensitive(laneGenreContext, "ambient") ||
       contains_ascii_case_insensitive(laneGenreContext, "downtempo") ||
       contains_ascii_case_insensitive(laneGenreContext, "easy listening") ||
       contains_ascii_case_insensitive(laneGenreContext, "easylistening") ||
       contains_ascii_case_insensitive(laneGenreContext, "lounge") ||
       contains_ascii_case_insensitive(laneGenreContext, "new age") ||
       contains_ascii_case_insensitive(laneGenreContext, "newage") ||
       contains_ascii_case_insensitive(laneGenreContext, "acoustic"));
  // Folding policy is not pulse evidence. In particular, the unmatched
  // fallback uses mode=off and must still be free to classify a strong direct
  // high-pulse family from the measured board.
  if (routeCenter <= 95.0 || routeMax <= 110.0 || lowGenreContext) {
    return "low_pulse_context";
  }

  if (stats.high_direct_support >= 0.55 &&
      stats.high_support >= stats.mid_support * 0.85) {
    return "high_pulse_context";
  }
  if (stats.best_support >= 0.85 &&
      std::abs(stats.low_support - stats.mid_support) <= 0.12) {
    return "harmonic_conflict";
  }
  return "general";
}

[[nodiscard]] double mir_raw_support(const MirRawCandidate& candidate,
                                         std::size_t segmentCount) noexcept {
  if (segmentCount == 0 || candidate.segments.empty()) {
    return candidate.synthetic_support;
  }
  return (std::max)(static_cast<double>(candidate.segment_hits) /
                        static_cast<double>(segmentCount),
                    candidate.synthetic_support);
}

[[nodiscard]] double mir_raw_winner_support(
    const MirRawCandidate& candidate, std::size_t segmentCount) noexcept {
  if (segmentCount == 0 || candidate.winner_segments.empty()) {
    return candidate.synthetic_winner_support;
  }
  return (std::max)(static_cast<double>(candidate.winner_segment_hits) /
                        static_cast<double>(segmentCount),
                    candidate.synthetic_winner_support);
}

[[nodiscard]] bool mir_raw_alias_allowed(const MirRawCandidate& candidate,
                                              const char* lane) noexcept {
  return mir_alias_is_production(candidate.alias_bit) ||
         (std::strcmp(lane, "low_pulse_context") == 0 &&
          candidate.alias_bit == static_cast<uint16_t>(MirAlias::one_third) &&
          candidate.bpm <= 75.0);
}

[[nodiscard]] double score_mir_raw_conservative(
    const MirRawCandidate& candidate,
    std::size_t segmentCount,
    const char* lane,
    double routeMin,
    double routeMax,
    double routeCenter) noexcept {
  const double support = mir_raw_support(candidate, segmentCount);
  const double winnerSupport = mir_raw_winner_support(candidate, segmentCount);
  const double avgQuant = candidate.rows > 0
                              ? candidate.quant_sum / static_cast<double>(candidate.rows)
                              : 0.0;
  const bool direct = candidate.alias_bit == static_cast<uint16_t>(MirAlias::direct);
  const bool harmonicAlias =
      candidate.alias_bit == static_cast<uint16_t>(MirAlias::two_thirds) ||
      candidate.alias_bit == static_cast<uint16_t>(MirAlias::three_halves) ||
      candidate.alias_bit == static_cast<uint16_t>(MirAlias::three_quarters) ||
      candidate.alias_bit == static_cast<uint16_t>(MirAlias::four_thirds);

  double score = support * 9.0 + winnerSupport * 2.0 +
                 std::log1p((std::max)(0.0, candidate.score_sum)) * 0.8 +
                 candidate.best_combined_score * 2.5 + avgQuant * 0.4;
  score += direct ? 0.8 : harmonicAlias ? -0.2
                                        : mir_alias_is_extreme(candidate.alias_bit)
                                              ? -3.5
                                              : -1.0;
  if (routeCenter > 0.0) {
    score -= std::abs(candidate.bpm - routeCenter) *
             (std::strcmp(lane, "general") == 0 ? 0.060 : 0.035);
  }
  if (std::strcmp(lane, "high_pulse_context") == 0) {
    if (candidate.bpm >= 150.0) {
      score += 5.0;
    }
    if (direct) {
      score += 2.0;
    }
    if (candidate.bpm < 130.0) {
      score -= 4.0;
    }
  } else if (std::strcmp(lane, "sparse_or_acapella") == 0) {
    if (support < 0.18) {
      score -= 2.0;
    }
    if (routeCenter > 0.0) {
      const double centerDistance =
          (std::min)(std::abs(candidate.bpm - routeCenter), 36.0);
      score -= centerDistance * 0.010;
    }
    if (mir_alias_is_extreme(candidate.alias_bit)) {
      score -= 2.0;
    }
  }
  if (mir_alias_is_extreme(candidate.alias_bit)) {
    score -= 4.0;
  }
  if (support < 0.15) {
    score -= 3.0;
  }
  return score;
}

[[nodiscard]] const MirRawCandidate* select_mir_raw_conservative(
    const MirCandidateBoard& board,
    const char* lane,
    double routeMin,
    double routeMax,
    double routeCenter) noexcept {
  std::vector<const MirRawCandidate*> pool;
  pool.reserve(board.raw_candidates.size());
  for (const auto& candidate : board.raw_candidates) {
    if (mir_raw_alias_allowed(candidate, lane)) {
      pool.push_back(&candidate);
    }
  }
  if (pool.empty()) {
    for (const auto& candidate : board.raw_candidates) {
      pool.push_back(&candidate);
    }
  }
  const double supportFloor = std::strcmp(lane, "sparse_or_acapella") == 0 ? 0.08 : 0.15;
  std::vector<const MirRawCandidate*> supported;
  supported.reserve(pool.size());
  for (const auto* candidate : pool) {
    if (mir_raw_support(*candidate, board.segment_count) >= supportFloor) {
      supported.push_back(candidate);
    }
  }
  if (!supported.empty()) {
    pool = std::move(supported);
  }

  const MirRawCandidate* best = nullptr;
  double bestScore = -std::numeric_limits<double>::infinity();
  for (const auto* candidate : pool) {
    const double score = score_mir_raw_conservative(
        *candidate, board.segment_count, lane, routeMin, routeMax, routeCenter);
    const auto tie = std::make_tuple(
        mir_raw_support(*candidate, board.segment_count),
        mir_raw_winner_support(*candidate, board.segment_count),
        candidate->score_sum, -std::abs(candidate->bpm - routeCenter));
    const auto bestTie = best == nullptr
                             ? std::make_tuple(-1.0, -1.0, -1.0, -std::numeric_limits<double>::infinity())
                             : std::make_tuple(mir_raw_support(*best, board.segment_count),
                                               mir_raw_winner_support(*best, board.segment_count),
                                               best->score_sum,
                                               -std::abs(best->bpm - routeCenter));
    if (best == nullptr || score > bestScore + 1e-9 ||
        (std::abs(score - bestScore) <= 1e-9 && tie > bestTie)) {
      best = candidate;
      bestScore = score;
    }
  }
  return best;
}

[[nodiscard]] const MirCandidate* find_mir_cluster(
    const std::vector<MirCandidate>& candidates, double bpm) noexcept {
  const auto found = std::find_if(
      candidates.begin(), candidates.end(), [bpm](const MirCandidate& candidate) {
        return std::abs(candidate.bpm - bpm) < 0.001;
      });
  return found != candidates.end() ? &*found : nullptr;
}

[[nodiscard]] double score_mir_cluster_conservative(
    const MirCandidate& candidate,
    const char* lane,
    double routeMin,
    double routeMax,
    double routeCenter) noexcept {
  const bool direct = mir_has_alias(candidate.alias_bits, MirAlias::direct);
  const uint16_t productionBits =
      static_cast<uint16_t>(MirAlias::direct) |
      static_cast<uint16_t>(MirAlias::half) |
      static_cast<uint16_t>(MirAlias::double_time) |
      static_cast<uint16_t>(MirAlias::two_thirds) |
      static_cast<uint16_t>(MirAlias::three_halves) |
      static_cast<uint16_t>(MirAlias::three_quarters) |
      static_cast<uint16_t>(MirAlias::four_thirds);
  double score = candidate.support * 8.0 + candidate.winner_support * 1.5 +
                 std::log1p((std::max)(0.0, candidate.score_sum)) * 1.5 +
                 candidate.best_combined_score * 3.0 +
                 (std::min)(3, std::popcount(static_cast<unsigned>(
                                    candidate.alias_bits & productionBits))) *
                     0.7 +
                 (direct ? 1.2 : 0.0);
  if (routeCenter > 0.0) {
    score -= std::abs(candidate.bpm - routeCenter) * 0.040;
  }
  if (std::strcmp(lane, "low_pulse_context") == 0) {
    score -= std::abs(candidate.bpm - 68.0) * 0.065;
    if (candidate.bpm >= 48.0 && candidate.bpm <= 86.0) {
      score += 2.5;
    } else if (candidate.bpm > 115.0) {
      score -= 7.0;
    }
    if (mir_has_alias(candidate.alias_bits, MirAlias::one_third) ||
        mir_has_alias(candidate.alias_bits, MirAlias::two_thirds) ||
        mir_has_alias(candidate.alias_bits, MirAlias::three_quarters)) {
      score += 0.8;
    }
  } else if (std::strcmp(lane, "high_pulse_context") == 0) {
    if (candidate.bpm >= 150.0 && direct) {
      score += 4.0;
    } else if (candidate.bpm >= 145.0) {
      score += 1.0;
    }
    if (candidate.bpm < 118.0) {
      score -= 4.0;
    }
  } else if (std::strcmp(lane, "harmonic_conflict") == 0) {
    if (routeCenter > 0.0) {
      score -= std::abs(candidate.bpm - routeCenter) * 0.10;
    }
    if (direct) {
      score += 0.8;
    }
  } else if (std::strcmp(lane, "sparse_or_acapella") == 0) {
    if (candidate.support < 0.12) {
      score -= 2.0;
    }
    if (routeCenter > 0.0) {
      score -= std::abs(candidate.bpm - routeCenter) * 0.05;
    }
  }
  return score;
}

[[nodiscard]] const MirCandidate* best_mir_cluster_candidate(
    const std::vector<MirCandidate>& candidates,
    const std::vector<const MirCandidate*>& pool,
    const char* lane,
    double routeMin,
    double routeMax,
    double routeCenter) noexcept {
  const std::vector<const MirCandidate*>* searchPool = &pool;
  std::vector<const MirCandidate*> fallback;
  if (searchPool->empty()) {
    fallback.reserve(candidates.size());
    for (const auto& candidate : candidates) {
      fallback.push_back(&candidate);
    }
    searchPool = &fallback;
  }
  const MirCandidate* best = nullptr;
  double bestScore = -std::numeric_limits<double>::infinity();
  for (const auto* candidate : *searchPool) {
    const double score = score_mir_cluster_conservative(
        *candidate, lane, routeMin, routeMax, routeCenter);
    const auto tie = std::make_tuple(candidate->support, candidate->winner_support,
                                     candidate->score_sum,
                                     -std::abs(candidate->bpm - routeCenter));
    const auto bestTie = best == nullptr
                             ? std::make_tuple(-1.0, -1.0, -1.0,
                                               -std::numeric_limits<double>::infinity())
                             : std::make_tuple(best->support, best->winner_support,
                                               best->score_sum,
                                               -std::abs(best->bpm - routeCenter));
    if (best == nullptr || score > bestScore + 1e-9 ||
        (std::abs(score - bestScore) <= 1e-9 && tie > bestTie)) {
      best = candidate;
      bestScore = score;
    }
  }
  return best;
}

[[nodiscard]] const MirCandidate* select_mir_low_pulse(
    const std::vector<MirCandidate>& candidates) noexcept {
  const auto hasAnyAlias = [](const MirCandidate& c,
                              uint16_t bits) noexcept {
    return (c.alias_bits & bits) != 0;
  };
  const auto aliasCount = [](const MirCandidate& c,
                             uint16_t bits) noexcept {
    return std::popcount(static_cast<unsigned>(c.alias_bits & bits));
  };
  const auto aliasBitsExactly = [](const MirCandidate& c,
                                   uint16_t bits) noexcept {
    return c.alias_bits == bits;
  };
  const auto lowEvidence = [&](const MirCandidate* c) noexcept {
    return c->support * 4.0 + c->winner_support +
           c->best_combined_score * 10.0 + c->pulse_score * 4.0 +
           c->pulse_section_support * 4.0 +
           std::log1p((std::max)(0.0, c->score_sum)) * 1.5 +
           (mir_has_alias(c->alias_bits, MirAlias::direct) ? 0.7
                                                                  : 0.0);
  };
  const auto bestBy = [&](const std::vector<const MirCandidate*>& pool,
                          const auto& scoreFn) -> const MirCandidate* {
    const MirCandidate* best = nullptr;
    double bestScore = -std::numeric_limits<double>::infinity();
    for (const auto* candidate : pool) {
      const double score = scoreFn(candidate);
      if (best == nullptr || score > bestScore) {
        best = candidate;
        bestScore = score;
      }
    }
    return best;
  };

  // Port of the validated offline lane_composite_primary low-pulse chain:
  // threshold_best -> targeted_rescue_v3 -> targeted_rescue_v4.
  std::vector<const MirCandidate*> upper;
  for (const auto& candidate : candidates) {
    if (candidate.bpm >= 95.0 && candidate.bpm <= 125.0 &&
        candidate.support >= 0.70 && candidate.pulse_score >= 0.35 &&
        candidate.pulse_section_support >= 0.35) {
      upper.push_back(&candidate);
    }
  }
  if (!upper.empty()) {
    return bestBy(upper, [&](const MirCandidate* c) noexcept {
      return c->pulse_score * 8.0 + c->pulse_section_support * 6.0 +
             c->support * 2.0 +
             (mir_has_alias(c->alias_bits, MirAlias::direct) ? 1.0
                                                                    : 0.0) -
             std::abs(c->bpm - 110.0) * 1e-6;
    });
  }

  std::vector<const MirCandidate*> thresholdPool;
  for (const auto& candidate : candidates) {
    if (candidate.bpm >= 45.0 && candidate.bpm <= 90.0) {
      thresholdPool.push_back(&candidate);
    }
  }
  if (thresholdPool.empty()) {
    for (const auto& candidate : candidates) {
      thresholdPool.push_back(&candidate);
    }
  }
  const MirCandidate* selected =
      bestBy(thresholdPool, [&](const MirCandidate* c) noexcept {
        double out = c->support * 3.0 + c->winner_support +
                     c->pulse_score * 8.0 +
                     c->pulse_section_support * 5.0 +
                     c->best_combined_score * 4.0 +
                     std::log1p((std::max)(0.0, c->score_sum)) -
                     std::abs(c->bpm - 60.0) * 0.08;
        if (hasAnyAlias(*c, static_cast<uint16_t>(MirAlias::two_thirds) |
                                static_cast<uint16_t>(
                                    MirAlias::three_quarters) |
                                static_cast<uint16_t>(
                                    MirAlias::one_third))) {
          out += 1.0;
        }
        if (c->bpm > 90.0) {
          out -= 4.0;
        }
        return out;
      });
  if (selected == nullptr) {
    return nullptr;
  }

  const uint16_t lowerMidAliases =
      static_cast<uint16_t>(MirAlias::half) |
      static_cast<uint16_t>(MirAlias::three_quarters) |
      static_cast<uint16_t>(MirAlias::two_thirds);
  const uint16_t twoThirdsBit =
      static_cast<uint16_t>(MirAlias::two_thirds);
  const auto lowEvidenceWithAnchor =
      [&](const MirCandidate* c, double anchor) noexcept {
        return lowEvidence(c) - std::abs(c->bpm - anchor) * 0.045;
      };

  const double baseBpm = selected->bpm;
  if (baseBpm >= 58.0 && baseBpm <= 61.0) {
    std::vector<const MirCandidate*> mid;
    for (const auto& candidate : candidates) {
      if (candidate.bpm >= 68.0 && candidate.bpm <= 82.0 &&
          (candidate.support >= 0.45 ||
           (candidate.best_combined_score >= 0.15 &&
            candidate.score_sum >= 0.55))) {
        mid.push_back(&candidate);
      }
    }
    if (!mid.empty()) {
      const MirCandidate* selectedMid =
          bestBy(mid, [&](const MirCandidate* c) noexcept {
            return lowEvidenceWithAnchor(c, 74.0);
          });
      if (selectedMid != nullptr && selectedMid->bpm >= 79.0 &&
          selectedMid->bpm <= 81.0 &&
          aliasBitsExactly(*selectedMid, twoThirdsBit)) {
        std::vector<const MirCandidate*> lowerMid;
        for (const auto& candidate : candidates) {
          if (candidate.bpm >= 69.5 && candidate.bpm <= 72.5 &&
              aliasCount(candidate, lowerMidAliases) >= 2 &&
              (lowEvidence(&candidate) >= lowEvidence(selectedMid) * 0.80 ||
               (candidate.winner_support >= 0.24 &&
                candidate.score_sum >= 1.0 &&
                candidate.pulse_score >= 0.20))) {
            lowerMid.push_back(&candidate);
          }
        }
        if (!lowerMid.empty()) {
          selected = bestBy(lowerMid, [&](const MirCandidate* c) noexcept {
            return lowEvidenceWithAnchor(c, 71.0);
          });
        } else {
          selected = selectedMid;
        }
      } else {
        selected = selectedMid;
      }
    }
  } else if (baseBpm >= 56.0 && baseBpm <= 58.0) {
    std::vector<const MirCandidate*> midLow;
    for (const auto& candidate : candidates) {
      if (candidate.bpm >= 63.0 && candidate.bpm <= 66.5 &&
          (mir_has_alias(candidate.alias_bits, MirAlias::direct) ||
           candidate.best_combined_score >= 0.16) &&
          candidate.support >= 0.10) {
        midLow.push_back(&candidate);
      }
    }
    if (!midLow.empty()) {
      selected = bestBy(midLow, [&](const MirCandidate* c) noexcept {
        return lowEvidenceWithAnchor(c, 65.0);
      });
    }
  } else if (baseBpm >= 64.0 && baseBpm <= 67.0) {
    std::vector<const MirCandidate*> deepLow;
    for (const auto& candidate : candidates) {
      if (candidate.bpm >= 50.0 && candidate.bpm <= 53.0 &&
          (candidate.pulse_score >= 0.22 ||
           candidate.best_combined_score >= 0.15) &&
          hasAnyAlias(candidate, static_cast<uint16_t>(MirAlias::one_third) |
                                   static_cast<uint16_t>(
                                       MirAlias::quarter))) {
        deepLow.push_back(&candidate);
      }
    }
    if (!deepLow.empty()) {
      selected = bestBy(deepLow, [&](const MirCandidate* c) noexcept {
        return lowEvidenceWithAnchor(c, 52.0);
      });
    }
  } else if (baseBpm >= 79.0 && baseBpm <= 81.0 &&
             aliasBitsExactly(*selected, twoThirdsBit)) {
    std::vector<const MirCandidate*> lowerMid;
    for (const auto& candidate : candidates) {
      if (candidate.bpm >= 69.5 && candidate.bpm <= 72.5 &&
          aliasCount(candidate, lowerMidAliases) >= 2 &&
          (candidate.support >= 0.30 ||
           candidate.best_combined_score >= 0.15 ||
           candidate.score_sum >= 0.60)) {
        lowerMid.push_back(&candidate);
      }
    }
    if (!lowerMid.empty()) {
      const MirCandidate* bestLower =
          bestBy(lowerMid, [&](const MirCandidate* c) noexcept {
            return lowEvidenceWithAnchor(c, 71.0);
          });
      if (bestLower != nullptr &&
          lowEvidence(selected) <= lowEvidence(bestLower) * 1.75) {
        selected = bestLower;
      }
    }
  } else if (baseBpm >= 86.0 && baseBpm <= 90.0) {
    std::vector<const MirCandidate*> upperDirect;
    for (const auto& candidate : candidates) {
      if (candidate.bpm >= 98.0 && candidate.bpm <= 102.0 &&
          mir_has_alias(candidate.alias_bits, MirAlias::direct) &&
          candidate.support >= 0.60 &&
          candidate.best_combined_score >= 0.18) {
        upperDirect.push_back(&candidate);
      }
    }
    if (!upperDirect.empty()) {
      selected = bestBy(upperDirect, [&](const MirCandidate* c) noexcept {
        return c->support * 100.0 + c->winner_support * 10.0 +
               c->best_combined_score;
      });
    }
  }

  if (selected != nullptr && selected->bpm >= 50.0 &&
      selected->bpm <= 53.0) {
    std::vector<const MirCandidate*> veryLow;
    for (const auto& candidate : candidates) {
      if (candidate.bpm >= 48.5 && candidate.bpm <= 50.5 &&
          hasAnyAlias(candidate, static_cast<uint16_t>(MirAlias::one_third) |
                                   static_cast<uint16_t>(
                                       MirAlias::direct)) &&
          candidate.support >= 0.50 &&
          candidate.support >= selected->support + 0.30) {
        veryLow.push_back(&candidate);
      }
    }
    if (!veryLow.empty()) {
      selected = bestBy(veryLow, [&](const MirCandidate* c) noexcept {
        return c->support * 100.0 + c->winner_support * 10.0 +
               c->best_combined_score;
      });
    } else if (selected->bpm <= 51.5 &&
               mir_has_alias(selected->alias_bits, MirAlias::quarter) &&
               selected->support < 0.35) {
      std::vector<const MirCandidate*> upper;
      for (const auto& candidate : candidates) {
        if (candidate.bpm >= 98.0 && candidate.bpm <= 102.0 &&
            hasAnyAlias(candidate,
                        static_cast<uint16_t>(MirAlias::direct) |
                            static_cast<uint16_t>(
                                MirAlias::three_quarters)) &&
            candidate.support >= 0.60) {
          upper.push_back(&candidate);
        }
      }
      if (!upper.empty()) {
        selected = bestBy(upper, [&](const MirCandidate* c) noexcept {
          return c->support * 100.0 + c->winner_support * 10.0 +
                 c->best_combined_score;
        });
      }
    }
  }
  return selected;
}

[[nodiscard]] const MirCandidate*
select_mir_low_pulse_continuous_center_rescue(
    const std::vector<MirCandidate>& candidates,
    const MirCandidate* selected,
    double routeMin,
    double routeMax) noexcept {
  if (selected == nullptr || selected->bpm > 90.0) {
    return nullptr;
  }
  const double routeCenter =
      routeMin > 0.0 && routeMax > routeMin ? (routeMin + routeMax) * 0.5
                                            : 99.0;
  const MirCandidate* best = nullptr;
  for (const auto& candidate : candidates) {
    if (!(candidate.bpm >= 95.0 && candidate.bpm <= 102.0) ||
        std::abs(candidate.bpm - selected->bpm) < 10.0 ||
        candidate.continuous_score_ratio < 1.50 ||
        candidate.continuous_section_support < 0.80 ||
        candidate.continuous_section_stability < 0.70) {
      continue;
    }
    if (best == nullptr ||
        std::make_tuple(candidate.continuous_score_ratio,
                        candidate.continuous_section_support,
                        candidate.continuous_section_stability,
                        candidate.continuous_score,
                        -std::abs(candidate.bpm - routeCenter)) >
            std::make_tuple(best->continuous_score_ratio,
                            best->continuous_section_support,
                            best->continuous_section_stability,
                            best->continuous_score,
                            -std::abs(best->bpm - routeCenter))) {
      best = &candidate;
    }
  }
  return best;
}

[[nodiscard]] const MirCandidate* select_mir_harmonic_soft(
    const std::vector<MirCandidate>& candidates,
  double routeMin,
  double routeMax,
  double routeCenter) noexcept {
  std::vector<const MirCandidate*> pool;
  for (const auto& candidate : candidates) {
    if (candidate.bpm >= kMirProductionMinBpm &&
        candidate.bpm <= kMirProductionMaxBpm) {
      pool.push_back(&candidate);
    }
  }
  const MirCandidate* best = nullptr;
  double bestScore = -std::numeric_limits<double>::infinity();
  for (const auto* candidate : pool) {
    double score = candidate->support * 5.0 +
                   candidate->winner_support * 0.8 +
                   candidate->best_combined_score * 14.0 +
                   candidate->pulse_score * 8.0 +
                   candidate->pulse_section_support * 5.0 +
                   std::log1p((std::max)(0.0, candidate->score_sum)) * 0.4;
    if (routeCenter > 0.0) {
      score -= std::abs(candidate->bpm - routeCenter) * 0.035;
    }
    if (mir_has_alias(candidate->alias_bits, MirAlias::direct)) {
      score += 0.4;
    }
    if (mir_has_alias(candidate->alias_bits, MirAlias::two_thirds) ||
        mir_has_alias(candidate->alias_bits,
                          MirAlias::three_quarters) ||
        mir_has_alias(candidate->alias_bits, MirAlias::three_halves) ||
        mir_has_alias(candidate->alias_bits, MirAlias::four_thirds)) {
      score += 0.4;
    }
    if (routeCenter > 0.0 && candidate->bpm > routeCenter + 20.0) {
      score -= 1.2;
    }
    if (best == nullptr || score > bestScore) {
      best = candidate;
      bestScore = score;
    }
  }
  return best;
}

[[nodiscard]] const MirCandidate* select_mir_general_144_soft(
    const std::vector<MirCandidate>& candidates,
    const char* lane,
    double routeMin,
  double routeMax,
  double routeCenter) noexcept {
  std::vector<const MirCandidate*> pool;
  for (const auto& candidate : candidates) {
    if (candidate.bpm >= kMirProductionMinBpm &&
        candidate.bpm <= kMirProductionMaxBpm) {
      pool.push_back(&candidate);
    }
  }
  if (pool.empty()) {
    for (const auto& candidate : candidates) {
      pool.push_back(&candidate);
    }
  }
  const MirCandidate* baseline = best_mir_cluster_candidate(
      candidates, pool, lane, routeMin, routeMax, routeCenter);
  if (baseline == nullptr || routeCenter >= 150.0) {
    return baseline;
  }
  const bool baseline144 =
      baseline->bpm >= 143.0 && baseline->bpm <= 145.0 &&
      (mir_has_alias(baseline->alias_bits, MirAlias::three_halves) ||
       mir_has_alias(baseline->alias_bits, MirAlias::four_thirds));
  if (!baseline144) {
    return baseline;
  }

  double bestNon144Evidence = 0.0;
  for (const auto* candidate : pool) {
    if (candidate->bpm >= 126.0 && candidate->bpm <= 141.5) {
      bestNon144Evidence =
          (std::max)(bestNon144Evidence,
                     candidate->best_combined_score +
                         candidate->pulse_section_support * 0.12);
    }
  }
  const MirCandidate* best = nullptr;
  double bestScore = -std::numeric_limits<double>::infinity();
  for (const auto* candidate : pool) {
    if (!(candidate->bpm >= 126.0 && candidate->bpm <= 141.5)) {
      continue;
    }
    double score = candidate->support * 5.5 +
                   candidate->winner_support * 1.0 +
                   candidate->best_combined_score * 15.0 +
                   candidate->pulse_score * 5.0 +
                   candidate->pulse_section_support * 6.0 +
                   std::log1p((std::max)(0.0, candidate->score_sum)) * 0.7;
    if (mir_has_alias(candidate->alias_bits, MirAlias::direct)) {
      score += 0.5;
    }
    if (mir_has_alias(candidate->alias_bits, MirAlias::two_thirds) ||
        mir_has_alias(candidate->alias_bits,
                          MirAlias::three_quarters) ||
        mir_has_alias(candidate->alias_bits, MirAlias::three_halves) ||
        mir_has_alias(candidate->alias_bits, MirAlias::four_thirds)) {
      score += 0.2;
    }
    if (routeCenter > 0.0) {
      score -= std::abs(candidate->bpm - routeCenter) * 0.025;
    }
    const double evidence =
        candidate->best_combined_score + candidate->pulse_section_support * 0.12;
    const bool candidate144 =
        candidate->bpm >= 143.0 && candidate->bpm <= 145.0 &&
        (mir_has_alias(candidate->alias_bits, MirAlias::three_halves) ||
         mir_has_alias(candidate->alias_bits, MirAlias::four_thirds));
    if (candidate144 && bestNon144Evidence >= evidence * 1.20) {
      score -= 3.5;
    }
    if (best == nullptr || score > bestScore) {
      best = candidate;
      bestScore = score;
    }
  }
  if (best == nullptr) {
    return baseline;
  }
  const double bestEvidence =
      best->best_combined_score + best->pulse_section_support * 0.12;
  const double baselineEvidence =
      baseline->best_combined_score + baseline->pulse_section_support * 0.12;
  return bestEvidence >= baselineEvidence * 1.20 ? best : baseline;
}

[[nodiscard]] const MirCandidate* apply_mir_residual_rescue(
    const std::vector<MirCandidate>& candidates,
    const MirCandidate* selected,
    const char* lane,
    double routeMin) noexcept {
  if (selected == nullptr) {
    return nullptr;
  }
  const double selectedEvidence = mir_evidence_score(*selected);
  if (std::strcmp(lane, "high_pulse_context") == 0 &&
      selected->bpm >= 118.0 && selected->bpm <= 122.0 &&
      mir_has_alias(selected->alias_bits, MirAlias::direct)) {
    const MirCandidate* best = nullptr;
    double bestEvidence = 0.0;
    for (const auto& candidate : candidates) {
      const double evidence = mir_evidence_score(candidate);
      if (candidate.bpm >= 126.0 && candidate.bpm <= 130.5 &&
          candidate.support >= 0.60 &&
          candidate.best_combined_score >= 0.18 &&
          candidate.pulse_section_support >= 0.55 &&
          evidence >= selectedEvidence * 1.25 &&
          (best == nullptr || evidence > bestEvidence)) {
        best = &candidate;
        bestEvidence = evidence;
      }
    }
    if (best != nullptr) {
      return best;
    }
  }
  if (std::strcmp(lane, "harmonic_conflict") == 0) {
    const bool selectedHarmonic =
        mir_has_alias(selected->alias_bits, MirAlias::direct) ||
        mir_has_alias(selected->alias_bits, MirAlias::double_time) ||
        mir_has_alias(selected->alias_bits, MirAlias::three_halves) ||
        mir_has_alias(selected->alias_bits, MirAlias::four_thirds);

    const MirCandidate* lower = nullptr;
    double lowerEvidence = 0.0;
    for (const auto& candidate : candidates) {
      const double evidence = mir_evidence_score(candidate);
      if (candidate.bpm >= 84.0 &&
          mir_pulse_ratio_in_range(candidate.bpm, selected->bpm, 0.64,
                                   0.69) &&
          candidate.support >= 0.85 && candidate.winner_support >= 0.25 &&
          candidate.best_combined_score >= 0.08 &&
          evidence >= selectedEvidence * 0.55 &&
          selected->pulse_section_support < 0.60 && selectedHarmonic &&
          (mir_has_alias(candidate.alias_bits, MirAlias::two_thirds) ||
           mir_has_alias(candidate.alias_bits, MirAlias::four_thirds) ||
           mir_has_alias(candidate.alias_bits,
                             MirAlias::three_quarters)) &&
          (lower == nullptr ||
           std::make_tuple(evidence, candidate.support, candidate.winner_support) >
               std::make_tuple(lowerEvidence, lower->support, lower->winner_support))) {
        lower = &candidate;
        lowerEvidence = evidence;
      }
    }
    if (lower != nullptr) {
      return lower;
    }
  }
  return selected;
}

[[nodiscard]] smart_tempo::MirPrimarySelection
compute_mir_primary_selection(
    const MirCandidateBoard& board,
    const smart_tempo::HodgkinsonTatumProbeResult& result,
    const smart_tempo::HodgkinsonMaterialRiskEvidence& materialRisk,
    const MirRoutingContext* routingContext,
    double routeMin,
    double routeMax,
    bool enabled) {
  smart_tempo::MirPrimarySelection shadow;
  shadow.enabled = enabled;
  shadow.route_min_bpm = routeMin;
  shadow.route_max_bpm = routeMax;
  shadow.selector_version = "measured_candidate_profile_v1";
  shadow.route_center_bpm =
      routeMin > 0.0 && routeMax > routeMin ? (routeMin + routeMax) * 0.5 : 0.0;
  shadow.total_segment_count =
      board.segment_count > 0 ? board.segment_count : result.segment_count;
  shadow.odf_peak_count = result.odf_peak_count;
  if (!shadow.enabled) {
    shadow.decision_class = "disabled";
    shadow.reason = "disabled";
    return shadow;
  }
  std::vector<MirCandidate> candidates = collapse_mir_candidates(board);
  shadow.candidate_count = candidates.size();
  if (candidates.empty()) {
    shadow.decision_class = "no_candidate";
    shadow.reason = "empty_mir_primary_candidate_board";
    return shadow;
  }
  MirLaneStats laneStats;
  const char* lane = classify_mir_lane(
      board, routingContext, shadow.route_center_bpm, routeMax, &laneStats);
  shadow.lane = lane;
  shadow.lane_best_support = laneStats.best_support;
  shadow.lane_high_direct_support = laneStats.high_direct_support;
  shadow.lane_low_support = laneStats.low_support;
  shadow.lane_mid_support = laneStats.mid_support;
  shadow.lane_high_support = laneStats.high_support;

  const bool sparseAcapella = std::strcmp(lane, "sparse_or_acapella") == 0 ||
                              (result.odf_peak_count > 0 &&
                               result.odf_peak_count < 1500);
  shadow.sparse_acapella_review_hold = sparseAcapella;
  shadow.material_risk_review_hold = materialRisk.review_hold_candidate;

  const MirCandidate* selected = nullptr;
  if (std::strcmp(lane, "low_pulse_context") == 0) {
    shadow.lane_variant = "low_pulse_targeted_rescue_v4";
    selected = select_mir_low_pulse(candidates);
    const MirCandidate* continuousCenterRescue =
        select_mir_low_pulse_continuous_center_rescue(
            candidates, selected, routeMin, routeMax);
    if (continuousCenterRescue != nullptr) {
      selected = continuousCenterRescue;
      shadow.lane_variant = "low_pulse_continuous_center_rescue_v1";
    }
  } else if (std::strcmp(lane, "harmonic_conflict") == 0) {
    shadow.lane_variant = "harmonic_soft_route_best";
    selected = select_mir_harmonic_soft(
        candidates, routeMin, routeMax, shadow.route_center_bpm);
  } else if (std::strcmp(lane, "general") == 0 ||
             std::strcmp(lane, "high_pulse_context") == 0) {
    shadow.lane_variant = "general_144_soft";
    if (shadow.route_center_bpm >= 150.0) {
      // Python primary deliberately keeps high-center routes alias-separated.
      shadow.lane_variant = "high_center_conservative";
      const MirRawCandidate* rawSelected = select_mir_raw_conservative(
          board, lane, routeMin, routeMax, shadow.route_center_bpm);
      selected = rawSelected != nullptr
                     ? find_mir_cluster(candidates, rawSelected->bpm)
                     : nullptr;
    } else {
      selected = select_mir_general_144_soft(
          candidates, lane, routeMin, routeMax, shadow.route_center_bpm);
    }
  } else {
    shadow.lane_variant = "conservative";
    const MirRawCandidate* rawSelected = select_mir_raw_conservative(
        board, lane, routeMin, routeMax, shadow.route_center_bpm);
    selected = rawSelected != nullptr
                   ? find_mir_cluster(candidates, rawSelected->bpm)
                   : nullptr;
  }
  selected = apply_mir_residual_rescue(candidates, selected, lane, routeMin);
  if (std::strcmp(lane, "harmonic_conflict") == 0 && selected != nullptr &&
      selected->bpm >= 143.0 && selected->bpm <= 145.0 &&
      (mir_has_alias(selected->alias_bits, MirAlias::four_thirds) ||
       mir_has_alias(selected->alias_bits, MirAlias::three_halves))) {
    const MirCandidate* direct = nullptr;
    for (const auto& candidate : candidates) {
      if (candidate.bpm >= 128.0 && candidate.bpm <= 132.0 &&
          mir_has_alias(candidate.alias_bits, MirAlias::direct) &&
          candidate.support >= 0.70 &&
          candidate.support >= selected->support * 0.70 &&
          (direct == nullptr ||
           std::make_tuple(candidate.support, candidate.best_combined_score,
                           candidate.winner_support) >
               std::make_tuple(direct->support, direct->best_combined_score,
                               direct->winner_support))) {
        direct = &candidate;
      }
    }
    if (direct != nullptr) {
      selected = direct;
      shadow.lane_variant = "harmonic_direct_protection";
    }
  }
  if (selected == nullptr) {
    shadow.decision_class = "no_candidate";
    shadow.reason = "selector_returned_no_candidate";
    return shadow;
  }
  shadow.selected_bpm = selected->bpm;
  shadow.support = selected->support;
  shadow.winner_support = selected->winner_support;
  shadow.score_sum = selected->score_sum;
  shadow.best_combined_score = selected->best_combined_score;
  shadow.pulse_score = selected->pulse_score;
  shadow.pulse_section_support = selected->pulse_section_support;
  shadow.continuous_score = selected->continuous_score;
  shadow.continuous_section_support = selected->continuous_section_support;
  shadow.continuous_score_ratio = selected->continuous_score_ratio;
  shadow.alias_classes = mir_alias_text(selected->alias_bits);
  shadow.material_risk_review_hold =
      shadow.material_risk_review_hold ||
      is_mir_high_144_material_risk_review_hold(
          *selected, lane, routeMin, routeMax, shadow.route_center_bpm);
  shadow.high_family_conflict_review_hold =
      is_mir_high_family_conflict_review_hold(
          *selected, result, routeMin, routeMax);
  shadow.soft_center_conflict_review_hold =
      is_mir_soft_center_conflict_review_hold(
          *selected, routingContext, routeMin, routeMax,
          shadow.route_center_bpm);
  shadow.review_hold = shadow.sparse_acapella_review_hold ||
                       shadow.material_risk_review_hold ||
                       shadow.high_family_conflict_review_hold ||
                       shadow.soft_center_conflict_review_hold;
  shadow.route_valid =
      shadow.selected_bpm >= kMirProductionMinBpm &&
      shadow.selected_bpm <= kMirProductionMaxBpm;
  shadow.candidate = shadow.selected_bpm > 0.0 && std::isfinite(shadow.selected_bpm);
  shadow.auto_candidate = shadow.candidate && !shadow.review_hold;
  if (shadow.material_risk_review_hold) {
    shadow.decision_class = "review_hold";
    shadow.reason = "material_risk_review_hold";
  } else if (shadow.high_family_conflict_review_hold) {
    shadow.decision_class = "review_hold";
    shadow.reason = "high_family_conflict_review_hold";
  } else if (shadow.soft_center_conflict_review_hold) {
    shadow.decision_class = "review_hold";
    shadow.reason = "soft_center_conflict_review_hold";
  } else if (shadow.sparse_acapella_review_hold) {
    shadow.decision_class = "review_hold";
    shadow.reason = "sparse_or_acapella_review_hold";
  } else if (shadow.auto_candidate) {
    shadow.decision_class = "auto_candidate";
    shadow.reason = "measured_candidate_primary";
  } else {
    shadow.decision_class = "no_auto_candidate";
    shadow.reason = "invalid_primary_candidate";
  }
  return shadow;
}

[[nodiscard]] smart_tempo::MirPrimarySelection
compute_mir_dsp_selection(
    const MirCandidateBoard& board,
    const smart_tempo::HodgkinsonTatumProbeResult& result,
    const smart_tempo::HodgkinsonMaterialRiskEvidence& materialRisk,
    bool enabled) {
  smart_tempo::MirPrimarySelection shadow;
  shadow.enabled = enabled;
  shadow.selector_version = "route_independent_evidence_v1";
  shadow.route_min_bpm = 40.0;
  shadow.route_max_bpm = 220.0;
  shadow.route_center_bpm = 0.0;
  shadow.total_segment_count =
      board.segment_count > 0 ? board.segment_count : result.segment_count;
  shadow.odf_peak_count = result.odf_peak_count;
  if (!shadow.enabled) {
    shadow.decision_class = "disabled";
    shadow.reason = "disabled";
    return shadow;
  }

  const std::vector<MirCandidate> candidates =
      collapse_mir_candidates(board);
  shadow.candidate_count = candidates.size();
  if (candidates.empty()) {
    shadow.decision_class = "no_candidate";
    shadow.reason = "empty_route_independent_candidate_board";
    return shadow;
  }

  MirLaneStats laneStats;
  shadow.lane = classify_mir_lane(
      board, nullptr, 130.0, 220.0, &laneStats);
  shadow.lane_variant = "measured_evidence_composite_v1";
  shadow.lane_best_support = laneStats.best_support;
  shadow.lane_high_direct_support = laneStats.high_direct_support;
  shadow.lane_low_support = laneStats.low_support;
  shadow.lane_mid_support = laneStats.mid_support;
  shadow.lane_high_support = laneStats.high_support;

  double maxScoreSum = 0.0;
  for (const auto& candidate : candidates) {
    maxScoreSum = (std::max)(maxScoreSum, candidate.score_sum);
  }
  const auto measuredEvidence = [maxScoreSum](
                                    const MirCandidate& candidate) noexcept {
    const double normalizedScoreSum =
        maxScoreSum > 0.0
            ? std::clamp(candidate.score_sum / maxScoreSum, 0.0, 1.0)
            : 0.0;
    return 0.34 * candidate.support +
           0.24 * candidate.winner_support +
           0.14 * candidate.best_combined_score +
           0.13 * candidate.pulse_score +
           0.10 * candidate.pulse_section_support +
           0.03 * candidate.continuous_section_support +
           0.02 * normalizedScoreSum;
  };

  const MirCandidate* selected = nullptr;
  double selectedEvidence = -std::numeric_limits<double>::infinity();
  for (const auto& candidate : candidates) {
    if (!(candidate.bpm >= 40.0 && candidate.bpm <= 220.0) ||
        !std::isfinite(candidate.bpm)) {
      continue;
    }
    const double evidence = measuredEvidence(candidate);
    const auto tie = std::make_tuple(
        candidate.support, candidate.winner_support,
        candidate.pulse_section_support, candidate.best_combined_score,
        candidate.score_sum, -candidate.bpm);
    const auto selectedTie =
        selected == nullptr
            ? std::make_tuple(
                  -1.0, -1.0, -1.0, -1.0, -1.0,
                  -std::numeric_limits<double>::infinity())
            : std::make_tuple(
                  selected->support, selected->winner_support,
                  selected->pulse_section_support,
                  selected->best_combined_score, selected->score_sum,
                  -selected->bpm);
    if (selected == nullptr || evidence > selectedEvidence + 1e-12 ||
        (std::abs(evidence - selectedEvidence) <= 1e-12 &&
         tie > selectedTie)) {
      selected = &candidate;
      selectedEvidence = evidence;
    }
  }
  if (selected == nullptr) {
    shadow.decision_class = "no_candidate";
    shadow.reason = "no_candidate_inside_global_dsp_rails";
    return shadow;
  }

  shadow.selected_bpm = selected->bpm;
  shadow.support = selected->support;
  shadow.winner_support = selected->winner_support;
  shadow.score_sum = selected->score_sum;
  shadow.best_combined_score = selected->best_combined_score;
  shadow.pulse_score = selected->pulse_score;
  shadow.pulse_section_support = selected->pulse_section_support;
  shadow.continuous_score = selected->continuous_score;
  shadow.continuous_section_support = selected->continuous_section_support;
  shadow.continuous_score_ratio = selected->continuous_score_ratio;
  shadow.alias_classes = mir_alias_text(selected->alias_bits);
  shadow.sparse_acapella_review_hold =
      result.odf_peak_count > 0 && result.odf_peak_count < 1500;
  shadow.material_risk_review_hold = materialRisk.review_hold_candidate;
  shadow.review_hold =
      shadow.sparse_acapella_review_hold ||
      shadow.material_risk_review_hold;
  shadow.route_valid = true;
  shadow.candidate =
      shadow.selected_bpm > 0.0 && std::isfinite(shadow.selected_bpm);
  shadow.auto_candidate = shadow.candidate && !shadow.review_hold;
  if (shadow.material_risk_review_hold) {
    shadow.decision_class = "review_hold";
    shadow.reason = "material_risk_review_hold";
  } else if (shadow.sparse_acapella_review_hold) {
    shadow.decision_class = "review_hold";
    shadow.reason = "sparse_material_review_hold";
  } else {
    shadow.decision_class = "auto_candidate";
    shadow.reason = "route_independent_measured_evidence";
  }
  return shadow;
}

[[nodiscard]] double compute_mir_policy_measured_evidence(
    const MirCandidate& candidate,
    double maxScoreSum) noexcept {
  const double normalizedScoreSum =
      maxScoreSum > 0.0
          ? std::clamp(candidate.score_sum / maxScoreSum, 0.0, 1.0)
          : 0.0;
  return 0.34 * candidate.support +
         0.24 * candidate.winner_support +
         0.14 * candidate.best_combined_score +
         0.13 * candidate.pulse_score +
         0.10 * candidate.pulse_section_support +
         0.03 * candidate.continuous_section_support +
         0.02 * normalizedScoreSum;
}

[[nodiscard]] bool mir_policy_high_pulse_immune(
    const MirCandidate& candidate) noexcept {
  return candidate.bpm >= 155.0 && candidate.support >= 0.70 &&
         candidate.winner_support >= 0.55 &&
         (mir_has_alias(candidate.alias_bits, MirAlias::direct) ||
          mir_has_alias(candidate.alias_bits, MirAlias::double_time) ||
          mir_has_alias(candidate.alias_bits, MirAlias::four_thirds));
}

void populate_mir_policy_shadow_from_candidate(
    smart_tempo::MirPrimarySelection& shadow,
    const MirCandidate& selected) {
  shadow.selected_bpm = selected.bpm;
  shadow.support = selected.support;
  shadow.winner_support = selected.winner_support;
  shadow.score_sum = selected.score_sum;
  shadow.best_combined_score = selected.best_combined_score;
  shadow.pulse_score = selected.pulse_score;
  shadow.pulse_section_support = selected.pulse_section_support;
  shadow.continuous_score = selected.continuous_score;
  shadow.continuous_section_support = selected.continuous_section_support;
  shadow.continuous_score_ratio = selected.continuous_score_ratio;
  shadow.alias_classes = mir_alias_text(selected.alias_bits);
}

[[nodiscard]] smart_tempo::MirPrimarySelection
compute_mir_soft_center_selection(
    const MirCandidateBoard& board,
    const smart_tempo::HodgkinsonTatumProbeResult& result,
    const smart_tempo::HodgkinsonMaterialRiskEvidence& materialRisk,
    double priorMinBpm,
    double priorMaxBpm,
    bool routeMatched,
    bool genericRoute,
    bool enabled) {
  smart_tempo::MirPrimarySelection shadow;
  shadow.enabled = enabled;
  shadow.selector_version = "soft_center_evidence_v1";
  shadow.route_min_bpm = kMirProductionMinBpm;
  shadow.route_max_bpm = kMirProductionMaxBpm;
  shadow.route_center_bpm =
      priorMinBpm > 0.0 && priorMaxBpm > priorMinBpm
          ? (priorMinBpm + priorMaxBpm) * 0.5
          : 0.0;
  shadow.total_segment_count =
      board.segment_count > 0 ? board.segment_count : result.segment_count;
  shadow.odf_peak_count = result.odf_peak_count;
  if (!shadow.enabled) {
    shadow.decision_class = "disabled";
    shadow.reason = "disabled";
    return shadow;
  }

  const std::vector<MirCandidate> candidates =
      collapse_mir_candidates(board);
  shadow.candidate_count = candidates.size();
  if (candidates.empty()) {
    shadow.decision_class = "no_candidate";
    shadow.reason = "empty_route_independent_candidate_board";
    return shadow;
  }

  MirLaneStats laneStats;
  shadow.lane = classify_mir_lane(
      board, nullptr, 130.0, 220.0, &laneStats);
  shadow.lane_variant = "soft_center_measured_primary_v0";
  shadow.lane_best_support = laneStats.best_support;
  shadow.lane_high_direct_support = laneStats.high_direct_support;
  shadow.lane_low_support = laneStats.low_support;
  shadow.lane_mid_support = laneStats.mid_support;
  shadow.lane_high_support = laneStats.high_support;

  double maxScoreSum = 0.0;
  for (const auto& candidate : candidates) {
    maxScoreSum = (std::max)(maxScoreSum, candidate.score_sum);
  }

  struct ScoredCandidate {
    const MirCandidate* candidate = nullptr;
    double measured_score = 0.0;
    double soft_bonus = 0.0;
    double soft_score = 0.0;
  };

  std::vector<ScoredCandidate> scored;
  scored.reserve(candidates.size());
  for (const auto& candidate : candidates) {
    if (!(candidate.bpm >= kMirProductionMinBpm &&
          candidate.bpm <= kMirProductionMaxBpm) ||
        !std::isfinite(candidate.bpm)) {
      continue;
    }
    const double measuredScore =
        compute_mir_policy_measured_evidence(candidate, maxScoreSum);
    scored.push_back({&candidate, measuredScore, 0.0, measuredScore});
  }
  if (scored.empty()) {
    shadow.decision_class = "no_candidate";
    shadow.reason = "no_candidate_inside_global_dsp_rails";
    return shadow;
  }

  const auto measuredTie = [](const ScoredCandidate& item) {
    return std::make_tuple(
        item.measured_score, item.candidate->support,
        item.candidate->winner_support,
        item.candidate->pulse_section_support,
        item.candidate->best_combined_score, item.candidate->score_sum,
        -item.candidate->bpm);
  };
  std::sort(scored.begin(), scored.end(),
            [&](const ScoredCandidate& lhs, const ScoredCandidate& rhs) {
              return measuredTie(lhs) > measuredTie(rhs);
            });
  const ScoredCandidate measuredWinner = scored.front();
  const double measuredRunnerUpScore =
      scored.size() >= 2 ? scored[1].measured_score : 0.0;
  const double measuredDominanceMargin =
      measuredWinner.measured_score - measuredRunnerUpScore;

  const double priorWidth = priorMaxBpm - priorMinBpm;
  const bool priorUsable = routeMatched && !genericRoute &&
                           shadow.route_center_bpm > 0.0 &&
                           priorWidth > 0.0 && priorWidth < 100.0;
  constexpr double kClearMeasuredWinnerMargin = 0.25;
  const bool clearMeasuredWinner =
      measuredDominanceMargin > kClearMeasuredWinnerMargin;

  ScoredCandidate selected = measuredWinner;
  if (priorUsable && !clearMeasuredWinner) {
    constexpr double kMaxSoftCenterBonus = 0.08;
    const double spread = (std::max)(12.0, priorWidth * 0.5);
    for (auto& item : scored) {
      if (item.candidate->support < 0.15 || item.measured_score < 0.20) {
        continue;
      }
      const double distance =
          std::abs(item.candidate->bpm - shadow.route_center_bpm);
      const double scaled = distance / spread;
      item.soft_bonus =
          kMaxSoftCenterBonus * std::exp(-0.5 * scaled * scaled);
      item.soft_score = item.measured_score + item.soft_bonus;
    }
    const auto softTie = [](const ScoredCandidate& item) {
      return std::make_tuple(
          item.soft_score, item.measured_score, item.candidate->support,
          item.candidate->winner_support,
          item.candidate->pulse_section_support,
          item.candidate->best_combined_score, -item.candidate->bpm);
    };
    std::sort(scored.begin(), scored.end(),
              [&](const ScoredCandidate& lhs, const ScoredCandidate& rhs) {
                return softTie(lhs) > softTie(rhs);
              });
    selected = scored.front();
    shadow.soft_center_prior_applied =
        std::abs(selected.candidate->bpm - measuredWinner.candidate->bpm) >=
        0.10;
    shadow.soft_center_high_pulse_veto =
        shadow.soft_center_prior_applied &&
        mir_policy_high_pulse_immune(*measuredWinner.candidate) &&
        selected.candidate->bpm < measuredWinner.candidate->bpm;
    if (shadow.soft_center_high_pulse_veto) {
      selected = measuredWinner;
      shadow.soft_center_prior_applied = false;
    }
  }

  populate_mir_policy_shadow_from_candidate(shadow, *selected.candidate);
  shadow.measured_evidence_score = selected.measured_score;
  shadow.soft_center_bonus = selected.soft_bonus;
  shadow.soft_center_score = selected.soft_score;
  shadow.soft_center_runner_up_score =
      scored.size() >= 2 ? scored[1].soft_score : 0.0;
  shadow.soft_center_dominance_margin =
      selected.soft_score - shadow.soft_center_runner_up_score;
  shadow.sparse_acapella_review_hold =
      result.odf_peak_count > 0 && result.odf_peak_count < 1500;
  shadow.material_risk_review_hold = materialRisk.review_hold_candidate;
  shadow.review_hold =
      shadow.sparse_acapella_review_hold ||
      shadow.material_risk_review_hold;
  shadow.route_valid = true;
  shadow.candidate =
      shadow.selected_bpm > 0.0 && std::isfinite(shadow.selected_bpm);
  shadow.auto_candidate = shadow.candidate && !shadow.review_hold;
  if (shadow.material_risk_review_hold) {
    shadow.decision_class = "review_hold";
    shadow.reason = "material_risk_review_hold";
  } else if (shadow.sparse_acapella_review_hold) {
    shadow.decision_class = "review_hold";
    shadow.reason = "sparse_material_review_hold";
  } else if (shadow.soft_center_high_pulse_veto) {
    shadow.decision_class = "auto_candidate";
    shadow.reason = "soft_center_high_pulse_veto_kept_measured_winner";
  } else if (!priorUsable) {
    shadow.decision_class = "auto_candidate";
    shadow.reason = "soft_center_prior_unavailable";
  } else if (clearMeasuredWinner) {
    shadow.decision_class = "auto_candidate";
    shadow.reason = "clear_measured_winner";
  } else if (shadow.soft_center_prior_applied) {
    shadow.decision_class = "auto_candidate";
    shadow.reason = "soft_center_prior_selected_measured_candidate";
  } else {
    shadow.decision_class = "auto_candidate";
    shadow.reason = "soft_center_kept_measured_winner";
  }
  return shadow;
}

[[nodiscard]] double mir_policy_fullboard_alias_penalty(
    uint16_t aliasBits) noexcept {
  if (mir_has_alias(aliasBits, MirAlias::direct)) {
    return -0.05;
  }
  if (mir_has_alias(aliasBits, MirAlias::one_third) ||
      mir_has_alias(aliasBits, MirAlias::quarter) ||
      mir_has_alias(aliasBits, MirAlias::triple)) {
    return 0.18;
  }
  if (mir_has_alias(aliasBits, MirAlias::half) ||
      mir_has_alias(aliasBits, MirAlias::double_time)) {
    return 0.10;
  }
  return 0.04;
}

[[nodiscard]] double mir_policy_normalized(double value,
                                        double maximum) noexcept {
  if (!(maximum > 0.0)) {
    return 0.0;
  }
  return std::clamp(value / maximum, 0.0, 1.0);
}

[[nodiscard]] double mir_policy_safe_ratio(double value,
                                        double reference) noexcept {
  if (reference <= 0.0) {
    return value > 0.0 ? std::numeric_limits<double>::infinity() : 1.0;
  }
  return value / reference;
}

[[nodiscard]] std::vector<MirPolicyMeasuredCandidate>
prepare_mir_policy_fullboard_candidates(
    const MirCandidateBoard& board,
    MirLocalExactCache& localExactCache) {
  const std::vector<MirCandidate> collapsed =
      collapse_mir_candidates(board);
  struct PendingCandidate {
    MirCandidate candidate;
    MirLocalExactBpm exact;
    double continuous = 0.0;
    double log_score_sum = 0.0;
  };
  std::vector<PendingCandidate> pending;
  pending.reserve(collapsed.size());
  double maxLocal = 0.0;
  double maxContinuous = 0.0;
  double maxLogScoreSum = 0.0;
  for (const auto& candidate : collapsed) {
    const MirLocalExactBpm exact = localExactCache.measure(candidate.bpm);
    const double continuous =
        (std::max)(candidate.continuous_score,
                   candidate.continuous_section_support);
    const double logScoreSum = std::log1p((std::max)(0.0, candidate.score_sum));
    maxLocal = (std::max)(maxLocal, exact.score);
    maxContinuous = (std::max)(maxContinuous, continuous);
    maxLogScoreSum = (std::max)(maxLogScoreSum, logScoreSum);
    pending.push_back(
        PendingCandidate{candidate, exact, continuous, logScoreSum});
  }

  std::vector<MirPolicyMeasuredCandidate> measured;
  measured.reserve(pending.size());
  for (const auto& item : pending) {
    const double bpm =
        item.exact.bpm > 0.0 ? item.exact.bpm : item.candidate.bpm;
    if (!(bpm >= kMirProductionMinBpm && bpm <= kMirProductionMaxBpm) ||
        !std::isfinite(bpm)) {
      continue;
    }
    const double baseScore =
        0.5 * mir_policy_normalized(item.exact.score, maxLocal) +
        1.0 * mir_policy_normalized(item.continuous, maxContinuous) +
        0.5 * mir_policy_normalized(item.log_score_sum, maxLogScoreSum) -
        mir_policy_fullboard_alias_penalty(item.candidate.alias_bits);
    measured.push_back(MirPolicyMeasuredCandidate{
        item.candidate,
        item.candidate.bpm,
        bpm,
        item.exact.score,
        baseScore,
        mir_alias_text(item.candidate.alias_bits)});
  }
  std::sort(
      measured.begin(), measured.end(),
      [](const MirPolicyMeasuredCandidate& lhs,
         const MirPolicyMeasuredCandidate& rhs) {
        return std::make_tuple(lhs.base_score, lhs.candidate.support,
                               lhs.candidate.winner_support,
                               lhs.local_exact_score, -lhs.bpm) >
               std::make_tuple(rhs.base_score, rhs.candidate.support,
                               rhs.candidate.winner_support,
                               rhs.local_exact_score, -rhs.bpm);
      });
  return measured;
}

[[nodiscard]] bool mir_policy_is_harmonic_ratio(double candidateBpm,
                                              double baseBpm) noexcept {
  if (!(candidateBpm > 0.0 && baseBpm > 0.0)) {
    return false;
  }
  constexpr std::array<MirPulseRelation, 10> kRelations{
      MirPulseRelation::quarter,        MirPulseRelation::one_third,
      MirPulseRelation::half,           MirPulseRelation::two_thirds,
      MirPulseRelation::three_quarters, MirPulseRelation::four_thirds,
      MirPulseRelation::three_halves,   MirPulseRelation::double_time,
      MirPulseRelation::triple,         MirPulseRelation::quadruple};
  constexpr double kTolerance = 0.02;
  const double ratio = candidateBpm / baseBpm;
  return std::any_of(kRelations.begin(), kRelations.end(),
                     [ratio](MirPulseRelation relation) {
                       return std::abs(
                                  ratio - mir_pulse_relation_multiplier(
                                              relation)) <= kTolerance;
                     });
}

[[nodiscard]] bool mir_policy_fullboard_high_pulse_immune(
    const MirPolicyMeasuredCandidate& candidate) noexcept {
  return candidate.bpm >= 155.0 && candidate.candidate.support >= 0.50 &&
         (candidate.candidate.winner_support >= 0.20 ||
          mir_has_alias(candidate.candidate.alias_bits,
                            MirAlias::direct) ||
          mir_has_alias(candidate.candidate.alias_bits,
                            MirAlias::double_time));
}

[[nodiscard]] const MirPolicyMeasuredCandidate*
select_mir_policy_review_promotion_candidate(
    std::span<const MirPolicyMeasuredCandidate> candidates,
    double priorCenter,
    bool priorUsable,
    double& centerImprovement,
    double& scoreRatio) noexcept {
  centerImprovement = 0.0;
  scoreRatio = 0.0;
  if (candidates.empty()) {
    return nullptr;
  }
  const MirPolicyMeasuredCandidate& measured = candidates.front();
  if (!priorUsable) {
    return &measured;
  }
  const double measuredDistance = std::abs(measured.bpm - priorCenter);
  if (measuredDistance < 32.0) {
    return &measured;
  }

  const MirPolicyMeasuredCandidate* selected = nullptr;
  for (std::size_t index = 1; index < candidates.size(); ++index) {
    const auto& candidate = candidates[index];
    if (!mir_policy_is_harmonic_ratio(candidate.bpm, measured.bpm) ||
        candidate.candidate.support < 0.20) {
      continue;
    }
    const double candidateScoreRatio =
        measured.base_score > 0.0
            ? candidate.base_score / measured.base_score
            : 0.0;
    const double improvement =
        measuredDistance - std::abs(candidate.bpm - priorCenter);
    if (candidateScoreRatio < 0.55 || improvement < 20.0 ||
        (candidate.bpm < measured.bpm - 1.0 &&
         mir_policy_fullboard_high_pulse_immune(measured))) {
      continue;
    }
    if (selected == nullptr ||
        std::make_tuple(improvement, candidateScoreRatio,
                        candidate.candidate.support, candidate.base_score,
                        -candidate.bpm) >
            std::make_tuple(centerImprovement, scoreRatio,
                            selected->candidate.support,
                            selected->base_score, -selected->bpm)) {
      selected = &candidate;
      centerImprovement = improvement;
      scoreRatio = candidateScoreRatio;
    }
  }
  return selected != nullptr ? selected : &measured;
}

[[nodiscard]] const MirPolicyMeasuredCandidate*
find_mir_policy_output_candidate(
    std::span<const MirPolicyMeasuredCandidate> candidates,
    double outputBpm) noexcept {
  if (candidates.empty()) {
    return nullptr;
  }
  return &*std::min_element(
      candidates.begin(), candidates.end(),
      [outputBpm](const MirPolicyMeasuredCandidate& lhs,
                  const MirPolicyMeasuredCandidate& rhs) {
        return std::abs(lhs.bpm - outputBpm) <
               std::abs(rhs.bpm - outputBpm);
      });
}

[[nodiscard]] double mir_policy_historical_recovery_evidence(
    const MirPolicyMeasuredCandidate& candidate) noexcept {
  return candidate.candidate.support * 4.0 +
         candidate.candidate.winner_support +
         candidate.candidate.best_combined_score * 10.0 +
         candidate.candidate.pulse_score * 4.0 +
         candidate.candidate.pulse_section_support * 4.0 +
         std::log1p((std::max)(0.0, candidate.candidate.score_sum)) * 1.5 +
         (mir_has_alias(candidate.candidate.alias_bits,
                            MirAlias::direct)
              ? 0.7
              : 0.0);
}

// Allocation-free view over measured candidates. It centralizes pulse-family
// lookups without synthesizing, averaging, or otherwise changing a BPM value.
class MirMeasuredPulseFamilyMatrix {
 public:
  explicit MirMeasuredPulseFamilyMatrix(
      std::span<const MirPolicyMeasuredCandidate> candidates) noexcept
      : candidates_(candidates) {}

  [[nodiscard]] const MirPolicyMeasuredCandidate* nearest(
      double anchorBpm, MirPulseRelation relation) const noexcept {
    const double multiplier = mir_pulse_relation_multiplier(relation);
    if (!(anchorBpm > 0.0) || !(multiplier > 0.0) || candidates_.empty()) {
      return nullptr;
    }
    const double targetBpm = anchorBpm * multiplier;
    const auto candidate = std::min_element(
        candidates_.begin(), candidates_.end(),
        [targetBpm](const MirPolicyMeasuredCandidate& lhs,
                    const MirPolicyMeasuredCandidate& rhs) {
          return std::abs(lhs.bpm - targetBpm) <
                 std::abs(rhs.bpm - targetBpm);
        });
    return candidate == candidates_.end() ? nullptr : &*candidate;
  }

  [[nodiscard]] static double absolute_relation_error(
      double candidateBpm, double anchorBpm,
      MirPulseRelation relation) noexcept {
    if (!(candidateBpm > 0.0) || !(anchorBpm > 0.0)) {
      return std::numeric_limits<double>::infinity();
    }
    return std::abs(candidateBpm / anchorBpm -
                    mir_pulse_relation_multiplier(relation));
  }

  [[nodiscard]] static double relative_relation_error(
      double candidateBpm, double anchorBpm,
      MirPulseRelation relation) noexcept {
    const double multiplier = mir_pulse_relation_multiplier(relation);
    if (!(candidateBpm > 0.0) || !(anchorBpm > 0.0) ||
        !(multiplier > 0.0)) {
      return std::numeric_limits<double>::infinity();
    }
    return std::abs((candidateBpm / anchorBpm) / multiplier - 1.0);
  }

  [[nodiscard]] static bool matches(
      double candidateBpm, double anchorBpm, MirPulseRelation relation,
      double absoluteRatioTolerance) noexcept {
    return absolute_relation_error(candidateBpm, anchorBpm, relation) <=
           absoluteRatioTolerance;
  }

  template <std::size_t Size>
  [[nodiscard]] static double minimum_relative_error(
      double candidateBpm, double anchorBpm,
      const std::array<MirPulseRelation, Size>& relations) noexcept {
    double error = std::numeric_limits<double>::infinity();
    for (const auto relation : relations) {
      error = (std::min)(
          error, relative_relation_error(candidateBpm, anchorBpm, relation));
    }
    return error;
  }

  template <typename Predicate>
  [[nodiscard]] bool any_relation(double anchorBpm,
                                  MirPulseRelation relation,
                                  double absoluteRatioTolerance,
                                  Predicate&& predicate) const noexcept {
    return std::any_of(
        candidates_.begin(), candidates_.end(),
        [anchorBpm, relation, absoluteRatioTolerance,
         &predicate](const MirPolicyMeasuredCandidate& candidate) {
          return matches(candidate.bpm, anchorBpm, relation,
                         absoluteRatioTolerance) &&
                 predicate(candidate);
        });
  }

  template <typename Predicate>
  [[nodiscard]] bool any_equivalent(double bpm, double toleranceBpm,
                                    Predicate&& predicate) const noexcept {
    if (!(bpm > 0.0) || !(toleranceBpm >= 0.0)) {
      return false;
    }
    return std::any_of(
        candidates_.begin(), candidates_.end(),
        [bpm, toleranceBpm,
         &predicate](const MirPolicyMeasuredCandidate& candidate) {
          return std::abs(candidate.bpm - bpm) <= toleranceBpm &&
                 predicate(candidate);
        });
  }

 private:
  std::span<const MirPolicyMeasuredCandidate> candidates_;
};

[[nodiscard]] const MirPolicyMeasuredCandidate*
select_mir_policy_measured_half_pulse_recovery(
    std::span<const MirPolicyMeasuredCandidate> candidates,
    const MirPolicyMeasuredCandidate& current,
    double& evidenceRatio) noexcept {
  evidenceRatio = 0.0;
  const uint16_t currentAliases = current.candidate.alias_bits;
  const bool currentFamilyEligible =
      mir_has_alias(currentAliases, MirAlias::direct) ||
      mir_has_alias(currentAliases, MirAlias::double_time) ||
      mir_has_alias(currentAliases, MirAlias::three_halves) ||
      mir_has_alias(currentAliases, MirAlias::four_thirds) ||
      mir_has_alias(currentAliases, MirAlias::triple);
  if (current.bpm < 168.0 || current.candidate.support < 0.90 ||
      current.candidate.winner_support < 0.25 ||
      current.candidate.pulse_section_support < 0.60 ||
      !currentFamilyEligible) {
    return nullptr;
  }

  const double currentEvidence =
      (std::max)(mir_policy_historical_recovery_evidence(current), 0.000001);
  const MirPolicyMeasuredCandidate* selected = nullptr;
  for (const auto& candidate : candidates) {
    const uint16_t candidateAliases = candidate.candidate.alias_bits;
    const bool aliasEligible =
        mir_has_alias(candidateAliases, MirAlias::direct) ||
        mir_has_alias(candidateAliases, MirAlias::two_thirds) ||
        mir_has_alias(candidateAliases, MirAlias::three_quarters) ||
        mir_has_alias(candidateAliases, MirAlias::four_thirds);
    const double candidateEvidence =
        mir_policy_historical_recovery_evidence(candidate);
    const double candidateEvidenceRatio = candidateEvidence / currentEvidence;
    if (!MirMeasuredPulseFamilyMatrix::matches(
            candidate.bpm, current.bpm, MirPulseRelation::half, 0.01) ||
        candidate.bpm < 80.0 ||
        candidate.candidate.support < 0.95 ||
        candidate.candidate.winner_support < 0.35 ||
        candidate.candidate.continuous_score < 0.22 ||
        candidate.candidate.continuous_section_support < 0.65 ||
        candidateEvidenceRatio < 0.60 || !aliasEligible) {
      continue;
    }
    if (selected == nullptr ||
        std::make_tuple(candidateEvidence,
                        candidate.candidate.continuous_score,
                        candidate.candidate.support,
                        candidate.candidate.winner_support) >
            std::make_tuple(
                mir_policy_historical_recovery_evidence(*selected),
                selected->candidate.continuous_score,
                selected->candidate.support,
                selected->candidate.winner_support)) {
      selected = &candidate;
      evidenceRatio = candidateEvidenceRatio;
    }
  }
  return selected;
}

[[nodiscard]] const MirPolicyMeasuredCandidate*
select_mir_policy_residual_writer_recovery(
    std::span<const MirPolicyMeasuredCandidate> candidates,
    const MirPolicyMeasuredCandidate& current,
    const smart_tempo::HodgkinsonPartialBarAggregateShadow*
        partialBarAggregate,
    double measuredMargin, bool broadRoute,
    const char*& reason) noexcept {
  reason = nullptr;
  if (candidates.empty() || current.bpm <= 0.0) {
    return nullptr;
  }

  const MirMeasuredPulseFamilyMatrix pulseFamilies(candidates);
  const auto& measured = candidates.front();
  const double downwardDelta = current.bpm - measured.bpm;
  const double measuredLocalRatio = mir_policy_safe_ratio(
      measured.local_exact_score, current.local_exact_score);
  const double measuredSupportRatio = mir_policy_safe_ratio(
      measured.candidate.support, current.candidate.support);

  const bool broadDownwardRecovery =
      downwardDelta >= 35.0 && downwardDelta <= 60.0 &&
      measured.candidate.support >= 0.60 &&
      measured.local_exact_score >= 0.20 && measuredMargin >= 0.02 &&
      measuredLocalRatio >= 1.30 && measuredSupportRatio >= 1.20;
  const bool strictDirectDownwardRecovery =
      downwardDelta >= 35.0 && downwardDelta <= 60.0 &&
      measured.candidate.support >= 0.90 &&
      measured.local_exact_score >= 0.35 && measuredMargin >= 0.05 &&
      measuredLocalRatio >= 1.50 && measuredSupportRatio >= 1.00 &&
      mir_has_alias(measured.candidate.alias_bits, MirAlias::direct);
  if (broadDownwardRecovery || strictDirectDownwardRecovery) {
    reason = broadDownwardRecovery
                 ? "measured_residual_downward_family_recovery_gate"
                 : "measured_strict_direct_downward_family_recovery_gate";
    return &measured;
  }

  if (current.bpm >= 100.0 && measured.bpm - current.bpm >= 35.0 &&
      measured.bpm - current.bpm <= 100.0 &&
      measured.candidate.support >= 0.80 &&
      measured.local_exact_score >= 0.45) {
    const double targetBpm = measured.bpm * 0.5;
    const MirPolicyMeasuredCandidate* winnerHalf =
        pulseFamilies.nearest(measured.bpm, MirPulseRelation::half);
    if (winnerHalf != nullptr &&
        std::abs(winnerHalf->bpm - targetBpm) <=
            (std::max)(0.75, measured.bpm * 0.01) &&
        winnerHalf->candidate.support >= 0.40 &&
        winnerHalf->local_exact_score >= 0.18 &&
        mir_policy_safe_ratio(winnerHalf->local_exact_score,
                              measured.local_exact_score) >= 0.60 &&
        mir_policy_safe_ratio(winnerHalf->local_exact_score,
                              current.local_exact_score) >= 0.80 &&
        mir_policy_safe_ratio(winnerHalf->candidate.support,
                              current.candidate.support) >= 0.80) {
      reason = "measured_winner_half_family_recovery_gate";
      return winnerHalf;
    }
  }

  if (current.bpm >= 168.0) {
    const double targetBpm = current.bpm * 0.5;
    const MirPolicyMeasuredCandidate* exactHalf =
        pulseFamilies.nearest(current.bpm, MirPulseRelation::half);
    if (exactHalf != nullptr &&
        std::abs(exactHalf->bpm - targetBpm) <=
            (std::max)(0.75, current.bpm * 0.01) &&
        mir_has_alias(exactHalf->candidate.alias_bits, MirAlias::half) &&
        exactHalf->candidate.support >= 0.90 &&
        exactHalf->local_exact_score >= 0.22 &&
        mir_policy_safe_ratio(exactHalf->local_exact_score,
                              current.local_exact_score) >= 0.60 &&
        mir_policy_safe_ratio(exactHalf->candidate.support,
                              current.candidate.support) >= 0.50) {
      const bool retainIndependentHighPulse =
          current.bpm >= kMirPolicyHighPulseRetentionMinBpm &&
          current.bpm <= kMirPolicyHighPulseRetentionMaxBpm &&
          pulseFamilies.any_equivalent(
              current.bpm, 0.10,
              [&exactHalf](const auto& equivalent) {
                return mir_has_alias(equivalent.candidate.alias_bits,
                                     MirAlias::direct) &&
                       equivalent.candidate.support >=
                           kMirPolicyHighPulseRetentionMinSupport &&
                       equivalent.candidate.winner_support >=
                           kMirPolicyHighPulseRetentionMinWinnerSupport &&
                       equivalent.local_exact_score >=
                           kMirPolicyHighPulseRetentionMinLocalExactScore &&
                       equivalent.candidate.pulse_score >=
                           kMirPolicyHighPulseRetentionMinPulseScore &&
                       equivalent.candidate.pulse_section_support >=
                           kMirPolicyHighPulseRetentionMinPulseSectionSupport &&
                       mir_policy_safe_ratio(
                           exactHalf->local_exact_score,
                           equivalent.local_exact_score) <=
                           kMirPolicyHighPulseRetentionMaxHalfLocalScoreRatio;
              });
      if (retainIndependentHighPulse) {
        reason = "measured_independent_high_pulse_retention_gate";
        return nullptr;
      }
      reason = "measured_exact_half_family_recovery_gate";
      return exactHalf;
    }
  }

  // Generic and unmatched routes have no musical prior. Once every existing
  // release lane has declined the conflict, resolve it only when independent
  // measured views agree on the same candidate, or when the pulse-family
  // matrix has a decisive 3:4 node. This never creates a BPM and cannot
  // supersede an established recovery above.
  if (broadRoute &&
      std::abs(measured.bpm - current.bpm) >=
          kMirPolicyBroadCrossViewMinBpmDelta) {
    const bool dominantThreeQuarterNode =
        MirMeasuredPulseFamilyMatrix::matches(
            measured.bpm, current.bpm, MirPulseRelation::three_quarters,
            kMirPolicyBroadCrossViewThreeQuarterTolerance) &&
        mir_has_alias(measured.candidate.alias_bits,
                      MirAlias::three_quarters) &&
        measuredLocalRatio >=
            kMirPolicyBroadCrossViewThreeQuarterMinLocalRatio;

    bool strongPartialAgreement = false;
    bool octavePartialAgreement = false;
    if (partialBarAggregate != nullptr && partialBarAggregate->candidate &&
        partialBarAggregate->total_segment_count > 0) {
      const double totalSegments =
          static_cast<double>(partialBarAggregate->total_segment_count);
      const double rank1Support =
          static_cast<double>(partialBarAggregate->rank1_hits) / totalSegments;
      const double partialDelta = std::abs(
          partialBarAggregate->local_exact_bpm - measured.bpm);
      strongPartialAgreement =
          partialDelta <= kMirPolicyBroadCrossViewPartialMaxDelta &&
          measuredMargin >= kMirPolicyBroadCrossViewPartialMinMargin &&
          measuredLocalRatio >= kMirPolicyBroadCrossViewPartialMinLocalRatio &&
          rank1Support >= kMirPolicyBroadCrossViewPartialMinRank1Support &&
          partialBarAggregate->runner_up_score_ratio <=
              kMirPolicyBroadCrossViewPartialMaxRunnerRatio;
      const bool octaveRelation =
          MirMeasuredPulseFamilyMatrix::matches(
              measured.bpm, current.bpm, MirPulseRelation::half,
              kMirPolicyBroadCrossViewOctaveRelationTolerance) ||
          MirMeasuredPulseFamilyMatrix::matches(
              measured.bpm, current.bpm, MirPulseRelation::double_time,
              kMirPolicyBroadCrossViewOctaveRelationTolerance);
      octavePartialAgreement =
          partialDelta <= kMirPolicyBroadCrossViewOctaveMaxDelta &&
          measuredLocalRatio >= kMirPolicyBroadCrossViewOctaveMinLocalRatio &&
          octaveRelation;
    }

    if (dominantThreeQuarterNode || strongPartialAgreement ||
        octavePartialAgreement) {
      reason = "measured_broad_route_cross_view_family_recovery_gate";
      return &measured;
    }
  }
  return nullptr;
}

[[nodiscard]] const MirPolicyMeasuredCandidate*
select_mir_policy_measured_center_continuous_recovery(
    std::span<const MirPolicyMeasuredCandidate> candidates,
    const MirPolicyMeasuredCandidate& current,
    double priorCenterBpm,
    bool priorUsable) noexcept {
  if (!priorUsable) {
    return nullptr;
  }
  const double currentLocalScore =
      (std::max)(current.local_exact_score, 0.000001);
  const MirPolicyMeasuredCandidate* selected = nullptr;
  for (const auto& candidate : candidates) {
    if (std::abs(candidate.bpm - current.bpm) < 3.0 ||
        std::abs(candidate.bpm - priorCenterBpm) > 2.0 ||
        !mir_has_alias(candidate.candidate.alias_bits,
                           MirAlias::direct) ||
        candidate.candidate.continuous_score < 0.17 ||
        candidate.candidate.continuous_score_ratio < 1.10 ||
        candidate.candidate.support < 0.20 ||
        candidate.local_exact_score / currentLocalScore < 1.15) {
      continue;
    }
    if (selected == nullptr ||
        std::make_tuple(
            candidate.candidate.continuous_score,
            candidate.candidate.continuous_section_support,
            candidate.local_exact_score, candidate.candidate.support,
            -std::abs(candidate.bpm - priorCenterBpm)) >
            std::make_tuple(
                selected->candidate.continuous_score,
                selected->candidate.continuous_section_support,
                selected->local_exact_score, selected->candidate.support,
                -std::abs(selected->bpm - priorCenterBpm))) {
      selected = &candidate;
    }
  }
  return selected;
}

[[nodiscard]] const MirPolicyMeasuredCandidate*
select_mir_policy_low_pulse_hold_recovery(
    std::span<const MirPolicyMeasuredCandidate> candidates,
    const MirPolicyMeasuredCandidate& current) noexcept {
  const double currentLocalScore =
      (std::max)(current.local_exact_score, 0.000001);
  const MirPolicyMeasuredCandidate* selected = nullptr;
  for (const auto& candidate : candidates) {
    const double delta = candidate.bpm - current.bpm;
    const uint16_t aliases = candidate.candidate.alias_bits;
    const bool aliasEligible =
        mir_has_alias(aliases, MirAlias::direct) ||
        mir_has_alias(aliases, MirAlias::half) ||
        mir_has_alias(aliases, MirAlias::two_thirds) ||
        mir_has_alias(aliases, MirAlias::three_quarters);
    if (delta < -5.0 || delta > -0.5 ||
        candidate.local_exact_score / currentLocalScore < 1.05 ||
        candidate.candidate.continuous_score < 0.17 ||
        candidate.candidate.continuous_score_ratio < 1.10 ||
        candidate.candidate.continuous_section_stability < 0.70 ||
        !aliasEligible) {
      continue;
    }
    if (selected == nullptr ||
        std::make_tuple(
            candidate.candidate.continuous_score,
            candidate.candidate.continuous_section_stability,
            candidate.local_exact_score, candidate.candidate.support,
            -candidate.bpm) >
            std::make_tuple(
                selected->candidate.continuous_score,
                selected->candidate.continuous_section_stability,
                selected->local_exact_score, selected->candidate.support,
                -selected->bpm)) {
      selected = &candidate;
    }
  }
  return selected;
}

[[nodiscard]] const MirPolicyMeasuredCandidate*
select_mir_policy_sparse_pulse_hold_recovery(
    std::span<const MirPolicyMeasuredCandidate> candidates,
    const MirPolicyMeasuredCandidate& current) noexcept {
  const double currentLocalScore =
      (std::max)(current.local_exact_score, 0.000001);
  const MirPolicyMeasuredCandidate* selected = nullptr;
  for (const auto& candidate : candidates) {
    const double delta = candidate.bpm - current.bpm;
    if (delta < 10.0 || delta > 20.0 ||
        candidate.local_exact_score / currentLocalScore < 1.0 ||
        candidate.candidate.support < 0.06 ||
        candidate.candidate.winner_support < 0.06 ||
        candidate.candidate.pulse_score < 0.22 ||
        !mir_has_alias(candidate.candidate.alias_bits,
                           MirAlias::two_thirds)) {
      continue;
    }
    if (selected == nullptr ||
        std::make_tuple(candidate.candidate.pulse_score,
                        candidate.local_exact_score,
                        candidate.candidate.support, -candidate.bpm) >
            std::make_tuple(selected->candidate.pulse_score,
                            selected->local_exact_score,
                            selected->candidate.support, -selected->bpm)) {
      selected = &candidate;
    }
  }
  return selected;
}

[[nodiscard]] bool mir_policy_sparse_current_direct_release(
    const MirPolicyMeasuredCandidate& measured,
    const smart_tempo::MirPrimarySelection& current,
    double measuredMargin) noexcept {
  return current.sparse_acapella_review_hold &&
         std::abs(measured.bpm - current.selected_bpm) <= 0.30 &&
         mir_has_alias(measured.candidate.alias_bits,
                           MirAlias::direct) &&
         measured.candidate.support >= 0.85 &&
         measured.candidate.winner_support >= 0.50 &&
         measured.local_exact_score >= 0.25 && measuredMargin >= 0.20;
}

[[nodiscard]] bool mir_policy_protected_near_measured_release(
    const MirPolicyMeasuredCandidate& measured,
    const smart_tempo::MirPrimarySelection& current,
    double measuredMargin) noexcept {
  if (current.low_pulse_exactness_review_hold ||
      current.sparse_acapella_review_hold ||
      !mir_has_alias(measured.candidate.alias_bits,
                         MirAlias::direct)) {
    return false;
  }
  if (std::abs(measured.bpm - current.selected_bpm) <= 0.30 &&
      measured.candidate.support >= 0.80 &&
      measured.local_exact_score >= 0.24 && measuredMargin >= 0.08) {
    return true;
  }
  return std::abs(measured.bpm - current.selected_bpm) <= 1.25 &&
         measured.candidate.support >= 0.60 &&
         measured.local_exact_score >= 0.24 && measuredMargin >= 0.12;
}

[[nodiscard]] bool mir_policy_protected_half_alias_current_release(
    const MirPolicyMeasuredCandidate& measured,
    const smart_tempo::MirPrimarySelection& current,
    double measuredMargin) noexcept {
  if (current.low_pulse_exactness_review_hold ||
      current.sparse_acapella_review_hold || !(measured.bpm > 0.0) ||
      !mir_has_alias(measured.candidate.alias_bits,
                         MirAlias::direct)) {
    return false;
  }
  return MirMeasuredPulseFamilyMatrix::matches(
             current.selected_bpm, measured.bpm,
             MirPulseRelation::double_time, 0.035) &&
         measured.candidate.support >= 0.70 &&
         measured.local_exact_score >= 0.22 && measuredMargin >= 0.12;
}

[[nodiscard]] bool mir_policy_protected_strong_measured_direct_release(
    const MirPolicyMeasuredCandidate& measured,
    const smart_tempo::MirPrimarySelection& current,
    double measuredMargin) noexcept {
  if (current.low_pulse_exactness_review_hold ||
      current.sparse_acapella_review_hold ||
      !mir_has_alias(measured.candidate.alias_bits,
                         MirAlias::direct)) {
    return false;
  }
  const double currentLocal =
      (std::max)(current.selected_local_exact_score, 0.000001);
  return measured.candidate.support >= 0.60 &&
         measured.local_exact_score >= 0.24 && measuredMargin >= 0.15 &&
         measured.local_exact_score / currentLocal >= 1.35;
}

[[nodiscard]] const MirPolicyMeasuredCandidate*
select_mir_policy_stable_sparse_low_pulse_current_release(
    std::span<const MirPolicyMeasuredCandidate> candidates,
    const smart_tempo::MirPrimarySelection& current) noexcept {
  if (candidates.empty() || !current.sparse_acapella_review_hold ||
      std::strcmp(current.lane, "low_pulse_context") != 0 ||
      current.selected_local_exact_bpm < 45.0 ||
      current.selected_local_exact_bpm > 60.0 ||
      std::abs(current.selected_local_exact_bpm - current.selected_bpm) >
          0.15 ||
      current.selected_local_exact_score < 0.18 ||
      current.continuous_score < 0.18 ||
      current.continuous_score_ratio < 1.15 ||
      current.continuous_section_support < 0.30 ||
      current.lane_low_support < 0.35 || current.total_segment_count < 20 ||
      current.odf_peak_count < 500) {
    return nullptr;
  }
  const MirPolicyMeasuredCandidate* selected =
      find_mir_policy_output_candidate(candidates,
                                    current.selected_local_exact_bpm);
  if (selected == nullptr ||
      std::abs(selected->bpm - current.selected_local_exact_bpm) > 0.25) {
    return nullptr;
  }
  return selected;
}

[[nodiscard]] const MirPolicyMeasuredCandidate*
select_mir_policy_sparse_top_family_half_alias_release(
    std::span<const MirPolicyMeasuredCandidate> candidates,
    const smart_tempo::MirPrimarySelection& current) noexcept {
  if (candidates.empty() || !current.sparse_acapella_review_hold ||
      std::strcmp(current.lane, "low_pulse_context") != 0 ||
      current.continuous_score > 0.02) {
    return nullptr;
  }
  const MirPolicyMeasuredCandidate* selected = nullptr;
  for (const auto& candidate : candidates) {
    const double sourceFamilyBpm = candidate.bpm * 2.0;
    if (!mir_has_alias(candidate.candidate.alias_bits,
                           MirAlias::half) ||
        candidate.bpm < 60.0 || candidate.bpm > 75.0 ||
        sourceFamilyBpm < 128.0 || sourceFamilyBpm > 142.0 ||
        candidate.candidate.support < 0.15 ||
        candidate.local_exact_score < 0.12) {
      continue;
    }
    if (selected == nullptr ||
        std::make_tuple(candidate.candidate.support, candidate.base_score,
                        candidate.local_exact_score,
                        -std::abs(candidate.bpm - 67.5)) >
            std::make_tuple(selected->candidate.support,
                            selected->base_score,
                            selected->local_exact_score,
                            -std::abs(selected->bpm - 67.5))) {
      selected = &candidate;
    }
  }
  return selected;
}

[[nodiscard]] const MirPolicyMeasuredCandidate*
select_mir_policy_high_family_conflict_fullboard_release(
    std::span<const MirPolicyMeasuredCandidate> candidates,
    const smart_tempo::MirPrimarySelection& current,
    double priorCenterBpm,
    double priorSpreadBpm,
    bool routeMatched,
    bool genericRoute) noexcept {
  if (candidates.empty() || !current.review_hold ||
      !current.high_family_conflict_review_hold ||
      !(current.selected_bpm > 0.0) || !routeMatched || genericRoute ||
      !(priorCenterBpm > 0.0) || !(priorSpreadBpm > 0.0)) {
    return nullptr;
  }

  const MirPolicyMeasuredCandidate& measured = candidates.front();
  const double measuredMargin =
      candidates.size() > 1 ? measured.base_score - candidates[1].base_score
                            : measured.base_score;
  const double currentLocalScore =
      (std::max)(current.selected_local_exact_score, 0.000001);
  const double currentSupport =
      (std::max)(current.support, 0.000001);
  const double localRatio = measured.local_exact_score / currentLocalScore;
  const double supportRatio = measured.candidate.support / currentSupport;
  const double currentDelta = measured.bpm - current.selected_bpm;
  const double centerDistance = std::abs(measured.bpm - priorCenterBpm);

  if (!mir_has_alias(measured.candidate.alias_bits, MirAlias::direct) ||
      currentDelta < 4.0 || currentDelta > 16.0 ||
      !mir_policy_within_center_gate(centerDistance, priorSpreadBpm) ||
      measured.candidate.support < 0.70 ||
      measured.local_exact_score < 0.30 || measuredMargin < 0.35 ||
      localRatio < 1.60 || supportRatio < 0.65) {
    return nullptr;
  }

  return &measured;
}

[[nodiscard]] bool mir_policy_family_conflict_has_stable_alias(
    const MirPolicyMeasuredCandidate& measured) noexcept {
  const uint16_t aliasBits = measured.candidate.alias_bits;
  return mir_has_alias(aliasBits, MirAlias::direct) ||
         mir_has_alias(aliasBits, MirAlias::two_thirds) ||
         mir_has_alias(aliasBits, MirAlias::three_quarters) ||
         mir_has_alias(aliasBits, MirAlias::three_halves);
}

[[nodiscard]] bool mir_policy_family_conflict_measured_candidate_release(
    const MirPolicyMeasuredCandidate& measured,
    const MirPolicyMeasuredCandidate& conflict,
    double currentBpm,
    double conflictLocalScoreRatio) noexcept {
  const bool measuredIsUpwardAlias =
      currentBpm > 0.0 && measured.bpm > currentBpm + 8.0 &&
      conflict.bpm > 0.0 && conflict.bpm < measured.bpm - 10.0 &&
      !mir_has_alias(measured.candidate.alias_bits, MirAlias::direct);
  const bool conflictHasDirectPulse =
      mir_has_alias(conflict.candidate.alias_bits, MirAlias::direct) &&
      conflict.candidate.support >= 0.80 &&
      conflict.local_exact_score >= 0.20 &&
      conflict.candidate.continuous_score >= 0.20 &&
      conflict.candidate.continuous_section_support >= 0.80;
  if (measuredIsUpwardAlias && conflictHasDirectPulse) {
    return false;
  }
  return mir_policy_family_conflict_has_stable_alias(measured) &&
         measured.local_exact_score >= 0.28 &&
         measured.candidate.continuous_score >= 0.20 &&
         measured.candidate.continuous_section_support >= 0.90 &&
         conflictLocalScoreRatio >= 0.80;
}

[[nodiscard]] bool mir_policy_family_conflict_downward_measured_release(
    const MirPolicyMeasuredCandidate& measured,
    double currentBpm,
    double conflictLocalScoreRatio) noexcept {
  if (!(currentBpm > 0.0) || !(measured.bpm > 0.0) ||
      !(measured.bpm < currentBpm - 5.0)) {
    return false;
  }
  if (!mir_pulse_ratio_in_range(currentBpm, measured.bpm, 1.20, 1.55) ||
      !mir_policy_family_conflict_has_stable_alias(measured)) {
    return false;
  }
  const bool strongLocalContinuousWitness =
      measured.local_exact_score >= 0.25 &&
      measured.candidate.continuous_score >= 0.20 &&
      measured.candidate.continuous_section_support >= 0.75 &&
      conflictLocalScoreRatio >= 1.10;
  const bool strongSegmentWinnerWitness =
      measured.candidate.winner_support >= 0.75 &&
      measured.local_exact_score >= 0.20 &&
      measured.candidate.continuous_score >= 0.20 &&
      measured.candidate.continuous_section_support >= 0.80 &&
      conflictLocalScoreRatio >= 0.90;
  const bool broadMeasuredWitness =
      measured.candidate.support >= 0.74 &&
      measured.local_exact_score >= 0.20 &&
      measured.candidate.continuous_score >= 0.20 &&
      measured.candidate.continuous_section_support >= 0.74 &&
      conflictLocalScoreRatio >= 1.25;
  return strongLocalContinuousWitness || strongSegmentWinnerWitness ||
         broadMeasuredWitness;
}

[[nodiscard]] bool mir_policy_family_conflict_lower_harmonic_release(
    const MirPolicyMeasuredCandidate& measured,
    const MirPolicyMeasuredCandidate& conflict,
    double conflictLocalScoreRatio,
    double conflictBaseScoreRatio,
    double centerImprovementBpm,
    double priorCenterBpm,
    double priorSpreadBpm,
    bool priorUsable) noexcept {
  if (!(measured.bpm > 0.0 && conflict.bpm > 0.0) ||
      !(conflict.bpm < measured.bpm)) {
    return false;
  }
  const bool lowerHalf = MirMeasuredPulseFamilyMatrix::matches(
      conflict.bpm, measured.bpm, MirPulseRelation::half, 0.02);
  const bool lowerTwoThirds = MirMeasuredPulseFamilyMatrix::matches(
      conflict.bpm, measured.bpm, MirPulseRelation::two_thirds, 0.02);
  const bool centeredMeasuredFamily =
      priorUsable && priorSpreadBpm > 0.0 &&
      std::abs(conflict.bpm - priorCenterBpm) / priorSpreadBpm <= 0.45 &&
      conflict.candidate.support >= 0.95 &&
      conflict.local_exact_score >= 0.25 &&
      conflict.candidate.continuous_score >= 0.245 &&
      conflict.candidate.continuous_section_support >= 0.70 &&
      conflictLocalScoreRatio >= 0.85 && conflictBaseScoreRatio >= 0.95;
  if (lowerHalf) {
    return centeredMeasuredFamily ||
           (conflict.candidate.support >= 0.98 &&
           conflict.local_exact_score >= 0.30 &&
           conflict.candidate.continuous_score >= 0.24 &&
           conflict.candidate.continuous_section_support >= 0.95 &&
           conflictLocalScoreRatio >= 0.70 &&
           conflictBaseScoreRatio >= 0.80 &&
           centerImprovementBpm >= 40.0);
  }
  if (lowerTwoThirds) {
    return centeredMeasuredFamily ||
           (conflict.candidate.support >= 0.68 &&
           conflict.local_exact_score >= 0.21 &&
           conflict.candidate.continuous_score >= 0.21 &&
           conflict.candidate.continuous_section_support >= 0.68 &&
           conflictLocalScoreRatio >= 0.77 &&
           conflictBaseScoreRatio >= 0.83 &&
           centerImprovementBpm >= 40.0);
  }
  return false;
}

[[nodiscard]] bool mir_policy_family_conflict_alternative_release(
    const MirPolicyMeasuredCandidate& measured,
    const MirPolicyMeasuredCandidate& conflict,
    double measuredBaseMargin,
    double conflictBaseScoreRatio) noexcept {
  if (!(measured.bpm > 0.0) || !(conflict.bpm > 0.0) ||
      std::abs(measured.bpm - conflict.bpm) <= 1.0) {
    return false;
  }
  return measuredBaseMargin <= 0.05 && conflictBaseScoreRatio >= 1.50 &&
         mir_policy_family_conflict_has_stable_alias(conflict) &&
         conflict.candidate.support >= 0.68 &&
         conflict.local_exact_score >= 0.25 &&
         conflict.candidate.continuous_score >= 0.245 &&
         conflict.candidate.continuous_section_support >= 0.65;
}

[[nodiscard]] bool
mir_policy_family_conflict_strong_continuous_alternative_release(
    const MirPolicyMeasuredCandidate& measured,
    const MirPolicyMeasuredCandidate& conflict,
    double measuredBaseMargin,
    double conflictBaseScoreRatio) noexcept {
  if (!(measured.bpm > 0.0) || !(conflict.bpm > 0.0) ||
      std::abs(measured.bpm - conflict.bpm) <= 1.0) {
    return false;
  }
  return conflict.candidate.support >= 0.90 &&
         conflict.local_exact_score >= 0.25 &&
         conflict.candidate.continuous_score >= 0.24 &&
         conflict.candidate.continuous_section_support >= 0.80 &&
         measuredBaseMargin <= 0.10 && conflictBaseScoreRatio >= 0.90;
}

[[nodiscard]] bool mir_policy_family_conflict_strong_pulse_alternative_release(
    const MirPolicyMeasuredCandidate& conflict,
    double measuredBaseMargin,
    double centerImprovementBpm) noexcept {
  return conflict.bpm > 0.0 && conflict.candidate.support >= 0.74 &&
         conflict.local_exact_score >= 0.25 &&
         conflict.candidate.pulse_score >= 0.30 &&
         conflict.candidate.pulse_section_support >= 0.50 &&
         measuredBaseMargin <= 0.03 && centerImprovementBpm >= 15.0;
}

[[nodiscard]] bool
mir_policy_family_conflict_direct_high_support_alternative_release(
    const MirPolicyMeasuredCandidate& measured,
    const MirPolicyMeasuredCandidate& conflict,
    double conflictLocalScoreRatio,
    double conflictBaseScoreRatio,
    double centerImprovementBpm) noexcept {
  if (!(measured.bpm > 0.0) || !(conflict.bpm > 0.0) ||
      std::abs(measured.bpm - conflict.bpm) <= 1.0) {
    return false;
  }
  return mir_has_alias(conflict.candidate.alias_bits,
                           MirAlias::direct) &&
         conflict.candidate.support >= 0.95 &&
         conflict.local_exact_score >= 0.155 &&
         conflictLocalScoreRatio >= 1.02 && conflictBaseScoreRatio >= 0.82 &&
         centerImprovementBpm >= 8.0 && measured.candidate.support <= 0.40;
}

[[nodiscard]] const MirPolicyMeasuredCandidate*
select_mir_policy_family_conflict_center_challenger(
    std::span<const MirPolicyMeasuredCandidate> candidates,
    double priorCenterBpm,
    double priorSpreadBpm,
    bool routeMatched,
    bool genericRoute) {
  if (!routeMatched || genericRoute || priorCenterBpm <= 0.0 ||
      priorSpreadBpm <= 0.0) {
    return nullptr;
  }

  struct EligibleCandidate {
    const MirPolicyMeasuredCandidate* candidate = nullptr;
    double evidence = 0.0;
  };
  std::vector<EligibleCandidate> eligible;
  eligible.reserve(candidates.size());
  for (const auto& candidate : candidates) {
    const double normalizedDistance =
        std::abs(candidate.bpm - priorCenterBpm) / priorSpreadBpm;
    const double evidence =
        candidate.local_exact_score + candidate.candidate.continuous_score +
        0.25 * candidate.candidate.support +
        0.20 * candidate.candidate.winner_support;
    if (normalizedDistance >
            kMirPolicyCenterChallengerMaxNormalizedDistance ||
        candidate.candidate.support < 0.40 ||
        candidate.local_exact_score < 0.175 || evidence < 0.48) {
      continue;
    }
    eligible.push_back({&candidate, evidence});
  }
  std::sort(
      eligible.begin(), eligible.end(),
      [priorCenterBpm](const EligibleCandidate& lhs,
                       const EligibleCandidate& rhs) {
        return std::make_tuple(
                   lhs.evidence, lhs.candidate->local_exact_score,
                   lhs.candidate->candidate.support,
                   lhs.candidate->candidate.winner_support,
                   -std::abs(lhs.candidate->bpm - priorCenterBpm),
                   -lhs.candidate->bpm) >
               std::make_tuple(
                   rhs.evidence, rhs.candidate->local_exact_score,
                   rhs.candidate->candidate.support,
                   rhs.candidate->candidate.winner_support,
                   -std::abs(rhs.candidate->bpm - priorCenterBpm),
                   -rhs.candidate->bpm);
      });
  if (eligible.empty()) {
    return nullptr;
  }
  const double runnerEvidence =
      eligible.size() > 1 ? eligible[1].evidence : 0.0;
  return eligible.front().evidence - runnerEvidence >= 0.08
             ? eligible.front().candidate
             : nullptr;
}

[[nodiscard]] const MirPolicyMeasuredCandidate*
select_mir_policy_dominant_segment_family_recovery(
    std::span<const MirPolicyMeasuredCandidate> candidates, double currentBpm,
    const smart_tempo::HodgkinsonTatumProbeResult& primaryFamily) noexcept {
  if (!primaryFamily.candidate || !(currentBpm > 0.0) ||
      primaryFamily.family_rank_count == 0) {
    return nullptr;
  }
  const double topBpm = primaryFamily.family_rank_bpm[0];
  if (topBpm < 90.0 || topBpm > 105.0) {
    return nullptr;
  }
  const double currentRatio = currentBpm / topBpm;
  if (currentRatio < 1.45 || currentRatio > 1.55) {
    return nullptr;
  }
  const double topScore = primaryFamily.family_rank_score[0];
  const double runnerScore =
      primaryFamily.family_rank_count > 1
          ? primaryFamily.family_rank_score[1]
          : primaryFamily.runner_up_family_score;
  const double dominance =
      runnerScore > 0.0 ? topScore / runnerScore : topScore;
  const std::size_t fitSegments =
      primaryFamily.fit_segment_count > 0 ? primaryFamily.fit_segment_count
                                          : primaryFamily.segment_count;
  const std::size_t totalSegments =
      primaryFamily.total_segment_count > 0
          ? primaryFamily.total_segment_count
          : primaryFamily.segment_count;
  const double consensusSupport =
      fitSegments > 0
          ? static_cast<double>(primaryFamily.family_rank_support[0]) /
                static_cast<double>(fitSegments)
          : 0.0;
  const double candidateConsensusSupport =
      primaryFamily.candidate_segment_count > 0
          ? static_cast<double>(
                primaryFamily.family_rank_candidate_support[0]) /
                static_cast<double>(primaryFamily.candidate_segment_count)
          : 0.0;
  const double candidateSegmentRatio =
      totalSegments > 0
          ? static_cast<double>(primaryFamily.candidate_segment_count) /
                static_cast<double>(totalSegments)
          : 0.0;
  if (dominance < 10.0 || consensusSupport < 0.80 ||
      candidateConsensusSupport < 0.95 || candidateSegmentRatio < 0.35 ||
      candidateSegmentRatio > 0.55) {
    return nullptr;
  }
  for (std::size_t index = 0; index < primaryFamily.family_rank_count;
       ++index) {
    const double rankBpm = primaryFamily.family_rank_bpm[index];
    if (rankBpm > 0.0 && MirMeasuredPulseFamilyMatrix::matches(
                               rankBpm, topBpm,
                               MirPulseRelation::three_halves, 0.03)) {
      return nullptr;
    }
  }

  const MirPolicyMeasuredCandidate* selected = nullptr;
  for (const auto& candidate : candidates) {
    if (std::abs(candidate.cluster_bpm - topBpm) > 0.25 ||
        !mir_has_alias(candidate.candidate.alias_bits,
                           MirAlias::direct) ||
        candidate.candidate.support < 0.80) {
      continue;
    }
    if (selected == nullptr ||
        std::make_tuple(candidate.candidate.support,
                        candidate.candidate.winner_support,
                        candidate.local_exact_score, candidate.base_score) >
            std::make_tuple(selected->candidate.support,
                            selected->candidate.winner_support,
                            selected->local_exact_score,
                            selected->base_score)) {
      selected = &candidate;
    }
  }
  return selected;
}

[[nodiscard]] bool mir_policy_family_conflict_residual_center_valid(
    const MirPolicyMeasuredCandidate& measured,
    double priorCenterBpm,
    double priorSpreadBpm,
    bool routeMatched,
    bool genericRoute) noexcept {
  return routeMatched && !genericRoute && priorCenterBpm > 0.0 &&
         priorSpreadBpm > 0.0 &&
         std::abs(measured.bpm - priorCenterBpm) / priorSpreadBpm <= 2.0;
}

[[nodiscard]] bool mir_policy_family_conflict_residual_margin_release(
    const MirPolicyMeasuredCandidate& measured,
    const MirPolicyMeasuredCandidate& conflict,
    double measuredBaseMargin,
    double priorCenterBpm,
    double priorSpreadBpm,
    bool routeMatched,
    bool genericRoute) noexcept {
  return mir_policy_family_conflict_has_stable_alias(measured) &&
         mir_policy_family_conflict_residual_center_valid(
             measured, priorCenterBpm, priorSpreadBpm, routeMatched,
             genericRoute) &&
         measuredBaseMargin >= 0.30 && conflict.candidate.support < 0.99 &&
         measured.local_exact_score >= 0.20 &&
         measured.candidate.continuous_score >= 0.20 &&
         measured.candidate.continuous_section_support >= 0.58;
}

[[nodiscard]] bool
mir_policy_family_conflict_strong_centered_measured_winner_release(
    const MirPolicyMeasuredCandidate& measured,
    double measuredBaseMargin,
    double priorCenterBpm,
    double priorSpreadBpm,
    bool routeMatched,
    bool genericRoute) noexcept {
  return routeMatched && !genericRoute && priorCenterBpm > 0.0 &&
         priorSpreadBpm > 0.0 &&
         mir_has_alias(measured.candidate.alias_bits,
                           MirAlias::direct) &&
         measuredBaseMargin >= 0.30 && measured.candidate.support >= 0.75 &&
         measured.local_exact_score >= 0.28 &&
         measured.candidate.continuous_score >= 0.28 &&
         measured.candidate.continuous_section_support >= 0.75 &&
         std::abs(measured.bpm - priorCenterBpm) / priorSpreadBpm <= 0.80;
}

[[nodiscard]] bool
mir_policy_family_conflict_normalized_measured_winner_release(
    const MirPolicyMeasuredCandidate& measured,
    const MirPolicyMeasuredCandidate& conflict,
    double candidateLocalScoreRatio,
    double priorCenterBpm,
    double priorSpreadBpm,
    bool routeMatched,
    bool genericRoute) noexcept {
  const std::uint32_t aliases = measured.candidate.alias_bits;
  const bool forbiddenAlias =
      mir_has_alias(aliases, MirAlias::half) ||
      mir_has_alias(aliases, MirAlias::double_time) ||
      mir_has_alias(aliases, MirAlias::one_third) ||
      mir_has_alias(aliases, MirAlias::two_thirds) ||
      mir_has_alias(aliases, MirAlias::triple);
  return mir_policy_family_conflict_residual_center_valid(
             measured, priorCenterBpm, priorSpreadBpm, routeMatched,
             genericRoute) &&
         !forbiddenAlias && measured.candidate.support >= 0.28 &&
         measured.local_exact_score >= 0.18 &&
         measured.candidate.continuous_score >= 0.18 &&
         measured.candidate.continuous_section_support >= 0.28 &&
         measured.candidate.continuous_score_ratio >= 1.18 &&
         candidateLocalScoreRatio >= 1.0 &&
         conflict.candidate.continuous_section_support <= 0.000001;
}

[[nodiscard]] bool mir_policy_family_conflict_residual_two_thirds_release(
    const MirPolicyMeasuredCandidate& measured,
    double currentBpm,
    double conflictLocalScoreRatio,
    double priorCenterBpm,
    double priorSpreadBpm,
    bool routeMatched,
    bool genericRoute) noexcept {
  if (!(currentBpm > 0.0)) return false;
  return mir_policy_family_conflict_has_stable_alias(measured) &&
         mir_policy_family_conflict_residual_center_valid(
             measured, priorCenterBpm, priorSpreadBpm, routeMatched,
             genericRoute) &&
         MirMeasuredPulseFamilyMatrix::matches(
             measured.bpm, currentBpm, MirPulseRelation::two_thirds, 0.02) &&
         measured.candidate.support >= 0.95 &&
         measured.candidate.winner_support >= 0.30 &&
         conflictLocalScoreRatio >= 1.10 &&
         measured.candidate.continuous_score >= 0.20 &&
         measured.candidate.continuous_section_support >= 0.90;
}

[[nodiscard]] const MirPolicyMeasuredCandidate*
select_mir_policy_family_conflict_center_exact_release(
    std::span<const MirPolicyMeasuredCandidate> candidates,
    const MirPolicyMeasuredCandidate& outputCandidate,
    double priorCenterBpm,
    double priorSpreadBpm,
    bool routeMatched,
    bool genericRoute) noexcept {
  if (!routeMatched || genericRoute || priorCenterBpm <= 0.0 ||
      priorSpreadBpm <= 0.0) {
    return nullptr;
  }
  const double outputDistance =
      std::abs(outputCandidate.bpm - priorCenterBpm);
  const MirPolicyMeasuredCandidate* selected = nullptr;
  for (const auto& candidate : candidates) {
    if (std::abs(candidate.bpm - outputCandidate.bpm) < 15.0) {
      continue;
    }
    const double centerDistance =
        std::abs(candidate.bpm - priorCenterBpm);
    const double centerImprovement = outputDistance - centerDistance;
    const bool eligible =
        centerDistance <= kMirPolicyCenterGateAbsoluteCapBpm &&
        centerDistance / priorSpreadBpm <=
            kMirPolicyCenterGateNormalizedLimit &&
        centerImprovement >= 25.0 &&
        mir_has_alias(candidate.candidate.alias_bits,
                          MirAlias::direct) &&
        candidate.candidate.support >= 0.95 &&
        candidate.candidate.continuous_score >= 0.20 &&
        candidate.candidate.continuous_section_support >= 0.90 &&
        candidate.local_exact_score >=
            outputCandidate.local_exact_score * 1.05 &&
        candidate.base_score >= outputCandidate.base_score * 0.90;
    if (!eligible) {
      continue;
    }
    if (selected == nullptr ||
        std::make_tuple(
            candidate.candidate.continuous_section_support,
            candidate.candidate.support,
            candidate.candidate.continuous_score,
            candidate.local_exact_score,
            -std::abs(candidate.bpm - priorCenterBpm)) >
            std::make_tuple(
                selected->candidate.continuous_section_support,
                selected->candidate.support,
                selected->candidate.continuous_score,
                selected->local_exact_score,
                -std::abs(selected->bpm - priorCenterBpm))) {
      selected = &candidate;
    }
  }
  return selected;
}

struct MirPolicyReleaseFeature {
  const MirPolicyMeasuredCandidate* candidate = nullptr;
  double base = 0.0;
  double local = 0.0;
  double evidence = 0.0;
  double support = 0.0;
  double family_strength = 0.0;
  double center_proximity = 0.0;
};

[[nodiscard]] double mir_policy_release_evidence(
    const MirPolicyMeasuredCandidate& candidate) noexcept {
  return std::max(candidate.candidate.continuous_score,
                  candidate.candidate.pulse_score);
}

[[nodiscard]] double mir_policy_release_harmonic_closeness(
    double lowerBpm, double upperBpm) noexcept {
  if (lowerBpm <= 0.0 || upperBpm <= lowerBpm) {
    return 0.0;
  }
  constexpr std::array<MirPulseRelation, 5> relations = {
      MirPulseRelation::four_thirds, MirPulseRelation::three_halves,
      MirPulseRelation::double_time, MirPulseRelation::triple,
      MirPulseRelation::quadruple};
  const double relativeError =
      MirMeasuredPulseFamilyMatrix::minimum_relative_error(
          upperBpm, lowerBpm, relations);
  return std::max(0.0, 1.0 - relativeError / 0.018);
}

[[nodiscard]] std::vector<MirPolicyReleaseFeature>
build_mir_policy_release_features(
    std::span<const MirPolicyMeasuredCandidate> candidates,
    double priorCenterBpm,
    double priorSpreadBpm) {
  struct BucketCandidate {
    long long bucket = 0;
    const MirPolicyMeasuredCandidate* candidate = nullptr;
  };
  std::vector<BucketCandidate> unique;
  unique.reserve(candidates.size());
  for (const auto& candidate : candidates) {
    const long long bucket = static_cast<long long>(
        std::floor(candidate.bpm * 20.0 + 0.5));
    auto existing = std::find_if(
        unique.begin(), unique.end(),
        [bucket](const BucketCandidate& item) {
          return item.bucket == bucket;
        });
    if (existing == unique.end()) {
      unique.push_back({bucket, &candidate});
      continue;
    }
    const auto rank = [](const MirPolicyMeasuredCandidate& item) {
      return std::make_tuple(item.base_score, item.local_exact_score,
                             mir_policy_release_evidence(item),
                             item.candidate.support);
    };
    if (rank(candidate) > rank(*existing->candidate)) {
      existing->candidate = &candidate;
    }
  }
  if (unique.empty()) {
    return {};
  }
  double maxBase = 0.0;
  double maxLocal = 0.0;
  double maxEvidence = 0.0;
  for (const auto& item : unique) {
    maxBase = std::max(maxBase, item.candidate->base_score);
    maxLocal = std::max(maxLocal, item.candidate->local_exact_score);
    maxEvidence =
        std::max(maxEvidence, mir_policy_release_evidence(*item.candidate));
  }
  maxBase = maxBase > 0.0 ? maxBase : 1.0;
  maxLocal = maxLocal > 0.0 ? maxLocal : 1.0;
  maxEvidence = maxEvidence > 0.0 ? maxEvidence : 1.0;
  const double spread = std::max(priorSpreadBpm, 1.0);
  std::vector<MirPolicyReleaseFeature> features;
  features.reserve(unique.size());
  for (const auto& item : unique) {
    const double centerOffset =
        (item.candidate->bpm - priorCenterBpm) / spread;
    double familyStrength = 0.0;
    for (const auto& partner : unique) {
      const double closeness = mir_policy_release_harmonic_closeness(
          item.candidate->bpm, partner.candidate->bpm);
      if (closeness <= 0.0) {
        continue;
      }
      familyStrength = std::max(
          familyStrength,
          partner.candidate->base_score / maxBase * closeness);
    }
    features.push_back(
        {item.candidate,
         item.candidate->base_score / maxBase,
         item.candidate->local_exact_score / maxLocal,
         mir_policy_release_evidence(*item.candidate) / maxEvidence,
         item.candidate->candidate.support,
         familyStrength,
         std::exp(-0.5 * centerOffset * centerOffset)});
  }
  return features;
}

class MirPolicyReleaseFeatureCache {
 public:
  MirPolicyReleaseFeatureCache(
      std::span<const MirPolicyMeasuredCandidate> candidates,
      double priorCenterBpm, double priorSpreadBpm) noexcept
      : candidates_(candidates),
        prior_center_bpm_(priorCenterBpm),
        prior_spread_bpm_(priorSpreadBpm) {}

  [[nodiscard]] const std::vector<MirPolicyReleaseFeature>& get() {
    if (!ready_) {
      features_ = build_mir_policy_release_features(
          candidates_, prior_center_bpm_, prior_spread_bpm_);
      ready_ = true;
    }
    return features_;
  }

 private:
  std::span<const MirPolicyMeasuredCandidate> candidates_;
  double prior_center_bpm_ = 0.0;
  double prior_spread_bpm_ = 0.0;
  bool ready_ = false;
  std::vector<MirPolicyReleaseFeature> features_;
};

[[nodiscard]] const MirPolicyMeasuredCandidate*
select_mir_policy_release_ready_low_anchor(
    MirPolicyReleaseFeatureCache& featureCache) {
  auto features = featureCache.get();
  if (features.empty()) {
    return nullptr;
  }
  std::stable_sort(
      features.begin(), features.end(),
      [](const auto& left, const auto& right) {
        return left.base > right.base;
      });
  const auto& anchor = features.front();
  const double runnerBase = features.size() > 1 ? features[1].base : 0.0;
  const double anchorBpm = anchor.candidate->bpm;
  MirPulseRelation relation = MirPulseRelation::equivalent;
  if (anchorBpm <= 70.0 && anchor.base - runnerBase >= 0.10) {
    relation = MirPulseRelation::equivalent;
  } else if (anchorBpm >= 92.0 && anchorBpm <= 102.0) {
    relation = MirPulseRelation::two_thirds;
  } else if (anchorBpm >= 105.0 && anchorBpm <= 125.0) {
    relation = MirPulseRelation::half;
  } else if (anchorBpm >= 160.0 && anchorBpm <= 175.0) {
    relation = MirPulseRelation::one_third;
  } else {
    return nullptr;
  }
  const MirPolicyReleaseFeature* selected = nullptr;
  for (const auto& feature : features) {
    const double error = MirMeasuredPulseFamilyMatrix::relative_relation_error(
        feature.candidate->bpm, anchorBpm, relation);
    if (error > 0.01 || feature.candidate->local_exact_score < 0.15) {
      continue;
    }
    const auto key = std::make_tuple(
        error, -feature.local, -feature.evidence, -feature.support,
        -feature.base);
    if (selected == nullptr ||
        key < std::make_tuple(
                  MirMeasuredPulseFamilyMatrix::relative_relation_error(
                      selected->candidate->bpm, anchorBpm, relation),
                  -selected->local, -selected->evidence,
                  -selected->support, -selected->base)) {
      selected = &feature;
    }
  }
  return selected != nullptr ? selected->candidate : nullptr;
}

[[nodiscard]] const MirPolicyMeasuredCandidate*
select_mir_policy_release_ready_full_pulse(
    MirPolicyReleaseFeatureCache& featureCache,
    double priorCenterBpm,
    double priorSpreadBpm) {
  if (priorSpreadBpm <= 0.0) {
    return nullptr;
  }
  const auto& features = featureCache.get();
  const MirPolicyReleaseFeature* selected = nullptr;
  for (const auto& feature : features) {
    const auto& candidate = *feature.candidate;
    if ((candidate.bpm - priorCenterBpm) / priorSpreadBpm < 0.90 ||
        !mir_has_alias(candidate.candidate.alias_bits,
                          MirAlias::direct) ||
        !mir_has_alias(candidate.candidate.alias_bits,
                          MirAlias::double_time) ||
        candidate.local_exact_score < 0.175 ||
        candidate.candidate.continuous_score < 0.175 ||
        candidate.candidate.continuous_score_ratio < 1.15 ||
        candidate.candidate.support < 0.75 ||
        candidate.candidate.winner_support < 0.40) {
      continue;
    }
    const auto key = std::make_tuple(
        feature.base, feature.local, feature.evidence, feature.support,
        -candidate.bpm);
    if (selected == nullptr ||
        key > std::make_tuple(
                  selected->base, selected->local, selected->evidence,
                  selected->support, -selected->candidate->bpm)) {
      selected = &feature;
    }
  }
  return selected != nullptr ? selected->candidate : nullptr;
}

[[nodiscard]] const MirPolicyMeasuredCandidate*
select_mir_policy_release_ready_scored(
    MirPolicyReleaseFeatureCache& featureCache,
    double priorCenterBpm,
    double priorSpreadBpm,
    std::size_t maxRank,
    double maxCenterNorm,
    double currentBpm = 0.0,
    double minCurrentDelta = 0.0) {
  if (priorSpreadBpm <= 0.0) {
    return nullptr;
  }
  auto features = featureCache.get();
  std::stable_sort(
      features.begin(), features.end(),
      [](const auto& left, const auto& right) {
        return left.base > right.base;
      });
  const MirPolicyReleaseFeature* selected = nullptr;
  const std::size_t count = std::min(maxRank, features.size());
  const auto score = [](const MirPolicyReleaseFeature& feature) {
    return 0.20 * feature.center_proximity + 0.30 * feature.base +
           0.10 * feature.local + 0.10 * feature.evidence;
  };
  for (std::size_t index = 0; index < count; ++index) {
    const auto& feature = features[index];
    if (std::abs(feature.candidate->bpm - priorCenterBpm) /
                priorSpreadBpm >
            maxCenterNorm ||
        feature.candidate->local_exact_score < 0.16 ||
        std::abs(feature.candidate->bpm - currentBpm) < minCurrentDelta) {
      continue;
    }
    const auto key = std::make_tuple(
        score(feature), feature.center_proximity, feature.base,
        feature.evidence, feature.local, feature.support,
        -feature.candidate->bpm);
    if (selected == nullptr ||
        key > std::make_tuple(
                  score(*selected), selected->center_proximity,
                  selected->base, selected->evidence, selected->local,
                  selected->support, -selected->candidate->bpm)) {
      selected = &feature;
    }
  }
  return selected != nullptr ? selected->candidate : nullptr;
}

[[nodiscard]] const MirPolicyMeasuredCandidate*
select_mir_policy_release_ready_conflict_anchor(
    MirPolicyReleaseFeatureCache& featureCache,
    double priorCenterBpm) {
  if (priorCenterBpm <= 0.0) {
    return nullptr;
  }
  const auto& features = featureCache.get();
  if (features.empty()) {
    return nullptr;
  }
  const auto anchor = std::max_element(
      features.begin(), features.end(),
      [](const auto& left, const auto& right) {
        return left.base < right.base;
      });
  const MirPulseRelation relation =
      anchor->candidate->bpm < priorCenterBpm
          ? MirPulseRelation::four_thirds
          : (priorCenterBpm <= 88.0 ? MirPulseRelation::half
                                    : MirPulseRelation::two_thirds);
  const MirPolicyReleaseFeature* selected = nullptr;
  for (const auto& feature : features) {
    const double error = MirMeasuredPulseFamilyMatrix::relative_relation_error(
        feature.candidate->bpm, anchor->candidate->bpm, relation);
    if (error > 0.005 || feature.candidate->local_exact_score < 0.17 ||
        feature.candidate->candidate.support < 0.40) {
      continue;
    }
    const auto key = std::make_tuple(
        error, -feature.support, -feature.local, -feature.evidence,
        -feature.base);
    if (selected == nullptr ||
        key < std::make_tuple(
                  MirMeasuredPulseFamilyMatrix::relative_relation_error(
                      selected->candidate->bpm, anchor->candidate->bpm,
                      relation),
                  -selected->support, -selected->local,
                  -selected->evidence, -selected->base)) {
      selected = &feature;
    }
  }
  return selected != nullptr ? selected->candidate : nullptr;
}

[[nodiscard]] const MirPolicyMeasuredCandidate*
select_mir_policy_release_ready_sparse(
    MirPolicyReleaseFeatureCache& featureCache) {
  const auto& features = featureCache.get();
  if (features.empty()) {
    return nullptr;
  }
  const auto anchor = std::max_element(
      features.begin(), features.end(),
      [](const auto& left, const auto& right) {
        return left.base < right.base;
      });
  const double anchorBpm = anchor->candidate->bpm;
  MirPulseRelation relation = MirPulseRelation::equivalent;
  if (anchorBpm <= 72.0) {
    relation = MirPulseRelation::double_time;
  } else if (anchorBpm >= 85.0 && anchorBpm <= 102.0) {
    relation = MirPulseRelation::two_thirds;
  } else if (anchorBpm >= 112.0) {
    relation = MirPulseRelation::half;
  } else {
    return nullptr;
  }
  const MirPolicyReleaseFeature* selected = nullptr;
  for (const auto& feature : features) {
    const double error = MirMeasuredPulseFamilyMatrix::relative_relation_error(
        feature.candidate->bpm, anchorBpm, relation);
    if (error > 0.02 || feature.candidate->local_exact_score < 0.16 ||
        feature.candidate->candidate.support < 0.04) {
      continue;
    }
    const auto key = std::make_tuple(
        error, -feature.local, -feature.evidence, -feature.support,
        -feature.base);
    if (selected == nullptr ||
        key < std::make_tuple(
                  MirMeasuredPulseFamilyMatrix::relative_relation_error(
                      selected->candidate->bpm, anchorBpm, relation),
                  -selected->local, -selected->evidence,
                  -selected->support, -selected->base)) {
      selected = &feature;
    }
  }
  return selected != nullptr ? selected->candidate : nullptr;
}

[[nodiscard]] const MirPolicyMeasuredCandidate*
select_mir_policy_release_ready_low_family_plateau(
    MirPolicyReleaseFeatureCache& featureCache) {
  auto features = featureCache.get();
  features.erase(
      std::remove_if(
          features.begin(), features.end(),
          [](const MirPolicyReleaseFeature& feature) {
            return feature.candidate->bpm < 40.0 ||
                   feature.candidate->bpm > 85.0 ||
                   feature.family_strength < 0.90;
          }),
      features.end());
  if (features.empty()) {
    return nullptr;
  }
  const auto score = [](const MirPolicyReleaseFeature& feature) {
    return 0.50 * feature.family_strength +
           0.10 * feature.center_proximity + 0.05 * feature.base +
           0.15 * feature.local + 0.15 * feature.evidence +
           0.10 * feature.support;
  };
  std::stable_sort(
      features.begin(), features.end(),
      [&score](const auto& left, const auto& right) {
        return std::make_tuple(
                   score(left), left.family_strength, left.evidence,
                   left.local, left.base, -left.candidate->bpm) >
               std::make_tuple(
                   score(right), right.family_strength, right.evidence,
                   right.local, right.base, -right.candidate->bpm);
      });
  const double winnerScore = score(features.front());
  const double runnerScore =
      features.size() > 1 ? score(features[1]) : 0.0;
  if (winnerScore - runnerScore < 0.05) {
    return nullptr;
  }
  return features.front().candidate;
}

[[nodiscard]] bool mir_policy_partial_bar_family_conflict_release(
    const smart_tempo::HodgkinsonPartialBarAggregateShadow* partial,
    double priorCenterBpm, double priorSpreadBpm, bool routeMatched,
    bool genericRoute) noexcept {
  if (partial == nullptr || !partial->enabled || !partial->candidate ||
      !routeMatched || genericRoute || !(priorCenterBpm > 0.0) ||
      !(priorSpreadBpm > 0.0) || partial->total_segment_count == 0 ||
      !std::isfinite(partial->coarse_bpm) ||
      !std::isfinite(partial->local_exact_bpm) ||
      !std::isfinite(partial->local_exact_score) ||
      !std::isfinite(partial->runner_up_score_ratio)) {
    return false;
  }
  const double totalSegments =
      static_cast<double>(partial->total_segment_count);
  const double rank1Support =
      static_cast<double>(partial->rank1_hits) / totalSegments;
  const double top5Support =
      static_cast<double>(partial->top5_hits) / totalSegments;
  const double segmentSupport =
      static_cast<double>(partial->segment_count) / totalSegments;
  const double centerZ =
      std::abs(partial->local_exact_bpm - priorCenterBpm) / priorSpreadBpm;
  return std::abs(partial->local_exact_bpm - partial->coarse_bpm) <=
             kMirPolicyPartialBarMaxExactCoarseDelta &&
         rank1Support >= kMirPolicyPartialBarMinRank1Support &&
         top5Support >= kMirPolicyPartialBarMinTop5Support &&
         segmentSupport >= kMirPolicyPartialBarMinSegmentSupport &&
         centerZ <= kMirPolicyPartialBarMaxCenterZ &&
         partial->runner_up_score_ratio <=
             kMirPolicyPartialBarMaxRunnerRatio &&
         partial->local_exact_score >=
             kMirPolicyPartialBarMinLocalExactScore;
}

[[nodiscard]] bool mir_policy_partial_bar_exact_half_recovery(
    const smart_tempo::HodgkinsonPartialBarAggregateShadow* partial,
    double currentBpm) noexcept {
  if (partial == nullptr || !partial->enabled || !partial->candidate ||
      currentBpm < kMirPolicyPartialBarExactHalfMinCurrentBpm ||
      partial->total_segment_count == 0 ||
      !std::isfinite(partial->coarse_bpm) ||
      !std::isfinite(partial->local_exact_bpm) ||
      !std::isfinite(partial->local_exact_score) ||
      !std::isfinite(partial->runner_up_score_ratio)) {
    return false;
  }
  const double totalSegments =
      static_cast<double>(partial->total_segment_count);
  const double rank1Support =
      static_cast<double>(partial->rank1_hits) / totalSegments;
  const double top5Support =
      static_cast<double>(partial->top5_hits) / totalSegments;
  const double segmentSupport =
      static_cast<double>(partial->segment_count) / totalSegments;
  const double halfTolerance = (std::max)(0.75, currentBpm * 0.01);
  return std::abs(partial->local_exact_bpm - partial->coarse_bpm) <=
             kMirPolicyPartialBarMaxExactCoarseDelta &&
         std::abs(partial->local_exact_bpm - currentBpm * 0.5) <=
             halfTolerance &&
         rank1Support >= kMirPolicyPartialBarMinRank1Support &&
         top5Support >= kMirPolicyPartialBarMinTop5Support &&
         segmentSupport >= kMirPolicyPartialBarMinSegmentSupport &&
         partial->runner_up_score_ratio <=
             kMirPolicyPartialBarExactHalfMaxRunnerRatio &&
         partial->local_exact_score >=
             kMirPolicyPartialBarExactHalfMinLocalExactScore;
}

[[nodiscard]] bool mir_policy_partial_bar_dominant_octave_release(
    const smart_tempo::HodgkinsonPartialBarAggregateShadow* partial,
    const MirPolicyMeasuredCandidate& measured, double priorCenterBpm,
    double priorSpreadBpm, bool routeMatched, bool genericRoute) noexcept {
  if (!mir_policy_partial_bar_family_conflict_release(
          partial, priorCenterBpm, priorSpreadBpm, routeMatched,
          genericRoute) ||
      !(measured.bpm > 0.0) ||
      !mir_has_alias(measured.candidate.alias_bits, MirAlias::half)) {
    return false;
  }
  return MirMeasuredPulseFamilyMatrix::matches(
             partial->local_exact_bpm, measured.bpm,
             MirPulseRelation::double_time,
             kMirPolicyPartialBarDominantOctaveRelationTolerance) &&
         mir_policy_safe_ratio(partial->local_exact_score,
                               measured.local_exact_score) >=
             kMirPolicyPartialBarDominantOctaveMinLocalScoreRatio;
}

[[nodiscard]] bool mir_policy_broad_route_conflict_review_hold(
    const MirPolicyMeasuredCandidate& measured,
    const MirPolicyMeasuredCandidate& currentCandidate,
    double currentBpm,
    double measuredBaseMargin,
    bool routeMatched,
    bool genericRoute) noexcept {
  // Generic routing is intentionally a no-prior fallback. It must retain the
  // same family-conflict safety hold as an unmatched route; only a calibrated
  // specific route may suppress this broad ambiguity guard.
  if ((routeMatched && !genericRoute) || !(currentBpm > 0.0) ||
      !std::isfinite(currentBpm) || measuredBaseMargin < 0.0 ||
      measuredBaseMargin > 0.10) {
    return false;
  }

  const double bpmDelta = std::abs(measured.bpm - currentBpm);
  const double localRatio = mir_policy_safe_ratio(
      measured.local_exact_score, currentCandidate.local_exact_score);
  const double supportRatio = mir_policy_safe_ratio(
      measured.candidate.support, currentCandidate.candidate.support);
  const bool remoteFamilyAliases =
      mir_has_alias(measured.candidate.alias_bits, MirAlias::two_thirds) &&
      mir_has_alias(measured.candidate.alias_bits, MirAlias::three_halves);
  const bool currentFamilyAliases =
      mir_has_alias(currentCandidate.candidate.alias_bits, MirAlias::direct) &&
      (mir_has_alias(currentCandidate.candidate.alias_bits,
                     MirAlias::three_quarters) ||
       mir_has_alias(currentCandidate.candidate.alias_bits,
                     MirAlias::four_thirds));

  // A broad route provides no trustworthy center tie-break. If its selected
  // candidate has weak segment support while a distant harmonic family is
  // much stronger but still ambiguous, writing either family would be a guess.
  return currentCandidate.candidate.support <= 0.15 &&
         measured.candidate.support >= 0.50 && bpmDelta >= 15.0 &&
         localRatio >= 1.20 && supportRatio >= 3.0 &&
         remoteFamilyAliases && currentFamilyAliases;
}

[[nodiscard]] const MirPolicyMeasuredCandidate*
select_mir_policy_low_pulse_four_thirds_fullboard_release(
    std::span<const MirPolicyMeasuredCandidate> candidates,
    const smart_tempo::MirPrimarySelection& current,
    double measuredBaseMargin) noexcept {
  if (candidates.empty() || !(current.selected_bpm > 0.0) ||
      !current.review_hold || !current.low_pulse_exactness_review_hold) {
    return nullptr;
  }
  const auto& measured = candidates.front();
  const double localRatio = mir_policy_safe_ratio(
      measured.local_exact_score, current.selected_local_exact_score);
  const double supportRatio = mir_policy_safe_ratio(
      measured.candidate.support, current.support);
  if (!mir_pulse_ratio_in_range(measured.bpm, current.selected_bpm, 1.30,
                                1.35) ||
      !mir_has_alias(measured.candidate.alias_bits, MirAlias::direct) ||
      !mir_has_alias(measured.candidate.alias_bits,
                     MirAlias::three_quarters) ||
      measured.candidate.support < 0.80 ||
      measured.local_exact_score < 0.28 || measuredBaseMargin < 0.35 ||
      measured.candidate.continuous_score < 0.28 ||
      measured.candidate.continuous_section_support < 0.80 ||
      measured.candidate.continuous_score_ratio < 1.75 ||
      (localRatio < 1.50 && supportRatio < 1.20)) {
    return nullptr;
  }
  return &measured;
}

[[nodiscard]] const MirPolicyMeasuredCandidate*
select_mir_policy_centered_continuous_harmonic_release(
    std::span<const MirPolicyMeasuredCandidate> candidates,
    double currentBpm,
    double priorCenterBpm,
    double priorSpreadBpm,
    bool routeMatched,
    bool genericRoute) noexcept {
  if (candidates.empty() || !(currentBpm > 0.0) || !routeMatched ||
      genericRoute || !(priorCenterBpm > 0.0) ||
      !(priorSpreadBpm > 0.0)) {
    return nullptr;
  }
  const MirPolicyMeasuredCandidate* current =
      find_mir_policy_output_candidate(candidates, currentBpm);
  if (current == nullptr ||
      current->candidate.continuous_section_support > 0.10) {
    return nullptr;
  }

  std::array<const MirPolicyMeasuredCandidate*, 2> unique{};
  std::size_t uniqueCount = 0;
  for (const auto& candidate : candidates) {
    const bool duplicate = std::any_of(
        unique.begin(), unique.begin() + uniqueCount,
        [&candidate](const MirPolicyMeasuredCandidate* existing) {
          return std::abs(candidate.bpm - existing->bpm) <= 0.01;
        });
    if (duplicate) {
      continue;
    }
    unique[uniqueCount++] = &candidate;
    if (uniqueCount == unique.size()) {
      break;
    }
  }

  const MirPolicyMeasuredCandidate* selected = nullptr;
  for (std::size_t index = 0; index < uniqueCount; ++index) {
    const auto& candidate = *unique[index];
    const double currentDelta = std::abs(candidate.bpm - currentBpm);
    const double centerDistance =
        std::abs(candidate.bpm - priorCenterBpm) / priorSpreadBpm;
    if (!mir_has_alias(candidate.candidate.alias_bits,
                       MirAlias::three_halves) ||
        currentDelta < 2.0 || currentDelta > 10.0 ||
        centerDistance > 0.25 || candidate.candidate.support < 0.75 ||
        candidate.local_exact_score < 0.24 ||
        candidate.candidate.continuous_score < 0.24 ||
        candidate.candidate.continuous_section_support < 0.75 ||
        candidate.candidate.continuous_score_ratio < 1.50 ||
        mir_policy_safe_ratio(candidate.base_score, current->base_score) <
            1.50 ||
        mir_policy_safe_ratio(candidate.local_exact_score,
                              current->local_exact_score) < 1.50) {
      continue;
    }
    if (selected == nullptr ||
        std::make_tuple(candidate.candidate.continuous_score,
                        candidate.candidate.continuous_section_support,
                        candidate.local_exact_score, candidate.base_score,
                        -std::abs(candidate.bpm - priorCenterBpm),
                        -candidate.bpm) >
            std::make_tuple(
                selected->candidate.continuous_score,
                selected->candidate.continuous_section_support,
                selected->local_exact_score, selected->base_score,
                -std::abs(selected->bpm - priorCenterBpm),
                -selected->bpm)) {
      selected = &candidate;
    }
  }
  return selected;
}

struct MirPolicyQualificationStage {
  smart_tempo::MirPolicyDecision decision;
  const MirPolicyMeasuredCandidate* measured = nullptr;
  bool ready = false;
};

void select_mir_policy_candidate(
    smart_tempo::MirPolicyDecision& shadow,
    const MirPolicyMeasuredCandidate& selected) noexcept {
  shadow.candidate_bpm = selected.bpm;
  shadow.candidate_cluster_bpm = selected.cluster_bpm;
  shadow.candidate_base_score = selected.base_score;
  shadow.candidate_support = selected.candidate.support;
  shadow.candidate_winner_support = selected.candidate.winner_support;
  shadow.candidate_local_exact_score = selected.local_exact_score;
  shadow.candidate_pulse_score = selected.candidate.pulse_score;
  shadow.candidate_pulse_section_support =
      selected.candidate.pulse_section_support;
  shadow.candidate_continuous_score = selected.candidate.continuous_score;
  shadow.candidate_continuous_section_support =
      selected.candidate.continuous_section_support;
  shadow.candidate_continuous_section_stability =
      selected.candidate.continuous_section_stability;
  shadow.candidate_continuous_score_ratio =
      selected.candidate.continuous_score_ratio;
  shadow.candidate_current_delta = selected.bpm - shadow.current_bpm;
  shadow.candidate_local_score_ratio = mir_policy_safe_ratio(
      selected.local_exact_score, shadow.current_local_exact_score);
  shadow.candidate_support_ratio = mir_policy_safe_ratio(
      selected.candidate.support, shadow.current_support);
  shadow.candidate_alias_classes = selected.alias_classes;
}

void set_mir_policy_candidate_resolution(
    smart_tempo::MirPolicyDecision& shadow,
    const MirPolicyMeasuredCandidate& selected, bool writerOverride,
    const char* action, const char* decisionClass,
    const char* reason) noexcept {
  select_mir_policy_candidate(shadow, selected);
  shadow.output_bpm = selected.bpm;
  shadow.writer_override_candidate = writerOverride;
  shadow.family_conflict_review_hold_candidate = false;
  shadow.action = action;
  shadow.decision_class = decisionClass;
  shadow.reason = reason;
}

[[nodiscard]] MirPolicyQualificationStage qualify_mir_policy_board(
    std::span<const MirPolicyMeasuredCandidate> candidates,
    const smart_tempo::MirPrimarySelection& current,
    double priorCenterBpm, double priorSpreadBpm, bool routeMatched,
    bool genericRoute, bool enabled) {
  MirPolicyQualificationStage stage;
  auto& shadow = stage.decision;
  shadow.enabled = enabled;
  shadow.current_auto_candidate =
      current.auto_candidate && !current.review_hold;
  shadow.current_review_hold = current.review_hold;
  shadow.soft_center_conflict_review_hold =
      current.soft_center_conflict_review_hold;
  shadow.soft_center_alias_review_hold =
      current.soft_center_alias_review_hold;
  shadow.low_pulse_exactness_review_hold =
      current.low_pulse_exactness_review_hold;
  shadow.sparse_acapella_review_hold =
      current.sparse_acapella_review_hold;
  shadow.protected_review_hold =
      current.low_pulse_exactness_review_hold ||
      current.sparse_acapella_review_hold ||
      current.material_risk_review_hold ||
      current.high_family_conflict_review_hold;
  shadow.current_bpm = current.selected_bpm;
  shadow.output_bpm = current.selected_bpm;
  shadow.current_local_exact_score = current.selected_local_exact_score;
  shadow.current_support = current.support;
  shadow.prior_center_bpm = priorCenterBpm;
  shadow.prior_spread_bpm = priorSpreadBpm;
  if (priorCenterBpm > 0.0 && priorSpreadBpm > 0.0) {
    shadow.prior_min_bpm = priorCenterBpm - priorSpreadBpm;
    shadow.prior_max_bpm = priorCenterBpm + priorSpreadBpm;
  }
  shadow.route_matched = routeMatched;
  shadow.generic_route = genericRoute;
  if (!enabled) {
    shadow.decision_class = "disabled";
    shadow.reason = "disabled";
    return stage;
  }

  shadow.candidate_count = candidates.size();
  shadow.source_candidate = !candidates.empty();
  if (candidates.empty()) {
    shadow.output_would_write = shadow.current_auto_candidate;
    shadow.output_would_review_hold = shadow.current_review_hold;
    shadow.decision_class = "no_candidate";
    shadow.reason = "empty_measured_fullboard";
    return stage;
  }

  stage.measured = &candidates.front();
  const auto& measured = *stage.measured;
  shadow.measured_winner_bpm = measured.bpm;
  shadow.measured_winner_cluster_bpm = measured.cluster_bpm;
  shadow.measured_winner_base_score = measured.base_score;
  shadow.measured_runner_up_base_score =
      candidates.size() > 1 ? candidates[1].base_score : 0.0;
  shadow.measured_base_margin =
      measured.base_score - shadow.measured_runner_up_base_score;
  shadow.measured_winner_support = measured.candidate.support;
  shadow.measured_winner_winner_support =
      measured.candidate.winner_support;
  shadow.measured_winner_local_exact_score = measured.local_exact_score;
  shadow.measured_winner_alias_classes = measured.alias_classes;
  stage.ready = true;
  return stage;
}

[[nodiscard]] bool try_apply_mir_policy_current_release_stage(
    smart_tempo::MirPolicyDecision& shadow,
    std::span<const MirPolicyMeasuredCandidate> candidates,
    MirPolicyReleaseFeatureCache& releaseFeatureCache,
    const smart_tempo::MirPrimarySelection& current,
    const smart_tempo::HodgkinsonTatumProbeResult& primaryFamily,
    const smart_tempo::HodgkinsonPartialBarAggregateShadow*
        partialBarAggregate,
    const MirPolicyMeasuredCandidate& measured, double priorCenterBpm,
    double priorSpreadBpm, bool routeMatched, bool genericRoute) {
  // Recover an in-rail measured winner when the primary selector's otherwise
  // writable result falls below the production domain. This is independent of
  // metadata and only admits a strongly supported FullBoard measurement.
  const bool outOfRailPrimaryWitnessRelease =
      shadow.current_auto_candidate && shadow.current_bpm > 0.0 &&
      shadow.current_bpm < kMirProductionMinBpm &&
      measured.bpm >= kMirProductionMinBpm &&
      measured.bpm <= kMirProductionMaxBpm &&
      measured.candidate.support >= kMirPolicyOutOfRailWitnessMinSupport &&
      measured.local_exact_score >=
          kMirPolicyOutOfRailWitnessMinLocalExactScore;
  if (outOfRailPrimaryWitnessRelease) {
    select_mir_policy_candidate(shadow, measured);
    shadow.output_bpm = measured.bpm;
    shadow.writer_override_candidate = true;
    shadow.output_would_write = true;
    shadow.output_would_review_hold = false;
    shadow.action = "writer_override";
    shadow.decision_class = "measured_recovery_candidate";
    shadow.reason = "measured_out_of_rail_primary_witness_release";
    return true;
  }

  // A decisive direct full-board measurement is independent of whether the
  // earlier primary selector classified its anchor as writable or review-only.
  // Keeping this before the branch prevents a broad safety classifier from
  // becoming an accidental prerequisite for recovering the measured pulse.
  const MirPolicyMeasuredCandidate* lowPulseFourThirdsRelease =
      select_mir_policy_low_pulse_four_thirds_fullboard_release(
          candidates, current, shadow.measured_base_margin);
  if (lowPulseFourThirdsRelease != nullptr) {
    select_mir_policy_candidate(shadow, *lowPulseFourThirdsRelease);
    shadow.output_bpm = lowPulseFourThirdsRelease->bpm;
    shadow.writer_override_candidate = true;
    shadow.output_would_write = true;
    shadow.output_would_review_hold = false;
    shadow.action = "writer_override";
    shadow.decision_class = "measured_recovery_candidate";
    shadow.reason = "measured_low_pulse_four_thirds_fullboard_release";
    return true;
  }

  const double dominantDirectDelta =
      std::abs(measured.bpm - shadow.current_bpm);
  const double dominantDirectLocalRatio = mir_policy_safe_ratio(
      measured.local_exact_score, shadow.current_local_exact_score);
  const double dominantDirectSupportRatio = mir_policy_safe_ratio(
      measured.candidate.support, shadow.current_support);
  const double primaryFamilyTopBpm =
      primaryFamily.family_rank_count > 0
          ? primaryFamily.family_rank_bpm[0]
          : primaryFamily.family_bpm;
  const double primaryFamilyTopScore =
      primaryFamily.family_rank_count > 0
          ? primaryFamily.family_rank_score[0]
          : primaryFamily.family_score;
  const double primaryFamilyRunnerScore =
      primaryFamily.family_rank_count > 1
          ? primaryFamily.family_rank_score[1]
          : primaryFamily.runner_up_family_score;
  const std::size_t primaryFamilyFitSegments =
      primaryFamily.fit_segment_count > 0 ? primaryFamily.fit_segment_count
                                          : primaryFamily.segment_count;
  const double primaryFamilyDominance =
      primaryFamilyRunnerScore > 0.0
          ? primaryFamilyTopScore / primaryFamilyRunnerScore
          : primaryFamilyTopScore;
  const double primaryFamilyConsensus =
      primaryFamilyFitSegments > 0
          ? static_cast<double>(primaryFamily.family_rank_support[0]) /
                static_cast<double>(primaryFamilyFitSegments)
          : 0.0;
  const double dominantDirectLowBpm =
      (std::min)(measured.bpm, shadow.current_bpm);
  const double dominantDirectHighBpm =
      (std::max)(measured.bpm, shadow.current_bpm);
  const bool primaryFamilyConfirmsMeasuredThreeHalves =
      primaryFamily.candidate && primaryFamily.family_rank_count > 0 &&
      std::abs(primaryFamilyTopBpm - measured.bpm) <= 2.0 &&
      primaryFamilyDominance >= 2.0 && primaryFamilyConsensus >= 0.50 &&
      MirMeasuredPulseFamilyMatrix::matches(
          dominantDirectHighBpm, dominantDirectLowBpm,
          MirPulseRelation::three_halves, 0.03);
  const bool partialBarConfirmsMeasured =
      genericRoute && partialBarAggregate != nullptr &&
      partialBarAggregate->candidate &&
      partialBarAggregate->total_segment_count > 0 &&
      std::abs(partialBarAggregate->local_exact_bpm - measured.bpm) <= 2.0 &&
      static_cast<double>(partialBarAggregate->top5_hits) /
              static_cast<double>(partialBarAggregate->total_segment_count) >=
          0.98;
  const bool partialBarRunnerConfirmsMeasured =
      genericRoute && partialBarAggregate != nullptr &&
      partialBarAggregate->candidate &&
      partialBarAggregate->total_segment_count > 0 &&
      std::abs(partialBarAggregate->runner_up_bpm - measured.bpm) <=
          kMirPolicyGenericPartialRunnerMaxMeasuredDeltaBpm &&
      partialBarAggregate->runner_up_score_ratio >=
          kMirPolicyGenericPartialRunnerMinScoreRatio &&
      measured.candidate.support >=
          kMirPolicyGenericPartialRunnerMinMeasuredSupport &&
      measured.local_exact_score >=
          kMirPolicyGenericPartialRunnerMinLocalExactScore &&
      primaryFamilyDominance >=
          kMirPolicyGenericPartialRunnerMinFamilyDominance &&
      primaryFamilyConsensus >=
          kMirPolicyGenericPartialRunnerMinFamilyConsensus;
  const bool dominantDirectFullBoardRelease =
      (shadow.current_bpm > 0.0 && dominantDirectDelta > 1.0 &&
       mir_has_alias(measured.candidate.alias_bits, MirAlias::direct) &&
       measured.candidate.support >= 0.90 &&
       measured.local_exact_score >= 0.35 &&
       shadow.measured_base_margin >= 0.35 &&
       (dominantDirectLocalRatio >= 1.20 ||
        dominantDirectSupportRatio >= 1.20)) ||
      (shadow.current_bpm > 0.0 && dominantDirectDelta > 15.0 &&
       mir_has_alias(measured.candidate.alias_bits, MirAlias::direct) &&
       measured.candidate.support >= 0.90 &&
       measured.local_exact_score >= 0.25 &&
       shadow.measured_base_margin >= 0.20 &&
       primaryFamilyConfirmsMeasuredThreeHalves &&
       (!genericRoute || measured.bpm > shadow.current_bpm ||
        partialBarRunnerConfirmsMeasured)) ||
      (shadow.current_bpm > 0.0 && dominantDirectDelta > 15.0 &&
       mir_has_alias(measured.candidate.alias_bits, MirAlias::direct) &&
       measured.candidate.support >= 0.99 &&
       measured.local_exact_score >= 0.34 &&
       shadow.measured_base_margin >= 0.35 &&
       dominantDirectLocalRatio >= 1.50 && partialBarConfirmsMeasured);
  if (dominantDirectFullBoardRelease) {
    select_mir_policy_candidate(shadow, measured);
    shadow.output_bpm = measured.bpm;
    shadow.writer_override_candidate = true;
    shadow.output_would_write = true;
    shadow.output_would_review_hold = false;
    shadow.action = "writer_override";
    shadow.decision_class = "measured_recovery_candidate";
    shadow.reason = "measured_dominant_direct_fullboard_release";
    return true;
  }

  const bool unresolvedGenericThreeHalvesFamilyConflict =
      shadow.current_auto_candidate && shadow.current_bpm > 0.0 &&
      genericRoute && measured.bpm < shadow.current_bpm &&
      dominantDirectDelta > 15.0 &&
      mir_has_alias(measured.candidate.alias_bits, MirAlias::direct) &&
      measured.candidate.support >= 0.90 &&
      measured.local_exact_score >= 0.25 &&
      shadow.measured_base_margin >= 0.20 &&
      primaryFamilyConfirmsMeasuredThreeHalves &&
      !partialBarConfirmsMeasured && !partialBarRunnerConfirmsMeasured;
  if (unresolvedGenericThreeHalvesFamilyConflict) {
    select_mir_policy_candidate(shadow, measured);
    shadow.output_bpm = 0.0;
    shadow.writer_override_candidate = false;
    shadow.family_conflict_review_hold_candidate = true;
    shadow.output_would_write = false;
    shadow.output_would_review_hold = true;
    shadow.action = "family_conflict_review_hold";
    shadow.decision_class = "review_hold_candidate";
    shadow.reason = "generic_three_halves_family_conflict";
    return true;
  }

  if (shadow.current_auto_candidate && shadow.current_bpm > 0.0) {
    const bool priorUsable =
        routeMatched && !genericRoute && shadow.prior_center_bpm > 0.0;
    const MirPolicyMeasuredCandidate* currentCandidate =
        find_mir_policy_output_candidate(candidates, shadow.current_bpm);
    if (currentCandidate != nullptr) {
      double recoveryEvidenceRatio = 0.0;
      const MirPolicyMeasuredCandidate* recovery =
          select_mir_policy_measured_half_pulse_recovery(
              candidates, *currentCandidate, recoveryEvidenceRatio);
      const char* recoveryReason = "measured_half_pulse_recovery";
      if (recovery == nullptr) {
        recovery = select_mir_policy_measured_center_continuous_recovery(
            candidates, *currentCandidate, shadow.prior_center_bpm,
            priorUsable);
        recoveryReason = "measured_center_continuous_recovery";
      }
      if (recovery != nullptr) {
        select_mir_policy_candidate(shadow, *recovery);
        shadow.candidate_recovery_evidence_ratio =
            recoveryEvidenceRatio;
        shadow.output_bpm = recovery->bpm;
        shadow.writer_override_candidate = true;
        shadow.output_would_write = true;
        shadow.output_would_review_hold = false;
        shadow.action = "writer_override";
        shadow.decision_class = "measured_recovery_candidate";
        shadow.reason = recoveryReason;
        return true;
      }
      const MirPolicyMeasuredCandidate* centeredHarmonicRelease =
          select_mir_policy_centered_continuous_harmonic_release(
              candidates, shadow.current_bpm, shadow.prior_center_bpm,
              shadow.prior_spread_bpm, routeMatched, genericRoute);
      if (centeredHarmonicRelease != nullptr) {
        select_mir_policy_candidate(shadow, *centeredHarmonicRelease);
        shadow.output_bpm = centeredHarmonicRelease->bpm;
        shadow.writer_override_candidate = true;
        shadow.output_would_write = true;
        shadow.output_would_review_hold = false;
        shadow.action = "writer_override";
        shadow.decision_class = "measured_recovery_candidate";
        shadow.reason =
            "measured_centered_continuous_harmonic_release";
        return true;
      }
      if (mir_policy_broad_route_conflict_review_hold(
              measured, *currentCandidate, shadow.current_bpm,
              shadow.measured_base_margin, routeMatched, genericRoute)) {
        select_mir_policy_candidate(shadow, measured);
        shadow.broad_route_conflict_review_hold_candidate = true;
        shadow.output_bpm = 0.0;
        shadow.output_would_write = false;
        shadow.output_would_review_hold = true;
        shadow.action = "broad_route_conflict_review_hold";
        shadow.decision_class = "review_hold_candidate";
        shadow.reason = "weak_current_strong_remote_family_conflict";
        return true;
      }
    }

    select_mir_policy_candidate(shadow, measured);
    const double absoluteDelta =
        std::abs(measured.bpm - shadow.current_bpm);
    const double centerDistance =
        priorUsable ? std::abs(measured.bpm - shadow.prior_center_bpm)
                    : std::numeric_limits<double>::infinity();
    const bool frozenWriterOverride =
        absoluteDelta >= 0.10 && absoluteDelta <= 35.0 &&
        measured.candidate.support >= 0.90 &&
        measured.local_exact_score >= 0.16 &&
        shadow.measured_base_margin >= 0.0 &&
        shadow.candidate_local_score_ratio >= 0.80 &&
        shadow.candidate_support_ratio >= 0.80;
    const double normalizedCenterDistance =
        shadow.prior_spread_bpm > 0.0
            ? centerDistance / shadow.prior_spread_bpm
            : std::numeric_limits<double>::infinity();
    const bool centerAnchoredLargeWriterOverride =
        priorUsable && absoluteDelta > 3.0 && absoluteDelta <= 90.0 &&
        mir_policy_within_center_gate(centerDistance, shadow.prior_spread_bpm) &&
        normalizedCenterDistance <= kMirPolicyCenterGateNormalizedLimit &&
        measured.candidate.support >= 0.40 &&
        measured.local_exact_score >= 0.16 &&
        shadow.measured_base_margin >= 0.0 &&
        shadow.candidate_local_score_ratio >= 1.20 &&
        shadow.candidate_support_ratio >= 0.45;
    shadow.writer_override_candidate =
        frozenWriterOverride || centerAnchoredLargeWriterOverride;
    if (shadow.writer_override_candidate) {
      shadow.output_bpm = measured.bpm;
      shadow.action = "writer_override";
      shadow.decision_class = "writer_override_candidate";
      shadow.reason = centerAnchoredLargeWriterOverride
                          ? "center_anchored_writer_override_gate"
                          : "frozen_writer_override_gate";
    } else {
      shadow.action = "keep_current";
      shadow.decision_class = "keep_current_writer";
      shadow.reason = "writer_override_gate_veto";
    }

    const MirPolicyMeasuredCandidate* outputCandidate =
        find_mir_policy_output_candidate(candidates, shadow.output_bpm);
    if (priorUsable && outputCandidate != nullptr &&
        shadow.output_bpm >= 80.0) {
      const double outputDistance =
          std::abs(shadow.output_bpm - shadow.prior_center_bpm);
      if (outputDistance >= 15.0) {
        const MirPolicyMeasuredCandidate* conflict = nullptr;
        for (const auto& candidate : candidates) {
          const double familyDelta =
              std::abs(candidate.bpm - shadow.output_bpm);
          if (familyDelta < 15.0 || candidate.candidate.support < 0.60) {
            continue;
          }
          const double localRatio = mir_policy_safe_ratio(
              candidate.local_exact_score,
              outputCandidate->local_exact_score);
          const double baseRatio = mir_policy_safe_ratio(
              candidate.base_score, outputCandidate->base_score);
          const double improvement =
              outputDistance -
              std::abs(candidate.bpm - shadow.prior_center_bpm);
          if (localRatio < 0.70 || baseRatio < 0.80 ||
              improvement < 10.0) {
            continue;
          }
          if (conflict == nullptr ||
              std::make_tuple(
                  improvement, localRatio, baseRatio,
                  candidate.candidate.support, -candidate.bpm) >
                  std::make_tuple(
                      shadow.family_conflict_center_improvement,
                      shadow.family_conflict_local_score_ratio,
                      shadow.family_conflict_base_score_ratio,
                      conflict->candidate.support, -conflict->bpm)) {
            conflict = &candidate;
            shadow.family_conflict_center_improvement = improvement;
            shadow.family_conflict_local_score_ratio = localRatio;
            shadow.family_conflict_base_score_ratio = baseRatio;
          }
        }
        if (conflict != nullptr) {
          shadow.family_conflict_alternative_bpm = conflict->bpm;
          shadow.family_conflict_alias_classes = conflict->alias_classes;
          shadow.family_conflict_support = conflict->candidate.support;
          shadow.family_conflict_winner_support =
              conflict->candidate.winner_support;
          shadow.family_conflict_local_exact_score =
              conflict->local_exact_score;
          shadow.family_conflict_pulse_score =
              conflict->candidate.pulse_score;
          shadow.family_conflict_pulse_section_support =
              conflict->candidate.pulse_section_support;
          shadow.family_conflict_continuous_score =
              conflict->candidate.continuous_score;
          shadow.family_conflict_continuous_section_support =
              conflict->candidate.continuous_section_support;
          shadow.family_conflict_continuous_score_ratio =
              conflict->candidate.continuous_score_ratio;
          const double conflictCenterDistance =
              std::abs(conflict->bpm - shadow.prior_center_bpm);
          const double conflictCurrentDelta =
              std::abs(conflict->bpm - shadow.current_bpm);
          const bool closeCenterConflict = conflictCenterDistance <= 7.0;
          const bool narrowLargeFamilyCorrection =
              conflictCenterDistance <= 8.1 && conflictCurrentDelta >= 30.0;
          const bool segmentWinnerConflictEvidence =
              conflict->candidate.winner_support >= 0.60 &&
              conflict->local_exact_score >= 0.18 &&
              shadow.family_conflict_local_score_ratio >= 1.00 &&
              shadow.family_conflict_base_score_ratio >= 0.95;
          const bool continuousConflictEvidence =
              conflict->candidate.continuous_score >= 0.26 &&
              conflict->candidate.continuous_section_support >= 0.95 &&
              conflict->candidate.continuous_score_ratio >= 1.70 &&
              shadow.family_conflict_local_score_ratio >= 0.90 &&
              shadow.family_conflict_base_score_ratio >= 0.90;
          const bool centerCloseConflictRelease =
              (closeCenterConflict || narrowLargeFamilyCorrection) &&
              shadow.family_conflict_center_improvement >= 15.0 &&
              shadow.family_conflict_local_score_ratio >= 0.80 &&
              shadow.family_conflict_base_score_ratio >= 0.80 &&
              (segmentWinnerConflictEvidence || continuousConflictEvidence);
          const bool dominantMeasuredConflictRelease =
              std::abs(conflict->bpm - measured.bpm) <= 0.10 &&
              measured.candidate.support >= 0.90 &&
              shadow.candidate_local_score_ratio >= 2.0 &&
              shadow.family_conflict_local_score_ratio >= 2.0 &&
              shadow.family_conflict_base_score_ratio >= 1.10 &&
              shadow.family_conflict_center_improvement >= 15.0;
          const bool measuredEvidenceDominatesCenterConflict =
              std::abs(outputCandidate->bpm - measured.bpm) <= 0.10 &&
              std::abs(conflict->bpm - shadow.current_bpm) <= 1.0 &&
              mir_has_alias(measured.candidate.alias_bits,
                                MirAlias::direct) &&
              measured.candidate.support >= 0.90 &&
              measured.local_exact_score >= 0.35 &&
              shadow.candidate_local_score_ratio >= 1.30 &&
              measured.candidate.continuous_score >= 0.35 &&
              measured.candidate.continuous_section_support >= 0.90 &&
              measured.candidate.continuous_score_ratio >= 1.50 &&
              shadow.family_conflict_local_score_ratio <= 0.78 &&
              shadow.family_conflict_base_score_ratio <= 0.90 &&
              shadow.family_conflict_center_improvement >= 15.0;
          const bool retainStrongMeasuredCurrent =
              currentCandidate != nullptr &&
              std::abs(currentCandidate->bpm - shadow.current_bpm) <= 0.10 &&
              std::abs(conflict->bpm - shadow.current_bpm) <= 0.10 &&
              currentCandidate->candidate.support >= 0.90 &&
              currentCandidate->candidate.winner_support >= 0.10 &&
              currentCandidate->local_exact_score >= 0.24 &&
              currentCandidate->candidate.continuous_score >= 0.24 &&
              currentCandidate->candidate.continuous_section_support >=
                  0.75 &&
              currentCandidate->candidate.continuous_score_ratio >= 1.00;
          const bool lowerHarmonicFamilyConflictRelease =
              mir_policy_family_conflict_lower_harmonic_release(
                  measured, *conflict,
                  shadow.family_conflict_local_score_ratio,
                  shadow.family_conflict_base_score_ratio,
                  shadow.family_conflict_center_improvement,
                  shadow.prior_center_bpm, shadow.prior_spread_bpm,
                  priorUsable);
          const bool measuredCandidateFamilyConflictRelease =
              mir_policy_family_conflict_measured_candidate_release(
                  measured, *conflict, shadow.current_bpm,
                  shadow.family_conflict_local_score_ratio);
          const bool downwardMeasuredFamilyConflictRelease =
              mir_policy_family_conflict_downward_measured_release(
                  measured, shadow.current_bpm,
                  shadow.family_conflict_local_score_ratio);
          const bool alternativeFamilyConflictRelease =
              mir_policy_family_conflict_alternative_release(
                  measured, *conflict, shadow.measured_base_margin,
                  shadow.family_conflict_base_score_ratio);
          const bool strongContinuousAlternativeFamilyConflictRelease =
              mir_policy_family_conflict_strong_continuous_alternative_release(
                  measured, *conflict, shadow.measured_base_margin,
                  shadow.family_conflict_base_score_ratio);
          const bool strongPulseAlternativeFamilyConflictRelease =
              mir_policy_family_conflict_strong_pulse_alternative_release(
                  *conflict, shadow.measured_base_margin,
                  shadow.family_conflict_center_improvement);
          const bool directHighSupportAlternativeFamilyConflictRelease =
              mir_policy_family_conflict_direct_high_support_alternative_release(
                  measured, *conflict,
                  shadow.family_conflict_local_score_ratio,
                  shadow.family_conflict_base_score_ratio,
                  shadow.family_conflict_center_improvement);
          const bool residualMarginFamilyConflictRelease =
              mir_policy_family_conflict_residual_margin_release(
                  measured, *conflict, shadow.measured_base_margin,
                  priorCenterBpm, priorSpreadBpm, routeMatched,
                  genericRoute);
          const bool residualTwoThirdsFamilyConflictRelease =
              mir_policy_family_conflict_residual_two_thirds_release(
                  measured, shadow.current_bpm,
                  shadow.family_conflict_local_score_ratio, priorCenterBpm,
                  priorSpreadBpm, routeMatched, genericRoute);
          const bool strongCenteredMeasuredWinnerFamilyConflictRelease =
              mir_policy_family_conflict_strong_centered_measured_winner_release(
                  measured, shadow.measured_base_margin, priorCenterBpm,
                  priorSpreadBpm, routeMatched, genericRoute);
          const bool normalizedMeasuredWinnerFamilyConflictRelease =
              mir_policy_family_conflict_normalized_measured_winner_release(
                  measured, *conflict, shadow.candidate_local_score_ratio,
                  priorCenterBpm, priorSpreadBpm, routeMatched, genericRoute);
          if (measuredEvidenceDominatesCenterConflict) {
            set_mir_policy_candidate_resolution(
                shadow, measured, true, "writer_override",
                "writer_override_candidate",
                "measured_evidence_dominates_center_conflict_gate");
          } else if (retainStrongMeasuredCurrent) {
            set_mir_policy_candidate_resolution(
                shadow, *currentCandidate, false, "keep_current",
                "keep_current_writer",
                "measured_current_family_conflict_retention_gate");
          } else if (centerCloseConflictRelease ||
              dominantMeasuredConflictRelease) {
            set_mir_policy_candidate_resolution(
                shadow, *conflict, true, "writer_override",
                "writer_override_candidate",
                centerCloseConflictRelease
                    ? "measured_family_conflict_evidence_release_gate"
                    : "dominant_measured_family_conflict_release_gate");
          } else if (lowerHarmonicFamilyConflictRelease) {
            set_mir_policy_candidate_resolution(
                shadow, *conflict, true, "writer_override",
                "writer_override_candidate",
                "measured_lower_harmonic_family_conflict_release_gate");
          } else if (measuredCandidateFamilyConflictRelease) {
            set_mir_policy_candidate_resolution(
                shadow, measured, true, "writer_override",
                "writer_override_candidate",
                "measured_candidate_family_conflict_release_gate");
          } else if (downwardMeasuredFamilyConflictRelease) {
            set_mir_policy_candidate_resolution(
                shadow, measured, true, "writer_override",
                "writer_override_candidate",
                "measured_downward_family_conflict_release_gate");
          } else if (alternativeFamilyConflictRelease) {
            set_mir_policy_candidate_resolution(
                shadow, *conflict, true, "writer_override",
                "writer_override_candidate",
                "measured_alternative_family_conflict_release_gate");
          } else if (strongContinuousAlternativeFamilyConflictRelease) {
            set_mir_policy_candidate_resolution(
                shadow, *conflict, true, "writer_override",
                "writer_override_candidate",
                "measured_strong_continuous_alternative_family_conflict_release_gate");
          } else if (strongPulseAlternativeFamilyConflictRelease) {
            set_mir_policy_candidate_resolution(
                shadow, *conflict, true, "writer_override",
                "writer_override_candidate",
                "measured_strong_pulse_alternative_family_conflict_release_gate");
          } else if (directHighSupportAlternativeFamilyConflictRelease) {
            set_mir_policy_candidate_resolution(
                shadow, *conflict, true, "writer_override",
                "writer_override_candidate",
                "measured_direct_high_support_alternative_family_conflict_release_gate");
          } else if (const auto* recovery =
                         select_mir_policy_dominant_segment_family_recovery(
                             candidates, shadow.current_bpm, primaryFamily);
                     recovery != nullptr) {
            set_mir_policy_candidate_resolution(
                shadow, *recovery, true, "writer_override",
                "writer_override_candidate",
                "measured_dominant_segment_family_recovery_gate");
          } else if (residualMarginFamilyConflictRelease) {
            set_mir_policy_candidate_resolution(
                shadow, measured, true, "writer_override",
                "writer_override_candidate",
                "measured_residual_margin_family_conflict_release_gate");
          } else if (residualTwoThirdsFamilyConflictRelease) {
            set_mir_policy_candidate_resolution(
                shadow, measured, true, "writer_override",
                "writer_override_candidate",
                "measured_residual_two_thirds_family_conflict_release_gate");
          } else if (strongCenteredMeasuredWinnerFamilyConflictRelease) {
            set_mir_policy_candidate_resolution(
                shadow, measured, true, "writer_override",
                "writer_override_candidate",
                "measured_strong_centered_winner_family_conflict_release_gate");
          } else if (normalizedMeasuredWinnerFamilyConflictRelease) {
            set_mir_policy_candidate_resolution(
                shadow, measured, true, "writer_override",
                "writer_override_candidate",
                "measured_normalized_winner_family_conflict_release_gate");
          } else if (const auto* candidate =
                         select_mir_policy_family_conflict_center_exact_release(
                             candidates, *outputCandidate, priorCenterBpm,
                             priorSpreadBpm, routeMatched, genericRoute);
                     candidate != nullptr) {
            set_mir_policy_candidate_resolution(
                shadow, *candidate, true, "writer_override",
                "writer_override_candidate",
                "measured_center_exact_family_conflict_release_gate");
          } else if (const auto* candidate =
                         select_mir_policy_family_conflict_center_challenger(
                             candidates, priorCenterBpm, priorSpreadBpm,
                             routeMatched, genericRoute);
                     candidate != nullptr) {
            set_mir_policy_candidate_resolution(
                shadow, *candidate, true, "writer_override",
                "writer_override_candidate",
                "measured_center_challenger_family_conflict_release_gate");
          } else if (const auto* candidate =
                         select_mir_policy_release_ready_scored(
                             releaseFeatureCache, priorCenterBpm,
                             priorSpreadBpm, 2, 1.0, shadow.current_bpm,
                             30.0);
                     candidate != nullptr) {
            set_mir_policy_candidate_resolution(
                shadow, *candidate, true, "writer_override",
                "writer_override_candidate",
                "measured_release_ready_center_family_conflict_gate");
          } else if (const auto* candidate =
                         select_mir_policy_release_ready_conflict_anchor(
                             releaseFeatureCache, priorCenterBpm);
                     candidate != nullptr) {
            set_mir_policy_candidate_resolution(
                shadow, *candidate, true, "writer_override",
                "writer_override_candidate",
                "measured_release_ready_anchor_family_conflict_gate");
          } else if (mir_policy_partial_bar_family_conflict_release(
                         partialBarAggregate, priorCenterBpm,
                         priorSpreadBpm, routeMatched, genericRoute)) {
            shadow.candidate_bpm = partialBarAggregate->local_exact_bpm;
            shadow.candidate_cluster_bpm = partialBarAggregate->coarse_bpm;
            shadow.candidate_base_score = partialBarAggregate->score_sum;
            shadow.candidate_support =
                static_cast<double>(partialBarAggregate->rank1_hits) /
                static_cast<double>(
                    partialBarAggregate->total_segment_count);
            shadow.candidate_winner_support =
                static_cast<double>(partialBarAggregate->top5_hits) /
                static_cast<double>(
                    partialBarAggregate->total_segment_count);
            shadow.candidate_local_exact_score =
                partialBarAggregate->local_exact_score;
            shadow.candidate_current_delta =
                partialBarAggregate->local_exact_bpm - shadow.current_bpm;
            shadow.candidate_alias_classes = "partial_bar_direct";
            shadow.output_bpm = partialBarAggregate->local_exact_bpm;
            shadow.writer_override_candidate = true;
            shadow.partial_bar_family_conflict_release_candidate = true;
            shadow.family_conflict_review_hold_candidate = false;
            shadow.action = "writer_override";
            shadow.decision_class = "writer_override_candidate";
            shadow.reason =
                "measured_partial_bar_family_conflict_release";
          } else {
            shadow.writer_override_candidate = false;
            shadow.review_hold_promotion_candidate = false;
            shadow.family_conflict_review_hold_candidate = true;
            shadow.action = "family_conflict_review_hold";
            shadow.decision_class = "review_hold_candidate";
            shadow.reason = "frozen_writer_family_conflict_gate";
          }
        }
      }
    }
    if (std::strcmp(shadow.reason, "writer_override_gate_veto") == 0 &&
        currentCandidate != nullptr) {
      if (mir_policy_partial_bar_dominant_octave_release(
              partialBarAggregate, measured, priorCenterBpm,
              priorSpreadBpm, routeMatched, genericRoute)) {
        shadow.candidate_bpm = partialBarAggregate->local_exact_bpm;
        shadow.candidate_cluster_bpm = partialBarAggregate->coarse_bpm;
        shadow.candidate_base_score = partialBarAggregate->score_sum;
        shadow.candidate_support =
            static_cast<double>(partialBarAggregate->rank1_hits) /
            static_cast<double>(partialBarAggregate->total_segment_count);
        shadow.candidate_winner_support =
            static_cast<double>(partialBarAggregate->top5_hits) /
            static_cast<double>(partialBarAggregate->total_segment_count);
        shadow.candidate_local_exact_score =
            partialBarAggregate->local_exact_score;
        shadow.candidate_current_delta =
            partialBarAggregate->local_exact_bpm - shadow.current_bpm;
        shadow.candidate_local_score_ratio = mir_policy_safe_ratio(
            partialBarAggregate->local_exact_score,
            measured.local_exact_score);
        shadow.candidate_alias_classes = "partial_bar_octave";
        shadow.output_bpm = partialBarAggregate->local_exact_bpm;
        shadow.writer_override_candidate = true;
        shadow.review_hold_promotion_candidate = false;
        shadow.family_conflict_review_hold_candidate = false;
        shadow.action = "writer_override";
        shadow.decision_class = "measured_recovery_candidate";
        shadow.reason = "measured_partial_bar_dominant_octave_release";
      } else {
        const char* residualReason = nullptr;
        if (const MirPolicyMeasuredCandidate* residualRecovery =
                select_mir_policy_residual_writer_recovery(
                    candidates, *currentCandidate, partialBarAggregate,
                    shadow.measured_base_margin,
                    !routeMatched || genericRoute, residualReason)) {
          select_mir_policy_candidate(shadow, *residualRecovery);
          shadow.output_bpm = residualRecovery->bpm;
          shadow.writer_override_candidate = true;
          shadow.review_hold_promotion_candidate = false;
          shadow.family_conflict_review_hold_candidate = false;
          shadow.action = "writer_override";
          shadow.decision_class = "measured_recovery_candidate";
          shadow.reason = residualReason;
        } else if (residualReason != nullptr) {
          select_mir_policy_candidate(shadow, *currentCandidate);
          shadow.output_bpm = currentCandidate->bpm;
          shadow.writer_override_candidate = false;
          shadow.review_hold_promotion_candidate = false;
          shadow.family_conflict_review_hold_candidate = false;
          shadow.action = "keep_current";
          shadow.decision_class = "keep_current_writer";
          shadow.reason = residualReason;
        }
      }
    }
    if (std::strcmp(shadow.reason, "writer_override_gate_veto") == 0 &&
        mir_policy_partial_bar_exact_half_recovery(
            partialBarAggregate, shadow.current_bpm)) {
      shadow.candidate_bpm = partialBarAggregate->local_exact_bpm;
      shadow.candidate_cluster_bpm = partialBarAggregate->coarse_bpm;
      shadow.candidate_base_score = partialBarAggregate->score_sum;
      shadow.candidate_support =
          static_cast<double>(partialBarAggregate->rank1_hits) /
          static_cast<double>(partialBarAggregate->total_segment_count);
      shadow.candidate_winner_support =
          static_cast<double>(partialBarAggregate->top5_hits) /
          static_cast<double>(partialBarAggregate->total_segment_count);
      shadow.candidate_local_exact_score =
          partialBarAggregate->local_exact_score;
      shadow.candidate_current_delta =
          partialBarAggregate->local_exact_bpm - shadow.current_bpm;
      shadow.candidate_alias_classes = "partial_bar_half";
      shadow.output_bpm = partialBarAggregate->local_exact_bpm;
      shadow.writer_override_candidate = true;
      shadow.review_hold_promotion_candidate = false;
      shadow.family_conflict_review_hold_candidate = false;
      shadow.action = "writer_override";
      shadow.decision_class = "measured_recovery_candidate";
      shadow.reason = "measured_partial_bar_exact_half_recovery_gate";
    }
    shadow.output_would_review_hold =
        shadow.family_conflict_review_hold_candidate;
    shadow.output_would_write =
        !shadow.family_conflict_review_hold_candidate;
    return true;
  }

  return false;
}
[[nodiscard]] bool try_apply_mir_policy_hold_resolution_stage(
    smart_tempo::MirPolicyDecision& shadow,
    std::span<const MirPolicyMeasuredCandidate> candidates,
    MirPolicyReleaseFeatureCache& releaseFeatureCache,
    const smart_tempo::MirPrimarySelection& current,
    const smart_tempo::HodgkinsonMaterialRiskLocalExactProbe*
        materialRiskProbe,
    const MirPolicyMeasuredCandidate& measured, double priorCenterBpm,
    double priorSpreadBpm, bool routeMatched, bool genericRoute) {
  // The material-risk detector is intentionally broad. Release its hold when
  // the independent policy full board strongly confirms the same measured pulse;
  // this cannot perform an octave/family switch or synthesize a center value.
  if (current.review_hold && current.material_risk_review_hold &&
      materialRiskProbe != nullptr &&
      is_mir_policy_material_risk_local_exact_release_candidate(
          *materialRiskProbe)) {
    shadow.candidate_bpm = materialRiskProbe->best_bpm;
    shadow.candidate_cluster_bpm = materialRiskProbe->best_anchor_bpm;
    shadow.candidate_base_score = materialRiskProbe->best_score;
    shadow.candidate_local_exact_score = materialRiskProbe->best_score;
    shadow.candidate_current_delta =
        materialRiskProbe->best_bpm - shadow.current_bpm;
    shadow.candidate_local_score_ratio =
        materialRiskProbe->best_score_ratio_to_current;
    shadow.material_risk_probe_score_ratio =
        materialRiskProbe->best_score_ratio_to_current;
    shadow.material_risk_probe_top_span_bpm =
        materialRiskProbe->top_span_bpm;
    shadow.material_risk_probe_target_candidate_count =
        materialRiskProbe->target_family_candidate_count;
    shadow.material_risk_probe_non_boundary_top_count =
        materialRiskProbe->non_boundary_top_count;
    shadow.output_bpm = materialRiskProbe->best_bpm;
    shadow.review_hold_promotion_candidate = true;
    shadow.material_risk_local_exact_release_candidate = true;
    shadow.output_would_write = true;
    shadow.output_would_review_hold = false;
    shadow.action = "review_hold_promotion";
    shadow.decision_class = "review_hold_promotion_candidate";
    shadow.reason = "measured_material_risk_local_exact_release";
    return true;
  }

  if (current.review_hold && shadow.current_bpm > 0.0 &&
      current.material_risk_review_hold &&
      std::abs(measured.bpm - shadow.current_bpm) <= 1.0 &&
      measured.candidate.support >= 0.90 &&
      shadow.current_support >= 0.90 &&
      measured.local_exact_score >= 0.25 &&
      shadow.measured_base_margin >= 0.35) {
    select_mir_policy_candidate(shadow, measured);
    shadow.output_bpm = measured.bpm;
    shadow.writer_override_candidate = true;
    shadow.output_would_write = true;
    shadow.output_would_review_hold = false;
    shadow.action = "writer_override";
    shadow.decision_class = "measured_recovery_candidate";
    shadow.reason = "measured_material_risk_same_pulse_release";
    return true;
  }

  if (current.review_hold && shadow.current_bpm > 0.0 &&
      current.material_risk_review_hold &&
      std::strcmp(current.lane, "low_pulse_context") == 0 &&
      current.selected_local_exact_bpm >= 40.0 &&
      current.selected_local_exact_bpm <= 70.0 &&
      std::abs(current.selected_local_exact_bpm - current.selected_bpm) <=
          0.15 &&
      current.selected_local_exact_score >= 0.19 &&
      current.continuous_score >= 0.19 &&
      current.continuous_score_ratio >= 1.20 &&
      current.lane_low_support >= 0.60 &&
      current.total_segment_count >= 20 && current.odf_peak_count >= 500) {
    const MirPolicyMeasuredCandidate* currentCandidate =
        find_mir_policy_output_candidate(
            candidates, current.selected_local_exact_bpm);
    if (currentCandidate != nullptr &&
        mir_has_alias(
            currentCandidate->candidate.alias_bits, MirAlias::direct)) {
      select_mir_policy_candidate(shadow, *currentCandidate);
      shadow.output_bpm = currentCandidate->bpm;
      shadow.writer_override_candidate = true;
      shadow.output_would_write = true;
      shadow.output_would_review_hold = false;
      shadow.action = "writer_override";
      shadow.decision_class = "measured_recovery_candidate";
      shadow.reason = "measured_material_risk_low_pulse_release";
      return true;
    }
  }

  if (current.review_hold && shadow.current_bpm > 0.0 &&
      current.sparse_acapella_review_hold) {
    const MirPolicyMeasuredCandidate* sparseRelease =
        select_mir_policy_stable_sparse_low_pulse_current_release(candidates,
                                                               current);
    const char* sparseReleaseReason =
        "measured_sparse_low_pulse_current_release";
    if (sparseRelease == nullptr) {
      sparseRelease =
          select_mir_policy_sparse_top_family_half_alias_release(candidates,
                                                              current);
      sparseReleaseReason =
          "measured_sparse_top_family_half_alias_release";
    }
    if (sparseRelease != nullptr) {
      select_mir_policy_candidate(shadow, *sparseRelease);
      shadow.output_bpm = sparseRelease->bpm;
      shadow.writer_override_candidate = true;
      shadow.output_would_write = true;
      shadow.output_would_review_hold = false;
      shadow.action = "writer_override";
      shadow.decision_class = "measured_recovery_candidate";
      shadow.reason = sparseReleaseReason;
      return true;
    }
  }

  if (current.review_hold && shadow.current_bpm > 0.0 &&
      (current.low_pulse_exactness_review_hold ||
       current.sparse_acapella_review_hold)) {
    const MirPolicyMeasuredCandidate* currentCandidate =
        find_mir_policy_output_candidate(candidates, shadow.current_bpm);
    if (currentCandidate != nullptr) {
      const MirPolicyMeasuredCandidate* recovery = nullptr;
      const char* recoveryReason = nullptr;
      if (current.low_pulse_exactness_review_hold) {
        recovery = select_mir_policy_low_pulse_hold_recovery(
            candidates, *currentCandidate);
        recoveryReason = "measured_low_pulse_hold_recovery";
      } else if (current.sparse_acapella_review_hold) {
        recovery = select_mir_policy_sparse_pulse_hold_recovery(
            candidates, *currentCandidate);
        recoveryReason = "measured_sparse_pulse_hold_recovery";
      }
      if (recovery != nullptr) {
        select_mir_policy_candidate(shadow, *recovery);
        shadow.output_bpm = recovery->bpm;
        shadow.writer_override_candidate = true;
        shadow.output_would_write = true;
        shadow.output_would_review_hold = false;
        shadow.action = "writer_override";
        shadow.decision_class = "measured_recovery_candidate";
        shadow.reason = recoveryReason;
        return true;
      }
    }
  }

  if (current.review_hold && shadow.protected_review_hold) {
    if (mir_policy_sparse_current_direct_release(
            measured, current, shadow.measured_base_margin)) {
      select_mir_policy_candidate(shadow, measured);
      shadow.output_bpm = measured.bpm;
      shadow.writer_override_candidate = true;
      shadow.output_would_write = true;
      shadow.output_would_review_hold = false;
      shadow.action = "writer_override";
      shadow.decision_class = "measured_recovery_candidate";
      shadow.reason = "measured_sparse_current_direct_release_gate";
      return true;
    }
    if (mir_policy_protected_near_measured_release(
            measured, current, shadow.measured_base_margin)) {
      select_mir_policy_candidate(shadow, measured);
      shadow.output_bpm = measured.bpm;
      shadow.writer_override_candidate = true;
      shadow.output_would_write = true;
      shadow.output_would_review_hold = false;
      shadow.action = "writer_override";
      shadow.decision_class = "measured_recovery_candidate";
      shadow.reason = "measured_protected_near_release_gate";
      return true;
    }
    if (mir_policy_protected_half_alias_current_release(
            measured, current, shadow.measured_base_margin)) {
      select_mir_policy_candidate(shadow, measured);
      shadow.output_bpm = current.selected_bpm;
      shadow.writer_override_candidate = true;
      shadow.output_would_write = true;
      shadow.output_would_review_hold = false;
      shadow.action = "writer_override";
      shadow.decision_class = "measured_recovery_candidate";
      shadow.reason = "measured_protected_half_alias_current_release_gate";
      return true;
    }
    if (mir_policy_protected_strong_measured_direct_release(
            measured, current, shadow.measured_base_margin)) {
      select_mir_policy_candidate(shadow, measured);
      shadow.output_bpm = measured.bpm;
      shadow.writer_override_candidate = true;
      shadow.output_would_write = true;
      shadow.output_would_review_hold = false;
      shadow.action = "writer_override";
      shadow.decision_class = "measured_recovery_candidate";
      shadow.reason = "measured_protected_strong_direct_release_gate";
      return true;
    }
  }

  if (const MirPolicyMeasuredCandidate* highFamilyRelease =
          select_mir_policy_high_family_conflict_fullboard_release(
              candidates, current, shadow.prior_center_bpm,
              shadow.prior_spread_bpm, routeMatched, genericRoute)) {
    select_mir_policy_candidate(shadow, *highFamilyRelease);
    shadow.output_bpm = highFamilyRelease->bpm;
    shadow.writer_override_candidate = true;
    shadow.output_would_write = true;
    shadow.output_would_review_hold = false;
    shadow.action = "writer_override";
    shadow.decision_class = "measured_recovery_candidate";
    shadow.reason = "measured_high_family_conflict_fullboard_release";
    return true;
  }

  if (current.review_hold && current.high_family_conflict_review_hold) {
    if (const MirPolicyMeasuredCandidate* highFamilyRelease =
            select_mir_policy_release_ready_scored(
                releaseFeatureCache, shadow.prior_center_bpm,
                shadow.prior_spread_bpm, 1, 0.5)) {
      select_mir_policy_candidate(shadow, *highFamilyRelease);
      shadow.output_bpm = highFamilyRelease->bpm;
      shadow.writer_override_candidate = true;
      shadow.output_would_write = true;
      shadow.output_would_review_hold = false;
      shadow.action = "writer_override";
      shadow.decision_class = "measured_recovery_candidate";
      shadow.reason = "measured_release_ready_high_family_winner";
      return true;
    }
  }

  const bool protectedCurrentExactRelease =
      shadow.current_bpm > 0.0 &&
      std::abs(measured.bpm - shadow.current_bpm) <= 0.05 &&
      measured.candidate.support >= 0.78 &&
      measured.local_exact_score >= 0.27 &&
      shadow.measured_base_margin >= 0.45;
  const bool protectedDirectMeasuredRelease =
      shadow.current_bpm > 0.0 &&
      std::abs(measured.bpm - shadow.current_bpm) <= 18.0 &&
      measured.candidate.support >= 0.84 &&
      measured.local_exact_score >= 0.22 &&
      shadow.measured_base_margin >= 0.17;

  if (current.review_hold && shadow.protected_review_hold &&
      !current.low_pulse_exactness_review_hold &&
      !current.sparse_acapella_review_hold &&
      mir_has_alias(measured.candidate.alias_bits, MirAlias::direct) &&
      (protectedCurrentExactRelease || protectedDirectMeasuredRelease)) {
    select_mir_policy_candidate(shadow, measured);
    shadow.output_bpm = measured.bpm;
    shadow.writer_override_candidate = true;
    shadow.output_would_write = true;
    shadow.output_would_review_hold = false;
    shadow.action = "writer_override";
    shadow.decision_class = "measured_recovery_candidate";
    shadow.reason = protectedCurrentExactRelease
                        ? "measured_protected_current_release_gate"
                        : "measured_protected_direct_release_gate";
    return true;
  }

  if (current.review_hold && current.sparse_acapella_review_hold) {
    if (const MirPolicyMeasuredCandidate* sparseRelease =
            select_mir_policy_release_ready_sparse(releaseFeatureCache)) {
      select_mir_policy_candidate(shadow, *sparseRelease);
      shadow.output_bpm = sparseRelease->bpm;
      shadow.writer_override_candidate = true;
      shadow.output_would_write = true;
      shadow.output_would_review_hold = false;
      shadow.action = "writer_override";
      shadow.decision_class = "measured_recovery_candidate";
      shadow.reason = "measured_release_ready_sparse_family";
      return true;
    }
  }

  if (current.review_hold && current.material_risk_review_hold) {
    if (const MirPolicyMeasuredCandidate* materialRelease =
            select_mir_policy_release_ready_scored(
                releaseFeatureCache, shadow.prior_center_bpm,
                shadow.prior_spread_bpm, 1, 1.0)) {
      select_mir_policy_candidate(shadow, *materialRelease);
      shadow.output_bpm = materialRelease->bpm;
      shadow.writer_override_candidate = true;
      shadow.output_would_write = true;
      shadow.output_would_review_hold = false;
      shadow.action = "writer_override";
      shadow.decision_class = "measured_recovery_candidate";
      shadow.reason = "measured_release_ready_material_risk_winner";
      return true;
    }
  }

  if (current.review_hold && current.low_pulse_exactness_review_hold) {
    const MirPolicyMeasuredCandidate* lowPulseRelease =
        select_mir_policy_release_ready_low_anchor(releaseFeatureCache);
    const char* lowPulseReason = "measured_release_ready_low_anchor";
    if (lowPulseRelease == nullptr) {
      lowPulseRelease = select_mir_policy_release_ready_full_pulse(
          releaseFeatureCache, shadow.prior_center_bpm,
          shadow.prior_spread_bpm);
      lowPulseReason = "measured_release_ready_full_pulse";
    }
    if (lowPulseRelease == nullptr) {
      lowPulseRelease = select_mir_policy_release_ready_low_family_plateau(
          releaseFeatureCache);
      lowPulseReason = "measured_release_ready_low_family_plateau";
    }
    if (lowPulseRelease != nullptr) {
      select_mir_policy_candidate(shadow, *lowPulseRelease);
      shadow.output_bpm = lowPulseRelease->bpm;
      shadow.writer_override_candidate = true;
      shadow.output_would_write = true;
      shadow.output_would_review_hold = false;
      shadow.action = "writer_override";
      shadow.decision_class = "measured_recovery_candidate";
      shadow.reason = lowPulseReason;
      return true;
    }
  }

  const bool promotableReviewHold =
      current.review_hold &&
      (current.soft_center_conflict_review_hold ||
       current.soft_center_alias_review_hold) &&
      !shadow.protected_review_hold;
  if (promotableReviewHold) {
    const bool priorUsable =
        routeMatched && !genericRoute && shadow.prior_center_bpm > 0.0;
    double centerImprovement = 0.0;
    double scoreRatio = 0.0;
    const MirPolicyMeasuredCandidate* selected =
        select_mir_policy_review_promotion_candidate(
            candidates, shadow.prior_center_bpm, priorUsable,
            centerImprovement, scoreRatio);
    if (selected != nullptr) {
      select_mir_policy_candidate(shadow, *selected);
      shadow.harmonic_center_improvement = centerImprovement;
      shadow.harmonic_score_ratio = scoreRatio;
      const bool centerConflictOk =
          priorUsable &&
          std::abs(selected->bpm - shadow.prior_center_bpm) <= 20.0;
      shadow.review_hold_promotion_candidate =
          selected->candidate.support >= 0.20 &&
          selected->local_exact_score >= 0.16 &&
          shadow.measured_base_margin >= 0.0 &&
          centerConflictOk;
      if (shadow.review_hold_promotion_candidate) {
        shadow.output_bpm = selected->bpm;
        shadow.output_would_write = true;
        shadow.output_would_review_hold = false;
        shadow.action = "review_hold_promotion";
        shadow.decision_class = "review_hold_promotion_candidate";
        shadow.reason = "frozen_soft_center_review_promotion_gate";
        return true;
      }
    }
    shadow.output_bpm = 0.0;
    shadow.output_would_write = false;
    shadow.output_would_review_hold = true;
    shadow.action = "keep_review_hold";
    shadow.decision_class = "protected_review_hold";
    shadow.reason = "review_hold_promotion_gate_veto";
    return true;
  }

  return false;
}

void apply_mir_policy_default_resolution_stage(
    smart_tempo::MirPolicyDecision& shadow,
    const smart_tempo::MirPrimarySelection& current) noexcept {
  shadow.output_bpm = shadow.current_auto_candidate ? current.selected_bpm : 0.0;
  shadow.output_would_write = shadow.current_auto_candidate;
  shadow.output_would_review_hold = current.review_hold;
  shadow.action = current.review_hold ? "keep_review_hold" : "keep_current";
  shadow.decision_class =
      current.review_hold ? "protected_review_hold" : "no_policy_action";
  shadow.reason = shadow.protected_review_hold
                      ? "protected_review_hold_reason"
                      : "outside_frozen_policy_surfaces";
}

[[nodiscard]] smart_tempo::MirPolicyDecision
compute_mir_policy_decision(
    std::span<const MirPolicyMeasuredCandidate> candidates,
    const smart_tempo::MirPrimarySelection& current,
    const smart_tempo::HodgkinsonTatumProbeResult& primaryFamily,
    const smart_tempo::HodgkinsonMaterialRiskLocalExactProbe*
        materialRiskProbe,
    const smart_tempo::HodgkinsonPartialBarAggregateShadow*
        partialBarAggregate,
    double priorCenterBpm,
    double priorSpreadBpm,
    bool routeMatched,
    bool genericRoute,
    bool enabled) {
  // Ordered policy stages. Their sequence is part of the release contract.
  MirPolicyQualificationStage qualification = qualify_mir_policy_board(
      candidates, current, priorCenterBpm, priorSpreadBpm, routeMatched,
      genericRoute, enabled);
  auto& shadow = qualification.decision;
  if (!qualification.ready) return shadow;
  const MirPolicyMeasuredCandidate& measured = *qualification.measured;
  MirPolicyReleaseFeatureCache releaseFeatureCache(
      candidates, priorCenterBpm, priorSpreadBpm);

  if (try_apply_mir_policy_current_release_stage(
          shadow, candidates, releaseFeatureCache, current, primaryFamily,
          partialBarAggregate, measured, priorCenterBpm, priorSpreadBpm,
          routeMatched, genericRoute)) {
    return shadow;
  }
  if (try_apply_mir_policy_hold_resolution_stage(
          shadow, candidates, releaseFeatureCache, current, materialRiskProbe,
          measured, priorCenterBpm, priorSpreadBpm, routeMatched,
          genericRoute)) {
    return shadow;
  }
  apply_mir_policy_default_resolution_stage(shadow, current);
  return shadow;
}

bool try_apply_mir_policy_partial_bar_topk_exact_release(
    smart_tempo::MirPolicyDecision& decision,
    std::span<const smart_tempo::HodgkinsonPartialBarTopKExactCandidate>
        candidates,
    double priorCenterBpm, double priorSpreadBpm, bool routeMatched,
    bool genericRoute, bool enabled) noexcept {
  if (!enabled || !decision.enabled ||
      !decision.family_conflict_review_hold_candidate || !routeMatched ||
      genericRoute || !(priorCenterBpm > 0.0) || !(priorSpreadBpm > 0.0) ||
      candidates.empty()) {
    return false;
  }

  double maxExactScore = 0.0;
  for (const auto& candidate : candidates) {
    if (std::isfinite(candidate.local_exact_score)) {
      maxExactScore = (std::max)(maxExactScore, candidate.local_exact_score);
    }
  }
  if (!(maxExactScore > 0.0)) {
    return false;
  }

  const auto relationError = [](double candidateBpm,
                                double fullboardBpm) noexcept {
    const double low = (std::min)(candidateBpm, fullboardBpm);
    const double high = (std::max)(candidateBpm, fullboardBpm);
    constexpr std::array<MirPulseRelation, 3> kRelations{
        MirPulseRelation::four_thirds, MirPulseRelation::three_halves,
        MirPulseRelation::double_time};
    return MirMeasuredPulseFamilyMatrix::minimum_relative_error(
        high, low, kRelations);
  };
  const auto stronger = [](const auto* left, const auto* right) noexcept {
    if (left == nullptr) {
      return right;
    }
    if (right == nullptr) {
      return left;
    }
    return std::make_tuple(left->local_exact_score,
                           left->score_ratio_to_top,
                           -static_cast<double>(left->family_rank)) >=
                   std::make_tuple(right->local_exact_score,
                                   right->score_ratio_to_top,
                                   -static_cast<double>(right->family_rank))
               ? left
               : right;
  };

  const smart_tempo::HodgkinsonPartialBarTopKExactCandidate*
      rationalCandidate = nullptr;
  const smart_tempo::HodgkinsonPartialBarTopKExactCandidate* centerCandidate =
      nullptr;
  for (const auto& candidate : candidates) {
    if (!std::isfinite(candidate.local_exact_bpm) ||
        !std::isfinite(candidate.local_exact_score) ||
        !std::isfinite(candidate.score_ratio_to_top) ||
        !(candidate.local_exact_bpm > 0.0) ||
        candidate.total_segment_count == 0 ||
        candidate.top5_hits < kMirPolicyPartialBarTopKMinTop5Hits) {
      continue;
    }
    const double totalSegments =
        static_cast<double>(candidate.total_segment_count);
    const double segmentSupport =
        static_cast<double>(candidate.segment_count) / totalSegments;
    if (segmentSupport < kMirPolicyPartialBarTopKMinSegmentSupport) {
      continue;
    }
    const double centerZ =
        std::abs(candidate.local_exact_bpm - priorCenterBpm) / priorSpreadBpm;
    const double exactToMax = candidate.local_exact_score / maxExactScore;
    if (relationError(candidate.local_exact_bpm, candidate.fullboard_bpm) <=
            kMirPolicyPartialBarTopKRelationTolerance &&
        centerZ <= kMirPolicyPartialBarTopKRationalMaxCenterZ &&
        candidate.local_exact_score >=
            kMirPolicyPartialBarTopKRationalMinExactScore &&
        candidate.score_ratio_to_top >=
            kMirPolicyPartialBarTopKRationalMinScoreRatio &&
        exactToMax >= kMirPolicyPartialBarTopKRationalMinExactToMax) {
      rationalCandidate = stronger(rationalCandidate, &candidate);
    }
    if (centerZ <= kMirPolicyPartialBarTopKCenterMaxCenterZ &&
        candidate.local_exact_score >=
            kMirPolicyPartialBarTopKCenterMinExactScore &&
        candidate.score_ratio_to_top >=
            kMirPolicyPartialBarTopKCenterMinScoreRatio &&
        exactToMax >= kMirPolicyPartialBarTopKCenterMinExactToMax) {
      centerCandidate = stronger(centerCandidate, &candidate);
    }
  }

  const auto* selected =
      rationalCandidate != nullptr ? rationalCandidate : centerCandidate;
  if (selected == nullptr) {
    return false;
  }
  const double totalSegments =
      static_cast<double>(selected->total_segment_count);
  decision.candidate_bpm = selected->local_exact_bpm;
  decision.candidate_cluster_bpm = selected->coarse_bpm;
  decision.candidate_base_score = selected->score_sum;
  decision.candidate_support =
      static_cast<double>(selected->segment_count) / totalSegments;
  decision.candidate_winner_support =
      static_cast<double>(selected->top5_hits) / totalSegments;
  decision.candidate_local_exact_score = selected->local_exact_score;
  decision.candidate_current_delta =
      selected->local_exact_bpm - decision.current_bpm;
  decision.candidate_local_score_ratio =
      selected->local_exact_score / maxExactScore;
  decision.candidate_support_ratio = selected->score_ratio_to_top;
  decision.candidate_alias_classes =
      rationalCandidate != nullptr ? "partial_bar_topk_rational"
                                   : "partial_bar_topk_center";
  decision.output_bpm = selected->local_exact_bpm;
  decision.writer_override_candidate = true;
  decision.review_hold_promotion_candidate = false;
  decision.partial_bar_topk_exact_release_candidate = true;
  decision.family_conflict_review_hold_candidate = false;
  decision.output_would_write = true;
  decision.output_would_review_hold = false;
  decision.action = "writer_override";
  decision.decision_class = "writer_override_candidate";
  decision.reason = rationalCandidate != nullptr
                        ? "measured_partial_bar_topk_exact_rational_release"
                        : "measured_partial_bar_topk_exact_center_release";
  return true;
}

bool is_mir_policy_partial_bar_cross_view_candidate(
    const smart_tempo::MirPolicyDecision& decision,
    const MirPolicyMeasuredCandidate& candidate) noexcept {
  if (!decision.enabled || !decision.output_would_write ||
      !(decision.output_bpm > 0.0) || !std::isfinite(decision.output_bpm) ||
      !(candidate.bpm > decision.output_bpm) ||
      !std::isfinite(candidate.bpm) ||
      !std::isfinite(candidate.local_exact_score)) {
    return false;
  }
  return mir_pulse_ratio_in_range(candidate.bpm, decision.output_bpm,
                                  kMirPolicyCrossViewMinFamilyRatio,
                                  kMirPolicyCrossViewMaxFamilyRatio) &&
         candidate.local_exact_score >=
             kMirPolicyCrossViewMinLocalExactScore &&
         candidate.candidate.support >=
             kMirPolicyCrossViewMinCandidateSupport &&
         candidate.candidate.continuous_score >=
             kMirPolicyCrossViewMinContinuousScore &&
         candidate.candidate.continuous_section_support >=
             kMirPolicyCrossViewMinContinuousSupport;
}

bool has_mir_policy_partial_bar_cross_view_candidate(
    const smart_tempo::MirPolicyDecision& decision,
    std::span<const MirPolicyMeasuredCandidate> fullBoardCandidates,
    bool enabled) noexcept {
  if (!enabled) {
    return false;
  }
  for (const auto& candidate : fullBoardCandidates) {
    if (is_mir_policy_partial_bar_cross_view_candidate(decision, candidate)) {
      return true;
    }
  }
  return false;
}

bool try_apply_mir_policy_partial_bar_cross_view_release(
    smart_tempo::MirPolicyDecision& decision,
    std::span<const MirPolicyMeasuredCandidate> fullBoardCandidates,
    std::span<const smart_tempo::HodgkinsonPartialBarTopKExactCandidate>
        partialBarTopK,
    bool enabled) noexcept {
  if (!has_mir_policy_partial_bar_cross_view_candidate(
          decision, fullBoardCandidates, enabled) || partialBarTopK.empty()) {
    return false;
  }

  const MirPolicyMeasuredCandidate* selectedFullBoard = nullptr;
  const smart_tempo::HodgkinsonPartialBarTopKExactCandidate*
      selectedPartial = nullptr;
  double selectedCoarseDiff = std::numeric_limits<double>::infinity();
  for (const auto& fullBoard : fullBoardCandidates) {
    if (!is_mir_policy_partial_bar_cross_view_candidate(decision, fullBoard)) {
      continue;
    }
    for (const auto& partial : partialBarTopK) {
      if (!std::isfinite(partial.coarse_bpm) ||
          !std::isfinite(partial.score_sum) ||
          !std::isfinite(partial.score_ratio_to_top) ||
          partial.total_segment_count == 0 ||
          partial.score_ratio_to_top < kMirPolicyCrossViewMinScoreRatio) {
        continue;
      }
      const double totalSegments =
          static_cast<double>(partial.total_segment_count);
      const double top5Support =
          static_cast<double>(partial.top5_hits) / totalSegments;
      const double segmentSupport =
          static_cast<double>(partial.segment_count) / totalSegments;
      const double coarseDiff = std::abs(fullBoard.bpm - partial.coarse_bpm);
      if (top5Support < kMirPolicyCrossViewMinTop5Support ||
          segmentSupport < kMirPolicyCrossViewMinSegmentSupport ||
          coarseDiff > kMirPolicyCrossViewMaxCoarseBridgeBpm) {
        continue;
      }
      const auto strength = std::make_tuple(
          partial.score_sum, -coarseDiff, fullBoard.bpm,
          partial.coarse_bpm);
      const auto selectedStrength =
          selectedPartial != nullptr
              ? std::make_tuple(selectedPartial->score_sum,
                                -selectedCoarseDiff,
                                selectedFullBoard->bpm,
                                selectedPartial->coarse_bpm)
              : std::make_tuple(-std::numeric_limits<double>::infinity(),
                                -std::numeric_limits<double>::infinity(),
                                -std::numeric_limits<double>::infinity(),
                                -std::numeric_limits<double>::infinity());
      if (strength > selectedStrength) {
        selectedFullBoard = &fullBoard;
        selectedPartial = &partial;
        selectedCoarseDiff = coarseDiff;
      }
    }
  }
  if (selectedFullBoard == nullptr || selectedPartial == nullptr) {
    return false;
  }

  select_mir_policy_candidate(decision, *selectedFullBoard);
  decision.candidate_cluster_bpm = selectedPartial->coarse_bpm;
  decision.candidate_current_delta =
      selectedFullBoard->bpm - decision.current_bpm;
  decision.candidate_support_ratio = selectedPartial->score_ratio_to_top;
  decision.candidate_alias_classes = "partial_bar_cross_view";
  decision.output_bpm = selectedFullBoard->bpm;
  decision.writer_override_candidate = true;
  decision.review_hold_promotion_candidate = false;
  decision.partial_bar_topk_exact_release_candidate = true;
  decision.family_conflict_review_hold_candidate = false;
  decision.output_would_write = true;
  decision.output_would_review_hold = false;
  decision.action = "writer_override";
  decision.decision_class = "writer_override_candidate";
  decision.reason = "measured_partial_bar_cross_view_bridge_release";
  return true;
}

[[nodiscard]] bool is_mir_policy_keep_current_micro_upgrade(
    const smart_tempo::MirPolicyDecision& decision) noexcept {
  return !decision.writer_override_candidate &&
         !decision.review_hold_promotion_candidate &&
         std::strcmp(decision.action, "keep_current") == 0 &&
         decision.current_auto_candidate && !decision.current_review_hold &&
         decision.candidate_support >= 0.40 &&
         decision.candidate_local_score_ratio >= 1.20 &&
         decision.candidate_support_ratio >= 0.45 &&
         std::abs(decision.candidate_current_delta) >= 0.05 &&
         std::abs(decision.candidate_current_delta) <= 3.00 &&
         decision.candidate_bpm > 0.0 &&
         std::isfinite(decision.candidate_bpm);
}

[[nodiscard]] double effective_mir_policy_output_bpm(
    const smart_tempo::MirPolicyDecision& decision) noexcept {
  return is_mir_policy_keep_current_micro_upgrade(decision)
             ? decision.candidate_bpm
             : decision.output_bpm;
}

[[nodiscard]] bool mir_output_has_measured_candidate_witness(
    double outputBpm,
    std::span<const MirPolicyMeasuredCandidate> fullBoardCandidates,
    const smart_tempo::MirPrimarySelection& primarySelection,
    const smart_tempo::HodgkinsonMaterialRiskLocalExactProbe*
        materialRiskProbe,
    const smart_tempo::HodgkinsonPartialBarAggregateShadow*
        partialBarAggregate,
    std::span<const smart_tempo::HodgkinsonPartialBarTopKExactCandidate>
        partialBarTopK) noexcept {
  const auto sameMeasuredBpm = [](double left, double right) noexcept {
    return left > 0.0 && right > 0.0 && std::isfinite(left) &&
           std::isfinite(right) && std::abs(left - right) <= 0.001;
  };
  const bool outputIsLegal = outputBpm >= kMirProductionMinBpm &&
                             outputBpm <= kMirProductionMaxBpm &&
                             std::isfinite(outputBpm);
  bool witnessed = outputIsLegal && std::any_of(
      fullBoardCandidates.begin(), fullBoardCandidates.end(),
      [outputBpm, &sameMeasuredBpm](const auto& candidate) {
        return sameMeasuredBpm(outputBpm, candidate.bpm);
      });
  if (!outputIsLegal) {
    witnessed = false;
  } else {
    witnessed = witnessed ||
                sameMeasuredBpm(outputBpm, primarySelection.selected_bpm) ||
                sameMeasuredBpm(outputBpm,
                                primarySelection.selected_local_exact_bpm);
    witnessed = witnessed ||
                (materialRiskProbe != nullptr &&
                 materialRiskProbe->candidate &&
                 sameMeasuredBpm(outputBpm, materialRiskProbe->best_bpm));
    witnessed = witnessed ||
                (partialBarAggregate != nullptr &&
                 partialBarAggregate->candidate &&
                 sameMeasuredBpm(outputBpm,
                                 partialBarAggregate->local_exact_bpm));
    witnessed = witnessed || std::any_of(
                                 partialBarTopK.begin(), partialBarTopK.end(),
                                 [outputBpm, &sameMeasuredBpm](const auto& row) {
                                   return sameMeasuredBpm(
                                       outputBpm, row.local_exact_bpm);
                                 });
  }
  return witnessed;
}

bool enforce_mir_measured_candidate_invariant(
    smart_tempo::MirPolicyDecision& decision,
    std::span<const MirPolicyMeasuredCandidate> fullBoardCandidates,
    const smart_tempo::MirPrimarySelection& primarySelection,
    const smart_tempo::HodgkinsonMaterialRiskLocalExactProbe*
        materialRiskProbe,
    const smart_tempo::HodgkinsonPartialBarAggregateShadow*
        partialBarAggregate,
    std::span<const smart_tempo::HodgkinsonPartialBarTopKExactCandidate>
        partialBarTopK) noexcept {
  if (!decision.output_would_write) {
    return true;
  }
  if (mir_output_has_measured_candidate_witness(
          effective_mir_policy_output_bpm(decision), fullBoardCandidates,
          primarySelection, materialRiskProbe, partialBarAggregate,
          partialBarTopK)) {
    return true;
  }
  decision.output_would_write = false;
  decision.output_would_review_hold = true;
  decision.writer_override_candidate = false;
  decision.review_hold_promotion_candidate = false;
  decision.action = "invariant_review_hold";
  decision.decision_class = "measured_candidate_invariant_hold";
  decision.reason = "unmeasured_policy_output_veto";
  decision.output_bpm = 0.0;
  return false;
}

[[nodiscard]] std::vector<MirManualReviewCandidate>
prepare_mir_manual_review_candidates(
    std::span<const MirPolicyMeasuredCandidate> fullBoardCandidates,
    const smart_tempo::MirPrimarySelection& primarySelection,
    const smart_tempo::HodgkinsonMaterialRiskLocalExactProbe*
        materialRiskProbe,
    const smart_tempo::HodgkinsonPartialBarAggregateShadow*
        partialBarAggregate,
    std::span<const smart_tempo::HodgkinsonPartialBarTopKExactCandidate>
        partialBarTopK) {
  std::vector<MirManualReviewCandidate> out;
  out.reserve(fullBoardCandidates.size() + partialBarTopK.size() + 3);

  const auto appendUniqueText = [](std::string& target,
                                   const char* value) {
    if (value == nullptr || value[0] == '\0') return;
    const std::string incoming(value);
    std::size_t incomingStart = 0;
    while (incomingStart <= incoming.size()) {
      const std::size_t incomingEnd = incoming.find('|', incomingStart);
      const std::string token =
          incoming.substr(incomingStart, incomingEnd - incomingStart);
      bool present = token.empty();
      std::size_t targetStart = 0;
      while (!present && targetStart <= target.size()) {
        const std::size_t targetEnd = target.find('|', targetStart);
        present = target.substr(targetStart, targetEnd - targetStart) == token;
        if (targetEnd == std::string::npos) break;
        targetStart = targetEnd + 1;
      }
      if (!present) {
        if (!target.empty()) target += '|';
        target += token;
      }
      if (incomingEnd == std::string::npos) break;
      incomingStart = incomingEnd + 1;
    }
  };
  const auto add = [&](double bpm, double evidenceScore, double support,
                       double winnerSupport, double localExactScore,
                       std::size_t evidenceRank,
                       const char* aliasClasses, const char* source) {
    if (!(bpm >= kMirProductionMinBpm && bpm <= kMirProductionMaxBpm) ||
        !std::isfinite(bpm)) {
      return;
    }
    auto existing = std::find_if(
        out.begin(), out.end(), [bpm](const auto& candidate) {
          return std::abs(candidate.bpm - bpm) <= 0.001;
        });
    if (existing == out.end()) {
      MirManualReviewCandidate candidate;
      candidate.bpm = bpm;
      candidate.evidence_score = evidenceScore;
      candidate.support = support;
      candidate.winner_support = winnerSupport;
      candidate.local_exact_score = localExactScore;
      candidate.evidence_rank = evidenceRank;
      candidate.alias_classes = aliasClasses != nullptr ? aliasClasses : "";
      candidate.sources = source != nullptr ? source : "";
      out.push_back(std::move(candidate));
      return;
    }
    existing->evidence_score =
        (std::max)(existing->evidence_score, evidenceScore);
    existing->support = (std::max)(existing->support, support);
    existing->winner_support =
        (std::max)(existing->winner_support, winnerSupport);
    existing->local_exact_score =
        (std::max)(existing->local_exact_score, localExactScore);
    if (evidenceRank > 0 &&
        (existing->evidence_rank == 0 ||
         evidenceRank < existing->evidence_rank)) {
      existing->evidence_rank = evidenceRank;
    }
    appendUniqueText(existing->alias_classes, aliasClasses);
    appendUniqueText(existing->sources, source);
  };

  for (std::size_t index = 0; index < fullBoardCandidates.size(); ++index) {
    const auto& measured = fullBoardCandidates[index];
    add(measured.bpm, measured.base_score, measured.candidate.support,
        measured.candidate.winner_support, measured.local_exact_score,
        index + 1, measured.alias_classes.c_str(), "full_board");
  }
  if (primarySelection.candidate) {
    const double primaryBpm =
        primarySelection.selected_local_exact_bpm > 0.0
            ? primarySelection.selected_local_exact_bpm
            : primarySelection.selected_bpm;
    add(primaryBpm, primarySelection.measured_evidence_score,
        primarySelection.support, primarySelection.winner_support,
        primarySelection.selected_local_exact_score, 0,
        primarySelection.alias_classes.c_str(), "primary_selector");
  }
  if (materialRiskProbe != nullptr && materialRiskProbe->candidate) {
    add(materialRiskProbe->best_bpm, materialRiskProbe->best_score,
        0.0, 0.0,
        materialRiskProbe->best_score, 0, "phase_scan",
        "material_risk_probe");
  }
  if (partialBarAggregate != nullptr && partialBarAggregate->candidate) {
    const double total =
        static_cast<double>(partialBarAggregate->total_segment_count);
    add(partialBarAggregate->local_exact_bpm,
        partialBarAggregate->score_sum,
        total > 0.0 ? partialBarAggregate->segment_count / total : 0.0,
        total > 0.0 ? partialBarAggregate->top5_hits / total : 0.0,
        partialBarAggregate->local_exact_score, 0, "partial_bar",
        "partial_bar_aggregate");
  }
  for (const auto& row : partialBarTopK) {
    const double total = static_cast<double>(row.total_segment_count);
    add(row.local_exact_bpm, row.score_sum,
        total > 0.0 ? row.segment_count / total : 0.0,
        total > 0.0 ? row.top5_hits / total : 0.0,
        row.local_exact_score, 0, "partial_bar",
        "partial_bar_topk");
  }

  std::stable_sort(
      out.begin(), out.end(), [](const auto& left, const auto& right) {
        const std::size_t leftRank =
            left.evidence_rank > 0 ? left.evidence_rank : 0;
        const std::size_t rightRank =
            right.evidence_rank > 0 ? right.evidence_rank : 0;
        return std::make_tuple(leftRank == 0, -left.evidence_score,
                               -left.support, -left.local_exact_score,
                               left.bpm) <
               std::make_tuple(rightRank == 0, -right.evidence_score,
                               -right.support, -right.local_exact_score,
                               right.bpm);
      });
  for (std::size_t index = 0; index < out.size(); ++index) {
    if (out[index].evidence_rank == 0) {
      out[index].evidence_rank = index + 1;
    }
  }
  return out;
}

[[nodiscard]] bool
is_mir_policy_material_risk_local_exact_release_candidate(
    const smart_tempo::HodgkinsonMaterialRiskLocalExactProbe& probe) noexcept {
  if (!probe.enabled || !probe.candidate || !probe.review_hold_candidate ||
      !std::isfinite(probe.selected_context_bpm) ||
      !std::isfinite(probe.current_local_exact_score) ||
      !std::isfinite(probe.best_bpm) || !std::isfinite(probe.best_score) ||
      !std::isfinite(probe.best_score_ratio_to_current) ||
      !std::isfinite(probe.top_span_bpm)) {
    return false;
  }
  const double targetMin =
      probe.selected_context_bpm - kMirPolicyMaterialRiskProbeTargetBelowMinBpm;
  const double targetMax =
      probe.selected_context_bpm - kMirPolicyMaterialRiskProbeTargetBelowMaxBpm;
  return std::abs(probe.selected_context_bpm -
                  kMirPolicyMaterialRiskProbeContextBpm) <=
             kMirPolicyMaterialRiskProbeContextToleranceBpm &&
         probe.best_bpm >= targetMin && probe.best_bpm <= targetMax &&
         probe.current_local_exact_score >=
             kMirPolicyMaterialRiskProbeMinCurrentScore &&
         probe.best_score >= kMirPolicyMaterialRiskProbeMinBestScore &&
         probe.best_score_ratio_to_current >=
             kMirPolicyMaterialRiskProbeMinScoreRatio &&
         probe.target_family_candidate_count >=
             kMirPolicyMaterialRiskProbeMinTargetCandidateCount &&
         probe.peak_count >= kMirPolicyMaterialRiskProbeMinPeakCount &&
         probe.non_boundary_top_count >=
             kMirPolicyMaterialRiskProbeMinNonBoundaryTopCount &&
         probe.top_span_bpm <= kMirPolicyMaterialRiskProbeMaxTopSpanBpm;
}

struct MirAliasSelectorCompetition {
  bool selected_rank1_three_quarters = false;
  double runner_up_score_ratio = 0.0;
};

[[nodiscard]] double mir_alias_prior(const char* aliasClass) noexcept {
  if (aliasClass == nullptr) return 0.35;
  if (std::strcmp(aliasClass, "direct") == 0) return 1.00;
  if (std::strcmp(aliasClass, "half") == 0) return 0.92;
  if (std::strcmp(aliasClass, "double") == 0) return 0.88;
  if (std::strcmp(aliasClass, "two_thirds") == 0) return 0.78;
  if (std::strcmp(aliasClass, "three_halves") == 0) return 0.76;
  if (std::strcmp(aliasClass, "three_quarters") == 0) return 0.70;
  if (std::strcmp(aliasClass, "four_thirds") == 0) return 0.70;
  if (std::strcmp(aliasClass, "one_third") == 0) return 0.45;
  if (std::strcmp(aliasClass, "quarter") == 0) return 0.35;
  if (std::strcmp(aliasClass, "triple") == 0) return 0.34;
  if (std::strcmp(aliasClass, "quadruple") == 0) return 0.25;
  return 0.35;
}

[[nodiscard]] MirAliasSelectorCompetition
compute_mir_alias_selector_competition(
    const smart_tempo::HodgkinsonTatumProbeResult& result,
    double routeMinBpm,
    double routeMaxBpm) noexcept {
  struct ScoredOption {
    double score = 0.0;
    double center_distance = 0.0;
    std::size_t rank_index = 0;
    const char* alias_class = "invalid";
  };

  MirAliasSelectorCompetition competition;
  if (!(routeMinBpm > 0.0 && routeMaxBpm > routeMinBpm) ||
      result.family_rank_count == 0 || result.family_rank_score[0] <= 0.0 ||
      result.candidate_segment_count == 0) {
    return competition;
  }

  const std::size_t fitSegments =
      result.fit_segment_count > 0 ? result.fit_segment_count
                                   : result.segment_count;
  const double setSupport =
      fitSegments > 0
          ? static_cast<double>(result.consensus_support) /
                static_cast<double>(fitSegments)
          : 0.0;
  const double topScore = result.family_rank_score[0];
  const double runnerRatio =
      result.runner_up_family_score > 0.0
          ? result.runner_up_family_score / topScore
          : 0.0;
  const double dominance =
      result.runner_up_family_score > 0.0
          ? topScore / result.runner_up_family_score
          : topScore;
  const double aggregateQuality =
      (std::clamp)(setSupport, 0.0, 1.0) * 0.40 +
      (std::clamp)(std::log1p(dominance) / std::log1p(8.0), 0.0, 1.0) *
          0.35 +
      (std::clamp)(1.0 - runnerRatio, 0.0, 1.0) * 0.25;
  const double routeCenter = (routeMinBpm + routeMaxBpm) * 0.5;
  const double routeHalfWidth =
      (std::max)(1.0, routeMaxBpm - routeMinBpm) * 0.5;

  std::array<ScoredOption, 44> options{};
  std::size_t optionCount = 0;
  const std::size_t rankCount =
      (std::min)(result.family_rank_count, result.family_rank_bpm.size());
  for (std::size_t rank = 0; rank < rankCount; ++rank) {
    const double familyBpm = result.family_rank_bpm[rank];
    const double familyScore = result.family_rank_score[rank];
    if (!(familyBpm > 0.0) || !(familyScore > 0.0)) continue;
    const double rankScoreRatio = familyScore / topScore;
    const double rankSupportRatio =
        static_cast<double>(result.family_rank_support[rank]) /
        static_cast<double>(result.candidate_segment_count);
    for (const auto& alias : kMirAliases) {
      const double aliasBpm = familyBpm * alias.multiplier;
      if (aliasBpm < routeMinBpm || aliasBpm > routeMaxBpm ||
          optionCount >= options.size()) {
        continue;
      }
      const double centerDistance = std::abs(aliasBpm - routeCenter);
      const double centerScore =
          (std::max)(0.0, 1.0 - (std::min)(1.0, centerDistance /
                                                       routeHalfWidth));
      options[optionCount++] = ScoredOption{
          42.0 * rankScoreRatio + 30.0 * rankSupportRatio +
              20.0 * aggregateQuality +
              13.0 * mir_alias_prior(alias.name) + 7.0 * centerScore -
              1.75 * static_cast<double>(rank),
          centerDistance, rank, alias.name};
    }
  }
  if (optionCount == 0) return competition;

  std::sort(options.begin(), options.begin() + optionCount,
            [](const ScoredOption& lhs, const ScoredOption& rhs) {
              if (lhs.score != rhs.score) return lhs.score > rhs.score;
              if (lhs.rank_index != rhs.rank_index) {
                return lhs.rank_index < rhs.rank_index;
              }
              return lhs.center_distance < rhs.center_distance;
            });
  competition.selected_rank1_three_quarters =
      options[0].rank_index == 0 &&
      std::strcmp(options[0].alias_class, "three_quarters") == 0;
  if (optionCount > 1 && options[0].score > 0.0) {
    competition.runner_up_score_ratio = options[1].score / options[0].score;
  }
  return competition;
}

[[nodiscard]] bool try_apply_mir_review_hold_alias_release_v0(
    smart_tempo::MirPrimarySelection& shadow,
    const smart_tempo::HodgkinsonTatumProbeResult& result) noexcept {
  if (!shadow.candidate || !shadow.soft_center_alias_review_hold ||
      shadow.sparse_acapella_review_hold || shadow.material_risk_review_hold ||
      shadow.high_family_conflict_review_hold ||
      shadow.low_pulse_exactness_review_hold ||
      shadow.soft_center_conflict_review_hold || !shadow.route_valid ||
      !(shadow.route_min_bpm > 0.0 &&
        shadow.route_max_bpm > shadow.route_min_bpm) ||
      !(shadow.selected_local_exact_bpm >= 168.0) ||
      result.family_rank_count == 0 || result.family_rank_bpm[0] <= 0.0 ||
      result.family_rank_score[0] <= 0.0 ||
      result.candidate_segment_count == 0) {
    return false;
  }

  const std::size_t totalSegments =
      result.total_segment_count > 0
          ? result.total_segment_count
          : (result.fit_segment_count > 0 ? result.fit_segment_count
                                          : result.segment_count);
  if (totalSegments == 0) {
    return false;
  }

  const double candidateSegmentRatio =
      static_cast<double>(result.candidate_segment_count) /
      static_cast<double>(totalSegments);
  const double rankCandidateSupportRatio =
      static_cast<double>(result.family_rank_candidate_support[0]) /
      static_cast<double>(result.candidate_segment_count);
  const MirAliasSelectorCompetition selectorCompetition =
      compute_mir_alias_selector_competition(
          result, shadow.route_min_bpm, shadow.route_max_bpm);
  const double runnerUpScoreRatio =
      selectorCompetition.runner_up_score_ratio;
  const double aliasBpm = result.family_rank_bpm[0] * 0.75;

  if (!(aliasBpm >= kMirProductionMinBpm && aliasBpm <= kMirProductionMaxBpm) ||
      !std::isfinite(aliasBpm) || aliasBpm < shadow.route_min_bpm ||
      aliasBpm > shadow.route_max_bpm ||
      !selectorCompetition.selected_rank1_three_quarters ||
      candidateSegmentRatio < 0.65 ||
      rankCandidateSupportRatio < 0.85 || runnerUpScoreRatio > 0.45) {
    return false;
  }

  shadow.review_hold_alias_release = true;
  shadow.review_hold_alias_release_bpm = aliasBpm;
  shadow.review_hold_alias_release_candidate_segment_ratio =
      candidateSegmentRatio;
  shadow.review_hold_alias_release_rank_candidate_support_ratio =
      rankCandidateSupportRatio;
  shadow.review_hold_alias_release_runner_up_score_ratio = runnerUpScoreRatio;
  shadow.soft_center_alias_review_hold = false;
  shadow.selected_bpm = aliasBpm;
  shadow.selected_cluster_bpm = aliasBpm;
  shadow.selected_local_exact_bpm = aliasBpm;
  shadow.selected_local_exact_score = 0.0;
  shadow.selected_local_exact_delta = 0.0;
  shadow.support = candidateSegmentRatio;
  shadow.winner_support = rankCandidateSupportRatio;
  shadow.score_sum = result.family_rank_score[0];
  shadow.alias_classes = "three_quarters";
  shadow.lane_variant = "review_hold_alias_release_v0";
  return true;
}


[[nodiscard]] AliasPhaseMetrics compute_alias_phase_metrics_with_span(
    std::span<const uint64_t> onsets,
    double aliasBpm,
    double sampleRate,
    uint64_t onsetSpanSamples) noexcept {
  AliasPhaseMetrics metrics;
  if (onsets.size() < 2 || aliasBpm <= 0.0 || !std::isfinite(aliasBpm) ||
      sampleRate <= 0.0 || !std::isfinite(sampleRate)) {
    return metrics;
  }

  constexpr double kTwoPi = 6.283185307179586476925286766559;
  const double periodSamples = (60.0 / aliasBpm) * sampleRate;
  if (periodSamples <= 0.0 || !std::isfinite(periodSamples)) {
    return metrics;
  }

  double beatSumSin = 0.0;
  double beatSumCos = 0.0;
  double axialSumSin = 0.0;
  double axialSumCos = 0.0;
  for (const uint64_t onset : onsets) {
    double phase =
        std::fmod(static_cast<double>(onset), periodSamples) / periodSamples;
    if (phase < 0.0) {
      phase += 1.0;
    }
    const double angle = phase * kTwoPi;
    const double axialAngle = std::fmod(angle * 2.0, kTwoPi);
    beatSumSin += std::sin(angle);
    beatSumCos += std::cos(angle);
    axialSumSin += std::sin(axialAngle);
    axialSumCos += std::cos(axialAngle);
  }
  const double onsetCount = static_cast<double>(onsets.size());
  metrics.phase_vs =
      std::sqrt(beatSumSin * beatSumSin + beatSumCos * beatSumCos) / onsetCount;
  metrics.axial_phase_vs =
      std::sqrt(axialSumSin * axialSumSin + axialSumCos * axialSumCos) /
      onsetCount;

  if (onsetSpanSamples > 0) {
    const double durationSec =
        static_cast<double>(onsetSpanSamples) / sampleRate;
    const double expectedBeats = (aliasBpm / 60.0) * durationSec;
    if (expectedBeats > 0.0 && std::isfinite(expectedBeats)) {
      metrics.normalized_onset_density =
          (std::min)(4.0, static_cast<double>(onsets.size()) / expectedBeats);
    }
  }
  return metrics;
}

AliasPhaseMetrics compute_alias_phase_metrics(
    std::span<const uint64_t> onsets,
    double aliasBpm,
    double sampleRate) noexcept {
  uint64_t onsetSpanSamples = 0;
  if (!onsets.empty()) {
    const auto [minIt, maxIt] = std::minmax_element(onsets.begin(), onsets.end());
    if (minIt != onsets.end() && maxIt != onsets.end() && *maxIt > *minIt) {
      onsetSpanSamples = *maxIt - *minIt;
    }
  }
  return compute_alias_phase_metrics_with_span(onsets, aliasBpm, sampleRate,
                                               onsetSpanSamples);
}

[[nodiscard]] double compute_continuous_refinement_score(
    const AliasPhaseMetrics& phaseMetrics) noexcept {
  return 0.50 * phaseMetrics.phase_vs +
         0.35 * phaseMetrics.axial_phase_vs +
         0.15 * (std::min)(1.0, phaseMetrics.normalized_onset_density);
}

[[nodiscard]] MirLocalExactBpm measure_mir_local_exact_bpm(
    std::span<const uint64_t> sortedMirPeaks,
    double clusterBpm,
    double sampleRate) {
  MirLocalExactCache cache(sortedMirPeaks, sampleRate);
  return cache.measure(clusterBpm);
}

[[nodiscard]] uint64_t quantized_local_exact_score_key(double bpm) noexcept {
  if (!(bpm > 0.0) || !std::isfinite(bpm)) {
    return 0;
  }
  // Collapse only floating-point representation noise for the same scan point.
  // The measured BPM itself stays untouched.
  constexpr double kMicroBpmScale = 1000000.0;
  return static_cast<uint64_t>(std::llround(bpm * kMicroBpmScale));
}

MirLocalExactCache::MirLocalExactCache(
    std::span<const uint64_t> sortedMirPeaks, double sampleRate)
    : m_sortedMirPeaks(sortedMirPeaks), m_sampleRate(sampleRate) {
  if (!m_sortedMirPeaks.empty()) {
    const auto [minIt, maxIt] =
        std::minmax_element(m_sortedMirPeaks.begin(), m_sortedMirPeaks.end());
    if (minIt != m_sortedMirPeaks.end() && maxIt != m_sortedMirPeaks.end() &&
        *maxIt > *minIt) {
      m_onsetSpanSamples = *maxIt - *minIt;
    }
  }
  // The accepted 60-track performance profile reached 10,543 unique
  // Local-Exact scan points on one track. Reserve above that observed ceiling
  // so the score table does not rehash mid-analysis; this changes allocation
  // behavior only, never candidate arithmetic or ordering.
  m_scoreByBpmBits.reserve(16384);
  m_resultByClusterBits.reserve(256);
}

MirLocalExactBpm MirLocalExactCache::measure(double clusterBpm) {
  MirLocalExactBpm result;
  if (m_sortedMirPeaks.size() < 2 || !(clusterBpm > 0.0) ||
      !std::isfinite(clusterBpm) || !(m_sampleRate > 0.0) ||
      !std::isfinite(m_sampleRate)) {
    return result;
  }

  const uint64_t clusterKey = std::bit_cast<uint64_t>(clusterBpm);
  if (const auto cached = m_resultByClusterBits.find(clusterKey);
      cached != m_resultByClusterBits.end()) {
    ++m_resultHits;
    return cached->second;
  }

  constexpr double kLocalHalfWindowBpm = 0.75;
  constexpr double kLocalStepBpm = 0.01;
  const double scanStart = (std::max)(30.0, clusterBpm - kLocalHalfWindowBpm);
  const double scanEnd = clusterBpm + kLocalHalfWindowBpm;
  const std::size_t stepCount = static_cast<std::size_t>(std::llround(
      (scanEnd - scanStart) / kLocalStepBpm));

  double bestBpm = clusterBpm;
  double bestScore = -std::numeric_limits<double>::infinity();
  for (std::size_t index = 0; index <= stepCount; ++index) {
    const double bpm = scanStart + static_cast<double>(index) * kLocalStepBpm;
    const uint64_t bpmKey = quantized_local_exact_score_key(bpm);
    double score = 0.0;
    if (const auto cached = m_scoreByBpmBits.find(bpmKey);
        cached != m_scoreByBpmBits.end()) {
      ++m_scoreHits;
      score = cached->second;
    } else {
      const AliasPhaseMetrics phase =
          compute_alias_phase_metrics_with_span(
              m_sortedMirPeaks, bpm, m_sampleRate, m_onsetSpanSamples);
      score = compute_continuous_refinement_score(phase);
      m_scoreByBpmBits.emplace(bpmKey, score);
    }
    if (score > bestScore) {
      bestScore = score;
      bestBpm = bpm;
    }
  }

  if (bestBpm > 0.0 && std::isfinite(bestBpm) && std::isfinite(bestScore)) {
    result.bpm = bestBpm;
    result.score = bestScore;
  }
  m_resultByClusterBits.emplace(clusterKey, result);
  return result;
}

[[nodiscard]] bool is_mir_low_pulse_exactness_review_hold(
    const smart_tempo::MirPrimarySelection& shadow) {
  return std::strcmp(shadow.lane, "low_pulse_context") == 0 &&
         !shadow.sparse_acapella_review_hold &&
         !shadow.material_risk_review_hold &&
         !shadow.high_family_conflict_review_hold &&
         !shadow.soft_center_conflict_review_hold &&
         !shadow.soft_center_alias_review_hold &&
         shadow.selected_local_exact_score > 0.0 &&
         shadow.selected_local_exact_score < 0.18 && shadow.support >= 0.30 &&
         shadow.winner_support >= 0.12 &&
         std::abs(shadow.selected_local_exact_delta) >= 0.15;
}

void refresh_mir_review_state(
    smart_tempo::MirPrimarySelection& shadow) {
  shadow.review_hold = shadow.sparse_acapella_review_hold ||
                       shadow.material_risk_review_hold ||
                       shadow.high_family_conflict_review_hold ||
                       shadow.soft_center_conflict_review_hold ||
                       shadow.soft_center_alias_review_hold ||
                       shadow.low_pulse_exactness_review_hold;
  shadow.candidate =
      shadow.selected_bpm > 0.0 && std::isfinite(shadow.selected_bpm);
  shadow.auto_candidate = shadow.candidate && !shadow.review_hold;
  if (shadow.material_risk_review_hold) {
    shadow.decision_class = "review_hold";
    shadow.reason = "material_risk_review_hold";
  } else if (shadow.high_family_conflict_review_hold) {
    shadow.decision_class = "review_hold";
    shadow.reason = "high_family_conflict_review_hold";
  } else if (shadow.soft_center_conflict_review_hold) {
    shadow.decision_class = "review_hold";
    shadow.reason = "soft_center_conflict_review_hold";
  } else if (shadow.soft_center_alias_review_hold) {
    shadow.decision_class = "review_hold";
    shadow.reason = "soft_center_alias_review_hold";
  } else if (shadow.low_pulse_exactness_review_hold) {
    shadow.decision_class = "review_hold";
    shadow.reason = "low_pulse_exactness_review_hold";
  } else if (shadow.sparse_acapella_review_hold) {
    shadow.decision_class = "review_hold";
    shadow.reason = "sparse_or_acapella_review_hold";
  } else if (shadow.auto_candidate) {
    shadow.decision_class = "auto_candidate";
    shadow.reason = shadow.review_hold_alias_release
                        ? "review_hold_alias_release_v0"
                        : "measured_candidate_primary";
  } else {
    shadow.decision_class = "no_auto_candidate";
    shadow.reason = "invalid_primary_candidate";
  }
}
void apply_mir_local_exact_result(
    smart_tempo::MirPrimarySelection& shadow,
    double clusterBpm,
    const MirLocalExactBpm& exact) {
  shadow.selected_cluster_bpm = clusterBpm;
  if (!(clusterBpm > 0.0) || !std::isfinite(clusterBpm)) {
    return;
  }

  if (!(exact.bpm > 0.0) || !std::isfinite(exact.bpm)) {
    return;
  }

  shadow.selected_local_exact_bpm = exact.bpm;
  shadow.selected_local_exact_score = exact.score;
  shadow.selected_local_exact_delta = exact.bpm - clusterBpm;

  // The selector still chooses the robust primary bucket. Only the writable value
  // is refined locally so we do not write quarter-BPM cluster labels as if
  // they were exact measurements.
  if (shadow.auto_candidate && !shadow.review_hold &&
      std::abs(exact.bpm - clusterBpm) <= 0.80) {
    shadow.selected_bpm = exact.bpm;
    shadow.route_valid = shadow.selected_bpm >= kMirProductionMinBpm &&
                         shadow.selected_bpm <= kMirProductionMaxBpm;
    shadow.candidate =
        shadow.selected_bpm > 0.0 && std::isfinite(shadow.selected_bpm);
    shadow.auto_candidate = shadow.candidate && !shadow.review_hold;
  }
}

void apply_mir_local_exact_measurement(
    smart_tempo::MirPrimarySelection& shadow,
    MirLocalExactCache& localExactCache) {
  const double clusterBpm = shadow.selected_bpm;
  const MirLocalExactBpm exact = localExactCache.measure(clusterBpm);
  apply_mir_local_exact_result(shadow, clusterBpm, exact);
}

void finalize_mir_selector_shadow(
    smart_tempo::MirPrimarySelection& shadow,
    const smart_tempo::HodgkinsonTatumProbeResult& result,
    MirLocalExactCache& localExactCache) {
  apply_mir_local_exact_measurement(shadow, localExactCache);
  if (is_mir_soft_center_alias_review_hold(shadow)) {
    shadow.soft_center_alias_review_hold = true;
  }
  if (is_mir_low_pulse_exactness_review_hold(shadow)) {
    shadow.low_pulse_exactness_review_hold = true;
  }
  if (try_apply_mir_review_hold_alias_release_v0(shadow, result)) {
    apply_mir_local_exact_measurement(shadow, localExactCache);
  }
  refresh_mir_review_state(shadow);
}
}  // namespace smart_tempo::mir_pipeline

