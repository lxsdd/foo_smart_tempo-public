#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace smart_tempo {

struct HodgkinsonTatumPeak {
  std::size_t odf_index = 0;
  double weight = 0.0;
};

struct HodgkinsonTatumProbeInput {
  bool enabled = false;
  bool license_gate_cleared = false;
  double input_bpm = 0.0;
  double sample_rate = 0.0;
  std::size_t segment_index = 0;
  std::size_t segment_count = 0;
  double segment_start_sec = 0.0;
  double segment_duration_sec = 0.0;
  std::size_t onset_count = 0;
  std::size_t odf_peak_count = 0;
};

struct HodgkinsonTatumProbeResult {
  bool enabled = false;
  bool candidate = false;
  const char* frontend_source = "smart_onsets";
  double input_bpm = 0.0;
  double candidate_bpm = 0.0;
  double loop_fit_score = 0.0;
  double loop_fit_confidence = 0.0;
  double family_bpm = 0.0;
  double family_score = 0.0;
  double candidate_ratio_to_input = 0.0;
  double family_ratio_to_input = 0.0;
  double runner_up_family_bpm = 0.0;
  double runner_up_family_score = 0.0;
  std::size_t runner_up_family_support = 0;
  std::size_t runner_up_family_candidate_support = 0;
  std::size_t family_rank_count = 0;
  std::array<double, 4> family_rank_bpm{};
  std::array<double, 4> family_rank_score{};
  std::array<std::size_t, 4> family_rank_support{};
  std::array<std::size_t, 4> family_rank_candidate_support{};
  std::size_t tatum_count = 0;
  std::size_t segment_index = 0;
  // Legacy aggregate count. For aggregate rows this historically represented
  // score-positive fit segments, not necessarily strict candidate=1 segments.
  std::size_t segment_count = 0;
  std::size_t total_segment_count = 0;
  std::size_t fit_segment_count = 0;
  std::size_t candidate_segment_count = 0;
  std::size_t consensus_support = 0;
  std::size_t candidate_consensus_support = 0;
  std::size_t onset_count = 0;
  std::size_t odf_peak_count = 0;
  double segment_start_sec = 0.0;
  double segment_duration_sec = 0.0;
  const char* meter = "unknown";
  const char* family_class = "not_evaluated";
  const char* candidate_ratio_class = "invalid";
  const char* family_ratio_class = "invalid";
  const char* reason = "not_evaluated";
};

struct HodgkinsonFamilyAliasOption {
  double bpm = 0.0;
  double multiplier = 1.0;
  const char* multiplier_class = "invalid";
};

struct HodgkinsonAliasPulseEvidence {
  bool enabled = false;
  bool route_valid = false;
  std::size_t family_rank = 0;
  double family_bpm = 0.0;
  double family_score = 0.0;
  std::size_t family_support = 0;
  std::size_t family_candidate_support = 0;
  double alias_bpm = 0.0;
  double alias_multiplier = 1.0;
  const char* alias_class = "invalid";
  double phase_vs = 0.0;
  double axial_phase_vs = 0.0;
  double beat_salience = 0.0;
  double tatum_salience = 0.0;
  double beat_tatum_ratio = 0.0;
  double normalized_onset_density = 0.0;
  double section_support = 0.0;
  double section_stability = 0.0;
  double pulse_score = 0.0;
  const char* reason = "not_evaluated";
};

struct HodgkinsonContinuousRefinementEvidence {
  bool enabled = false;
  bool route_valid = false;
  std::size_t family_rank = 0;
  double family_bpm = 0.0;
  double family_score = 0.0;
  std::size_t family_support = 0;
  double alias_bpm = 0.0;
  double alias_multiplier = 1.0;
  const char* alias_class = "invalid";
  double scan_start_bpm = 0.0;
  double scan_end_bpm = 0.0;
  double scan_step_bpm = 0.0;
  std::size_t scan_count = 0;
  double base_score = 0.0;
  double best_bpm = 0.0;
  double best_score = 0.0;
  double score_delta = 0.0;
  double score_ratio = 0.0;
  double runner_up_bpm = 0.0;
  double runner_up_score = 0.0;
  double peak_separation_bpm = 0.0;
  double best_phase_vs = 0.0;
  double best_axial_phase_vs = 0.0;
  double best_density = 0.0;
  double section_support = 0.0;
  double section_stability = 0.0;
  std::size_t odf_peak_count = 0;
  std::size_t accepted_segment_count = 0;
  double peak1_bpm = 0.0;
  double peak1_score = 0.0;
  double peak2_bpm = 0.0;
  double peak2_score = 0.0;
  double peak3_bpm = 0.0;
  double peak3_score = 0.0;
  double fine_scan_start_bpm = 0.0;
  double fine_scan_end_bpm = 0.0;
  double fine_scan_step_bpm = 0.0;
  std::size_t fine_scan_count = 0;
  double fine_best_bpm = 0.0;
  double fine_best_score = 0.0;
  double fine_score_delta = 0.0;
  double fine_score_ratio = 0.0;
  std::size_t fine_segment_count = 0;
  double fine_segment_median_bpm = 0.0;
  double fine_segment_q25_bpm = 0.0;
  double fine_segment_q75_bpm = 0.0;
  double fine_segment_iqr_bpm = 0.0;
  double fine_segment_min_bpm = 0.0;
  double fine_segment_max_bpm = 0.0;
  double fine_segment_support_005 = 0.0;
  double fine_segment_support_010 = 0.0;
  std::size_t range_segment_count = 0;
  double range_segment_median_bpm = 0.0;
  double range_segment_q25_bpm = 0.0;
  double range_segment_q75_bpm = 0.0;
  double range_segment_iqr_bpm = 0.0;
  double range_segment_min_bpm = 0.0;
  double range_segment_max_bpm = 0.0;
  double range_segment_support_best_050 = 0.0;
  double range_segment_support_alias_050 = 0.0;
  double range_segment_cluster1_bpm = 0.0;
  double range_segment_cluster1_support = 0.0;
  double range_segment_cluster2_bpm = 0.0;
  double range_segment_cluster2_support = 0.0;
  double range_segment_cluster3_bpm = 0.0;
  double range_segment_cluster3_support = 0.0;
  double interval_scan_start_bpm = 0.0;
  double interval_scan_end_bpm = 0.0;
  double interval_scan_step_bpm = 0.0;
  std::size_t interval_scan_count = 0;
  double interval_best_bpm = 0.0;
  double interval_best_score = 0.0;
  double interval_alias_score = 0.0;
  double interval_continuous_best_score = 0.0;
  double interval_peak1_bpm = 0.0;
  double interval_peak1_score = 0.0;
  double interval_peak2_bpm = 0.0;
  double interval_peak2_score = 0.0;
  double interval_peak3_bpm = 0.0;
  double interval_peak3_score = 0.0;
  double interval_fine_scan_start_bpm = 0.0;
  double interval_fine_scan_end_bpm = 0.0;
  double interval_fine_scan_step_bpm = 0.0;
  std::size_t interval_fine_scan_count = 0;
  double interval_fine_best_bpm = 0.0;
  double interval_fine_best_score = 0.0;
  std::size_t interval_segment_count = 0;
  double interval_segment_median_bpm = 0.0;
  double interval_segment_q25_bpm = 0.0;
  double interval_segment_q75_bpm = 0.0;
  double interval_segment_iqr_bpm = 0.0;
  double interval_segment_min_bpm = 0.0;
  double interval_segment_max_bpm = 0.0;
  double interval_segment_support_best_050 = 0.0;
  double interval_segment_support_alias_050 = 0.0;
  double interval_segment_cluster1_bpm = 0.0;
  double interval_segment_cluster1_support = 0.0;
  double interval_segment_cluster2_bpm = 0.0;
  double interval_segment_cluster2_support = 0.0;
  double interval_segment_cluster3_bpm = 0.0;
  double interval_segment_cluster3_support = 0.0;
  const char* reason = "not_evaluated";
};

struct MirHighPulseIntervalConflictShadow {
  bool enabled = false;
  bool candidate = false;
  bool write_candidate = false;
  bool review_candidate = false;
  bool runtime_available = false;
  bool same_direction = false;
  double runtime_bpm = 0.0;
  double phase_bpm = 0.0;
  double interval_bpm = 0.0;
  double interval_iqr_bpm = 0.0;
  double interval_cluster_support = 0.0;
  double interval_alias_support_050 = 0.0;
  double runtime_interval_delta = 0.0;
  double runtime_phase_delta = 0.0;
  const char* decision_class = "not_evaluated";
  const char* reason = "not_evaluated";
};

struct HodgkinsonMaterialRiskEvidence {
  bool enabled = false;
  bool candidate = false;
  bool route_valid = false;
  bool route_matched = false;
  bool generic_route = false;
  bool review_hold_candidate = false;
  double selected_context_bpm = 0.0;
  double route_min_bpm = 0.0;
  double route_max_bpm = 0.0;
  double route_center_bpm = 0.0;
  std::size_t segment_count = 0;
  double support_108 = 0.0;
  double support_120 = 0.0;
  double support_132 = 0.0;
  double support_135 = 0.0;
  double support_136 = 0.0;
  double support_144 = 0.0;
  double winner_support_144 = 0.0;
  const char* decision_class = "not_evaluated";
  const char* reason = "not_evaluated";
};

struct HodgkinsonMaterialRiskLocalExactProbe {
  bool enabled = false;
  bool candidate = false;
  bool review_hold_candidate = false;
  bool release_candidate = false;
  double selected_context_bpm = 0.0;
  double current_bpm = 0.0;
  double current_local_exact_score = 0.0;
  double scan_start_bpm = 0.0;
  double scan_end_bpm = 0.0;
  double scan_step_bpm = 0.0;
  std::size_t scan_count = 0;
  double best_bpm = 0.0;
  double best_score = 0.0;
  double best_anchor_bpm = 0.0;
  bool best_boundary_hit = false;
  double probe_138_bpm = 0.0;
  double probe_138_score = 0.0;
  bool probe_138_boundary_hit = false;
  double best_score_ratio_to_current = 0.0;
  double probe_138_score_ratio_to_current = 0.0;
  std::size_t target_family_candidate_count = 0;
  double top1_bpm = 0.0;
  double top1_score = 0.0;
  double top1_anchor_bpm = 0.0;
  bool top1_boundary_hit = false;
  double top2_bpm = 0.0;
  double top2_score = 0.0;
  double top2_anchor_bpm = 0.0;
  bool top2_boundary_hit = false;
  double top3_bpm = 0.0;
  double top3_score = 0.0;
  double top3_anchor_bpm = 0.0;
  bool top3_boundary_hit = false;
  std::size_t non_boundary_top_count = 0;
  double top_span_bpm = 0.0;
  std::size_t peak_count = 0;
  double support_136 = 0.0;
  double support_144 = 0.0;
  double winner_support_144 = 0.0;
  const char* decision_class = "not_evaluated";
  const char* reason = "not_evaluated";
};

struct MirPrimarySelection {
  bool enabled = false;
  bool candidate = false;
  bool auto_candidate = false;
  bool review_hold = false;
  bool route_valid = false;
  bool sparse_acapella_review_hold = false;
  bool material_risk_review_hold = false;
  bool high_family_conflict_review_hold = false;
  bool low_pulse_exactness_review_hold = false;
  bool soft_center_conflict_review_hold = false;
  bool soft_center_alias_review_hold = false;
  bool review_hold_alias_release = false;
  double review_hold_alias_release_bpm = 0.0;
  double review_hold_alias_release_candidate_segment_ratio = 0.0;
  double review_hold_alias_release_rank_candidate_support_ratio = 0.0;
  double review_hold_alias_release_runner_up_score_ratio = 0.0;
  double selected_bpm = 0.0;
  double selected_cluster_bpm = 0.0;
  double selected_local_exact_bpm = 0.0;
  double selected_local_exact_score = 0.0;
  double selected_local_exact_delta = 0.0;
  double route_min_bpm = 0.0;
  double route_max_bpm = 0.0;
  double route_center_bpm = 0.0;
  double support = 0.0;
  double winner_support = 0.0;
  double score_sum = 0.0;
  double best_combined_score = 0.0;
  double pulse_score = 0.0;
  double pulse_section_support = 0.0;
  double continuous_score = 0.0;
  double continuous_section_support = 0.0;
  double continuous_score_ratio = 0.0;
  double measured_evidence_score = 0.0;
  double soft_center_bonus = 0.0;
  double soft_center_score = 0.0;
  double soft_center_runner_up_score = 0.0;
  double soft_center_dominance_margin = 0.0;
  bool soft_center_prior_applied = false;
  bool soft_center_high_pulse_veto = false;
  double lane_best_support = 0.0;
  double lane_high_direct_support = 0.0;
  double lane_low_support = 0.0;
  double lane_mid_support = 0.0;
  double lane_high_support = 0.0;
  std::size_t candidate_count = 0;
  std::size_t total_segment_count = 0;
  std::size_t odf_peak_count = 0;
  std::string alias_classes;
  const char* selector_version = "measured_candidate_profile_v1";
  const char* lane = "unknown";
  const char* lane_variant = "none";
  const char* decision_class = "not_evaluated";
  const char* reason = "not_evaluated";
};

struct MirPolicyDecision {
  bool enabled = false;
  bool source_candidate = false;
  bool current_auto_candidate = false;
  bool current_review_hold = false;
  bool protected_review_hold = false;
  bool soft_center_conflict_review_hold = false;
  bool soft_center_alias_review_hold = false;
  bool low_pulse_exactness_review_hold = false;
  bool sparse_acapella_review_hold = false;
  bool writer_override_candidate = false;
  bool review_hold_promotion_candidate = false;
  bool material_risk_local_exact_release_candidate = false;
  bool partial_bar_family_conflict_release_candidate = false;
  bool partial_bar_topk_exact_release_candidate = false;
  bool family_conflict_review_hold_candidate = false;
  bool broad_route_conflict_review_hold_candidate = false;
  bool output_would_write = false;
  bool output_would_review_hold = false;
  bool route_matched = false;
  bool generic_route = false;
  double current_bpm = 0.0;
  double output_bpm = 0.0;
  double prior_min_bpm = 0.0;
  double prior_max_bpm = 0.0;
  double prior_center_bpm = 0.0;
  double prior_spread_bpm = 0.0;
  double measured_winner_bpm = 0.0;
  double measured_winner_cluster_bpm = 0.0;
  double measured_winner_base_score = 0.0;
  double measured_runner_up_base_score = 0.0;
  double measured_base_margin = 0.0;
  double measured_winner_support = 0.0;
  double measured_winner_winner_support = 0.0;
  double measured_winner_local_exact_score = 0.0;
  double current_local_exact_score = 0.0;
  double current_support = 0.0;
  double candidate_bpm = 0.0;
  double candidate_cluster_bpm = 0.0;
  double candidate_base_score = 0.0;
  double candidate_support = 0.0;
  double candidate_winner_support = 0.0;
  double candidate_local_exact_score = 0.0;
  double candidate_pulse_score = 0.0;
  double candidate_pulse_section_support = 0.0;
  double candidate_continuous_score = 0.0;
  double candidate_continuous_section_support = 0.0;
  double candidate_continuous_section_stability = 0.0;
  double candidate_continuous_score_ratio = 0.0;
  double candidate_recovery_evidence_ratio = 0.0;
  double candidate_current_delta = 0.0;
  double candidate_local_score_ratio = 0.0;
  double candidate_support_ratio = 0.0;
  double material_risk_probe_score_ratio = 0.0;
  double material_risk_probe_top_span_bpm = 0.0;
  std::size_t material_risk_probe_target_candidate_count = 0;
  std::size_t material_risk_probe_non_boundary_top_count = 0;
  double harmonic_center_improvement = 0.0;
  double harmonic_score_ratio = 0.0;
  double family_conflict_alternative_bpm = 0.0;
  double family_conflict_center_improvement = 0.0;
  double family_conflict_local_score_ratio = 0.0;
  double family_conflict_base_score_ratio = 0.0;
  double family_conflict_support = 0.0;
  double family_conflict_winner_support = 0.0;
  double family_conflict_local_exact_score = 0.0;
  double family_conflict_pulse_score = 0.0;
  double family_conflict_pulse_section_support = 0.0;
  double family_conflict_continuous_score = 0.0;
  double family_conflict_continuous_section_support = 0.0;
  double family_conflict_continuous_score_ratio = 0.0;
  std::size_t candidate_count = 0;
  std::string measured_winner_alias_classes;
  std::string candidate_alias_classes;
  std::string family_conflict_alias_classes;
  const char* selector_version = "measured_policy_v1";
  const char* action = "none";
  const char* decision_class = "not_evaluated";
  const char* reason = "not_evaluated";
};

struct HodgkinsonOnlyFamilyResolverShadow {
  bool enabled = false;
  bool candidate = false;
  bool input_valid = false;
  double input_bpm = 0.0;
  double route_min_bpm = 0.0;
  double route_max_bpm = 0.0;
  std::size_t route_alias_option_count = 0;
  std::array<HodgkinsonFamilyAliasOption, 11> route_alias_options{};
  double unique_alias_bpm = 0.0;
  double unique_alias_multiplier = 1.0;
  const char* unique_alias_class = "none";
  double center_alias_bpm = 0.0;
  double center_alias_multiplier = 1.0;
  const char* center_alias_class = "none";
  double center_distance_bpm = 0.0;
  double family_score = 0.0;
  double family_score_dominance = 0.0;
  double runner_up_score_ratio = 0.0;
  double consensus_support = 0.0;
  const char* decision_class = "not_evaluated";
  const char* reason = "not_evaluated";
};

struct HodgkinsonTatumSegmentInput {
  bool enabled = false;
  bool license_gate_cleared = false;
  double input_bpm = 0.0;
  double segment_start_sec = 0.0;
  double segment_duration_sec = 0.0;
  std::size_t segment_index = 0;
  std::size_t segment_count = 0;
  std::size_t odf_sample_count = 0;
  std::span<const HodgkinsonTatumPeak> odf_peaks;
};

[[nodiscard]] HodgkinsonTatumProbeResult evaluate_hodgkinson_tatum_probe_gate(
    const HodgkinsonTatumProbeInput& input) noexcept;

[[nodiscard]] HodgkinsonTatumProbeResult
evaluate_hodgkinson_tatum_segment_window(
    const HodgkinsonTatumSegmentInput& input) noexcept;

[[nodiscard]] HodgkinsonOnlyFamilyResolverShadow
evaluate_hodgkinson_only_family_resolver_shadow(
    const HodgkinsonTatumProbeResult& result,
    double routeMinBpm,
    double routeMaxBpm) noexcept;

[[nodiscard]] std::vector<HodgkinsonFamilyAliasOption>
route_valid_hodgkinson_family_aliases(double familyBpm,
                                      double routeMinBpm,
                                      double routeMaxBpm);

}  // namespace smart_tempo
