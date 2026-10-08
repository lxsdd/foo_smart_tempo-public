#pragma once

#include "hodgkinson_full_mir.h"
#include "hodgkinson_tatum_probe.h"
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace smart_tempo::mir_pipeline {

inline constexpr double kMirPolicyOutOfRailWitnessMinSupport = 0.80;
inline constexpr double kMirPolicyOutOfRailWitnessMinLocalExactScore = 0.25;
inline constexpr double kMirPolicyMaterialRiskProbeContextBpm = 144.0;
inline constexpr double kMirPolicyMaterialRiskProbeContextToleranceBpm = 0.5;
inline constexpr double kMirPolicyMaterialRiskProbeScanBelowStartBpm = 10.0;
inline constexpr double kMirPolicyMaterialRiskProbeScanBelowEndBpm = 2.0;
inline constexpr double kMirPolicyMaterialRiskProbeTargetBelowMinBpm = 8.0;
inline constexpr double kMirPolicyMaterialRiskProbeTargetBelowMaxBpm = 3.0;
inline constexpr double kMirPolicyMaterialRiskProbeScanStepBpm = 0.25;
inline constexpr double kMirPolicyMaterialRiskProbeMinCurrentScore = 0.15;
inline constexpr double kMirPolicyMaterialRiskProbeMinBestScore = 0.18;
inline constexpr double kMirPolicyMaterialRiskProbeMinScoreRatio = 1.18;
inline constexpr std::size_t
    kMirPolicyMaterialRiskProbeMinTargetCandidateCount = 8;
inline constexpr std::size_t kMirPolicyMaterialRiskProbeMinPeakCount = 5000;
inline constexpr std::size_t
    kMirPolicyMaterialRiskProbeMinNonBoundaryTopCount = 2;
inline constexpr double kMirPolicyMaterialRiskProbeMaxTopSpanBpm = 0.10;
inline constexpr bool kEnableMirPolicyPartialBarFamilyConflictRecovery = true;
inline constexpr double kMirPolicyPartialBarMinRank1Support = 0.90;
inline constexpr double kMirPolicyPartialBarMinTop5Support = 0.98;
inline constexpr double kMirPolicyPartialBarMinSegmentSupport = 0.98;
inline constexpr double kMirPolicyPartialBarMaxCenterZ = 0.80;
inline constexpr double kMirPolicyPartialBarMaxRunnerRatio = 0.65;
inline constexpr double kMirPolicyPartialBarMinLocalExactScore = 0.17;
inline constexpr double kMirPolicyPartialBarMaxExactCoarseDelta = 0.751;
inline constexpr double
    kMirPolicyPartialBarDominantOctaveRelationTolerance = 0.020;
inline constexpr double
    kMirPolicyPartialBarDominantOctaveMinLocalScoreRatio = 1.30;
inline constexpr double kMirPolicyPartialBarExactHalfMinCurrentBpm = 168.0;
inline constexpr double kMirPolicyPartialBarExactHalfMaxRunnerRatio = 0.50;
inline constexpr double kMirPolicyPartialBarExactHalfMinLocalExactScore = 0.22;
inline constexpr double
    kMirPolicyGenericPartialRunnerMaxMeasuredDeltaBpm = 2.0;
inline constexpr double
    kMirPolicyGenericPartialRunnerMinScoreRatio = 0.95;
inline constexpr double
    kMirPolicyGenericPartialRunnerMinMeasuredSupport = 0.99;
inline constexpr double
    kMirPolicyGenericPartialRunnerMinLocalExactScore = 0.35;
inline constexpr double
    kMirPolicyGenericPartialRunnerMinFamilyDominance = 4.0;
inline constexpr double
    kMirPolicyGenericPartialRunnerMinFamilyConsensus = 0.70;
inline constexpr double kMirPolicyHighPulseRetentionMinBpm = 168.0;
inline constexpr double kMirPolicyHighPulseRetentionMaxBpm = 190.0;
inline constexpr double kMirPolicyHighPulseRetentionMinSupport = 0.98;
inline constexpr double kMirPolicyHighPulseRetentionMinWinnerSupport = 0.50;
inline constexpr double kMirPolicyHighPulseRetentionMinLocalExactScore = 0.35;
inline constexpr double kMirPolicyHighPulseRetentionMinPulseScore = 0.35;
inline constexpr double kMirPolicyHighPulseRetentionMinPulseSectionSupport =
    0.80;
inline constexpr double kMirPolicyHighPulseRetentionMaxHalfLocalScoreRatio =
    0.75;
inline constexpr bool kEnableMirPolicyPartialBarTopKExactRelease = true;
inline constexpr double kMirPolicyPartialBarTopKRelationTolerance = 0.020;
inline constexpr double kMirPolicyPartialBarTopKRationalMaxCenterZ = 1.00;
inline constexpr double kMirPolicyPartialBarTopKRationalMinExactScore = 0.175;
inline constexpr double kMirPolicyPartialBarTopKRationalMinScoreRatio = 0.50;
inline constexpr double kMirPolicyPartialBarTopKRationalMinExactToMax = 0.95;
inline constexpr double kMirPolicyPartialBarTopKCenterMaxCenterZ = 0.050;
inline constexpr double kMirPolicyPartialBarTopKCenterMinExactScore = 0.175;
inline constexpr double kMirPolicyPartialBarTopKCenterMinScoreRatio = 0.50;
inline constexpr double kMirPolicyPartialBarTopKCenterMinExactToMax = 0.95;
inline constexpr double kMirPolicyPartialBarTopKMinSegmentSupport = 1.00;
inline constexpr std::size_t kMirPolicyPartialBarTopKMinTop5Hits = 1;
inline constexpr double kMirPolicyCrossViewMinFamilyRatio = 1.55;
inline constexpr double kMirPolicyCrossViewMaxFamilyRatio = 1.65;
inline constexpr double kMirPolicyCrossViewMinScoreRatio = 0.80;
inline constexpr double kMirPolicyCrossViewMinTop5Support = 0.25;
inline constexpr double kMirPolicyCrossViewMinSegmentSupport = 0.90;
inline constexpr double kMirPolicyCrossViewMinLocalExactScore = 0.20;
inline constexpr double kMirPolicyCrossViewMinCandidateSupport = 0.80;
inline constexpr double kMirPolicyCrossViewMinContinuousScore = 0.20;
inline constexpr double kMirPolicyCrossViewMinContinuousSupport = 0.80;
inline constexpr double kMirPolicyCrossViewMaxCoarseBridgeBpm = 1.25;
inline constexpr double kMirPolicyBroadCrossViewMinBpmDelta = 15.0;
inline constexpr double kMirPolicyBroadCrossViewThreeQuarterTolerance =
    0.01875;
inline constexpr double kMirPolicyBroadCrossViewThreeQuarterMinLocalRatio =
    1.30;
inline constexpr double kMirPolicyBroadCrossViewPartialMaxDelta = 0.25;
inline constexpr double kMirPolicyBroadCrossViewPartialMinMargin = 0.05;
inline constexpr double kMirPolicyBroadCrossViewPartialMinLocalRatio = 1.40;
inline constexpr double kMirPolicyBroadCrossViewPartialMinRank1Support = 0.90;
inline constexpr double kMirPolicyBroadCrossViewPartialMaxRunnerRatio = 0.50;
inline constexpr double kMirPolicyBroadCrossViewOctaveMaxDelta = 0.50;
inline constexpr double kMirPolicyBroadCrossViewOctaveMinLocalRatio = 1.30;
inline constexpr double kMirPolicyBroadCrossViewOctaveRelationTolerance = 0.02;

struct MirRoutingContext {
  const char* source_genres = nullptr;
  const char* normalized_genres = nullptr;
  const char* matched_token = nullptr;
  bool route_matched = false;
  bool is_generic_match = false;
};

struct AliasPhaseMetrics {
  double phase_vs = 0.0;
  double axial_phase_vs = 0.0;
  double normalized_onset_density = 0.0;
};

struct MirLocalExactBpm {
  double bpm = 0.0;
  double score = 0.0;
};

class MirLocalExactCache {
 public:
  MirLocalExactCache(std::span<const uint64_t> sortedMirPeaks,
                        double sampleRate);

  [[nodiscard]] MirLocalExactBpm measure(double clusterBpm);
  [[nodiscard]] std::size_t result_hits() const noexcept {
    return m_resultHits;
  }
  [[nodiscard]] std::size_t result_misses() const noexcept {
    return m_resultByClusterBits.size();
  }
  [[nodiscard]] std::size_t score_hits() const noexcept {
    return m_scoreHits;
  }
  [[nodiscard]] std::size_t score_misses() const noexcept {
    return m_scoreByBpmBits.size();
  }

 private:
  std::span<const uint64_t> m_sortedMirPeaks;
  double m_sampleRate = 0.0;
  uint64_t m_onsetSpanSamples = 0;
  std::unordered_map<uint64_t, double> m_scoreByBpmBits;
  std::unordered_map<uint64_t, MirLocalExactBpm> m_resultByClusterBits;
  std::size_t m_resultHits = 0;
  std::size_t m_scoreHits = 0;
};

enum class MirAlias : uint16_t {
  quarter = 1u << 0,
  one_third = 1u << 1,
  half = 1u << 2,
  two_thirds = 1u << 3,
  three_quarters = 1u << 4,
  direct = 1u << 5,
  four_thirds = 1u << 6,
  three_halves = 1u << 7,
  double_time = 1u << 8,
  triple = 1u << 9,
  quadruple = 1u << 10,
};

struct MirRawCandidate {
  double bpm = 0.0;
  const char* alias_class = "invalid";
  uint16_t alias_bit = 0;
  std::vector<uint8_t> segments;
  std::vector<uint8_t> winner_segments;
  std::size_t segment_hits = 0;
  std::size_t winner_segment_hits = 0;
  double score_sum = 0.0;
  double quant_sum = 0.0;
  double meter_sum = 0.0;
  double best_combined_score = 0.0;
  double best_quantization_score = 0.0;
  double best_meter_score = 0.0;
  std::size_t rows = 0;
  std::size_t winner_rows = 0;
  double synthetic_support = 0.0;
  double synthetic_winner_support = 0.0;
  double pulse_score = 0.0;
  double pulse_section_support = 0.0;
  double pulse_phase_vs = 0.0;
  double pulse_axial_phase_vs = 0.0;
  double continuous_score = 0.0;
  double continuous_section_support = 0.0;
  double continuous_score_ratio = 0.0;
  double continuous_section_stability = 0.0;
};

struct MirCandidate {
  double bpm = 0.0;
  uint16_t alias_bits = 0;
  double support = 0.0;
  double winner_support = 0.0;
  double score_sum = 0.0;
  double best_combined_score = 0.0;
  double avg_quant = 0.0;
  double pulse_score = 0.0;
  double pulse_section_support = 0.0;
  double pulse_phase_vs = 0.0;
  double pulse_axial_phase_vs = 0.0;
  double continuous_score = 0.0;
  double continuous_section_support = 0.0;
  double continuous_score_ratio = 0.0;
  double continuous_section_stability = 0.0;
  std::size_t rows = 0;
};

struct MirCandidateBoard {
  std::vector<MirRawCandidate> raw_candidates;
  std::unordered_map<uint64_t, std::size_t> raw_candidate_index;
  std::size_t segment_count = 0;
};

struct MirPolicyMeasuredCandidate {
  MirCandidate candidate;
  double cluster_bpm = 0.0;
  double bpm = 0.0;
  double local_exact_score = 0.0;
  double base_score = 0.0;
  std::string alias_classes;
};

struct MirManualReviewCandidate {
  double bpm = 0.0;
  double evidence_score = 0.0;
  double support = 0.0;
  double winner_support = 0.0;
  double local_exact_score = 0.0;
  std::size_t evidence_rank = 0;
  std::string alias_classes;
  std::string sources;
};

struct MirLaneStats {
  double best_support = 0.0;
  double high_direct_support = 0.0;
  double low_support = 0.0;
  double mid_support = 0.0;
  double high_support = 0.0;
};

[[nodiscard]] double mir_alias_multiplier(const char* aliasClass) noexcept;
[[nodiscard]] std::size_t mir_segment_hit_count(const std::vector<uint8_t>& flags) noexcept;
[[nodiscard]] uint64_t mir_segment_mask(const std::vector<uint8_t>& flags) noexcept;
[[nodiscard]] double cluster_material_bpm(double bpm) noexcept;
[[nodiscard]] bool mir_alias_is_production(uint16_t bit) noexcept;
[[nodiscard]] bool mir_alias_is_extreme(uint16_t bit) noexcept;
[[nodiscard]] bool mir_has_alias(uint16_t bits, MirAlias alias) noexcept;
[[nodiscard]] std::string mir_alias_text(uint16_t bits);
[[nodiscard]] bool contains_ascii_case_insensitive(const char* haystack, const char* needle) noexcept;

void initialize_mir_candidate_board(MirCandidateBoard& board, std::size_t segmentCount);
void add_mir_topk_candidate(MirCandidateBoard& board, const HodgkinsonFullMirSegmentCandidate& candidate);
void add_mir_alias_pulse_evidence(MirCandidateBoard& board, const HodgkinsonAliasPulseEvidence& evidence);
void add_mir_continuous_refinement_evidence(MirCandidateBoard& board, const HodgkinsonContinuousRefinementEvidence& evidence);

[[nodiscard]] MirHighPulseIntervalConflictShadow evaluate_mir_high_pulse_interval_conflict_shadow(
    const HodgkinsonContinuousRefinementEvidence& evidence,
    double runtimeBpm,
    bool runtimeAvailable,
    bool reviewHoldContext);
[[nodiscard]] std::vector<MirCandidate> collapse_mir_candidates(const MirCandidateBoard& board);
[[nodiscard]] double mir_raw_support(const MirRawCandidate& candidate, std::size_t segmentCount) noexcept;
[[nodiscard]] double mir_raw_winner_support(const MirRawCandidate& candidate, std::size_t segmentCount) noexcept;
[[nodiscard]] bool is_mir_soft_center_alias_review_hold(const MirPrimarySelection& shadow) noexcept;
[[nodiscard]] double compute_mir_diagnostic_confidence(const MirPrimarySelection& shadow) noexcept;
[[nodiscard]] MirPrimarySelection compute_mir_primary_selection(
    const MirCandidateBoard& board,
    const HodgkinsonTatumProbeResult& result,
    const HodgkinsonMaterialRiskEvidence& materialRisk,
    const MirRoutingContext* routingContext,
    double routeMin,
    double routeMax,
    bool enabled);
[[nodiscard]] MirPrimarySelection compute_mir_dsp_selection(
    const MirCandidateBoard& board,
    const HodgkinsonTatumProbeResult& result,
    const HodgkinsonMaterialRiskEvidence& materialRisk,
    bool enabled);
[[nodiscard]] MirPrimarySelection compute_mir_soft_center_selection(
    const MirCandidateBoard& board,
    const HodgkinsonTatumProbeResult& result,
    const HodgkinsonMaterialRiskEvidence& materialRisk,
    double priorMinBpm,
    double priorMaxBpm,
    bool routeMatched,
    bool genericRoute,
    bool enabled);
[[nodiscard]] MirPolicyDecision
compute_mir_policy_decision(
    std::span<const MirPolicyMeasuredCandidate> candidates,
    const MirPrimarySelection& current,
    const HodgkinsonTatumProbeResult& primaryFamily,
    const HodgkinsonMaterialRiskLocalExactProbe* materialRiskProbe,
    const HodgkinsonPartialBarAggregateShadow* partialBarAggregate,
    double priorCenterBpm,
    double priorSpreadBpm,
    bool routeMatched,
    bool genericRoute,
    bool enabled);
bool try_apply_mir_policy_partial_bar_topk_exact_release(
    MirPolicyDecision& decision,
    std::span<const HodgkinsonPartialBarTopKExactCandidate> candidates,
    double priorCenterBpm,
    double priorSpreadBpm,
    bool routeMatched,
    bool genericRoute,
    bool enabled) noexcept;
[[nodiscard]] bool is_mir_policy_partial_bar_cross_view_candidate(
    const MirPolicyDecision& decision,
    const MirPolicyMeasuredCandidate& candidate) noexcept;
[[nodiscard]] bool has_mir_policy_partial_bar_cross_view_candidate(
    const MirPolicyDecision& decision,
    std::span<const MirPolicyMeasuredCandidate> fullBoardCandidates,
    bool enabled) noexcept;
bool try_apply_mir_policy_partial_bar_cross_view_release(
    MirPolicyDecision& decision,
    std::span<const MirPolicyMeasuredCandidate> fullBoardCandidates,
    std::span<const HodgkinsonPartialBarTopKExactCandidate> partialBarTopK,
    bool enabled) noexcept;
[[nodiscard]] bool is_mir_policy_keep_current_micro_upgrade(
    const MirPolicyDecision& decision) noexcept;
[[nodiscard]] double effective_mir_policy_output_bpm(
    const MirPolicyDecision& decision) noexcept;
bool enforce_mir_measured_candidate_invariant(
    MirPolicyDecision& decision,
    std::span<const MirPolicyMeasuredCandidate> fullBoardCandidates,
    const MirPrimarySelection& primarySelection,
    const HodgkinsonMaterialRiskLocalExactProbe* materialRiskProbe,
    const HodgkinsonPartialBarAggregateShadow* partialBarAggregate,
    std::span<const HodgkinsonPartialBarTopKExactCandidate>
        partialBarTopK) noexcept;
[[nodiscard]] bool mir_output_has_measured_candidate_witness(
    double outputBpm,
    std::span<const MirPolicyMeasuredCandidate> fullBoardCandidates,
    const MirPrimarySelection& primarySelection,
    const HodgkinsonMaterialRiskLocalExactProbe* materialRiskProbe,
    const HodgkinsonPartialBarAggregateShadow* partialBarAggregate,
    std::span<const HodgkinsonPartialBarTopKExactCandidate>
        partialBarTopK) noexcept;
[[nodiscard]] std::vector<MirManualReviewCandidate>
prepare_mir_manual_review_candidates(
    std::span<const MirPolicyMeasuredCandidate> fullBoardCandidates,
    const MirPrimarySelection& primarySelection,
    const HodgkinsonMaterialRiskLocalExactProbe* materialRiskProbe,
    const HodgkinsonPartialBarAggregateShadow* partialBarAggregate,
    std::span<const HodgkinsonPartialBarTopKExactCandidate>
        partialBarTopK);
[[nodiscard]] bool
is_mir_policy_material_risk_local_exact_release_candidate(
    const HodgkinsonMaterialRiskLocalExactProbe& probe) noexcept;
[[nodiscard]] bool try_apply_mir_review_hold_alias_release_v0(
    MirPrimarySelection& shadow,
    const HodgkinsonTatumProbeResult& result) noexcept;
[[nodiscard]] AliasPhaseMetrics compute_alias_phase_metrics(
    std::span<const uint64_t> onsets,
    double aliasBpm,
    double sampleRate) noexcept;
[[nodiscard]] double compute_continuous_refinement_score(
    const AliasPhaseMetrics& phaseMetrics) noexcept;
[[nodiscard]] MirLocalExactBpm measure_mir_local_exact_bpm(
    std::span<const uint64_t> sortedMirPeaks,
    double clusterBpm,
    double sampleRate);
[[nodiscard]] std::vector<MirPolicyMeasuredCandidate>
prepare_mir_policy_fullboard_candidates(
    const MirCandidateBoard& board,
    MirLocalExactCache& localExactCache);
void apply_mir_local_exact_result(
    MirPrimarySelection& shadow,
    double clusterBpm,
    const MirLocalExactBpm& exact);
void apply_mir_local_exact_measurement(
    MirPrimarySelection& shadow,
    MirLocalExactCache& localExactCache);
void finalize_mir_selector_shadow(
    MirPrimarySelection& shadow,
    const HodgkinsonTatumProbeResult& result,
    MirLocalExactCache& localExactCache);

}  // namespace smart_tempo::mir_pipeline
