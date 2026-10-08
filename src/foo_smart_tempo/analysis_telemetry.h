#pragma once

#include "analysis_decision_support.h"
#include "ibpm_analyzer.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace smart_tempo {

struct HodgkinsonTatumProbeResult;
struct HodgkinsonFullMirSegmentCandidate;
struct HodgkinsonPartialBarAggregateShadow;
struct HodgkinsonPartialBarTopKExactCandidate;
struct HodgkinsonAliasPulseEvidence;
struct HodgkinsonContinuousRefinementEvidence;
struct MirHighPulseIntervalConflictShadow;
struct HodgkinsonMaterialRiskEvidence;
struct HodgkinsonMaterialRiskLocalExactProbe;
struct MirPrimarySelection;
struct MirPolicyDecision;
struct HodgkinsonOnlyFamilyResolverShadow;

bool verbose_console_logging_enabled() noexcept;

struct RuntimeCounters {
  std::atomic<uint64_t> tempo_allocations{0};
  std::atomic<uint64_t> analyze_calls{0};
  std::atomic<uint64_t> decode_us{0};
  std::atomic<uint64_t> onset_us{0};
  std::atomic<uint64_t> total_us{0};
  std::atomic<uint64_t> hodgkinson_material_risk_candidates{0};
  std::atomic<uint64_t> hodgkinson_material_risk_review_holds{0};
  std::atomic<uint64_t> mir_primary_auto_candidates{0};
  std::atomic<uint64_t> mir_primary_review_holds{0};
  std::atomic<uint64_t> mir_primary_sparse_acapella_review_holds{0};
  std::atomic<uint64_t> mir_primary_runtime_applied{0};
  std::atomic<uint64_t> mir_policy_writer_overrides{0};
  std::atomic<uint64_t> mir_policy_review_hold_promotions{0};
  std::atomic<uint64_t> mir_policy_keep_current_micro_upgrades{0};
  std::atomic<uint64_t> mir_policy_review_holds{0};
  std::atomic<uint64_t> mir_policy_family_conflict_review_holds{0};
};

struct RuntimeStatsSnapshot {
  uint64_t tempo_allocations = 0;
  uint64_t analyze_calls = 0;
  uint64_t hodgkinson_material_risk_candidates = 0;
  uint64_t hodgkinson_material_risk_review_holds = 0;
  uint64_t mir_primary_auto_candidates = 0;
  uint64_t mir_primary_review_holds = 0;
  uint64_t mir_primary_sparse_acapella_review_holds = 0;
  uint64_t mir_primary_runtime_applied = 0;
  uint64_t mir_policy_writer_overrides = 0;
  uint64_t mir_policy_review_hold_promotions = 0;
  uint64_t mir_policy_keep_current_micro_upgrades = 0;
  uint64_t mir_policy_review_holds = 0;
  uint64_t mir_policy_family_conflict_review_holds = 0;
  double decode_seconds = 0.0;
  double onset_seconds = 0.0;
  double total_seconds = 0.0;
};

class AnalyzeMetricsScope {
 public:
  bool stageProfiling = false;
  double decodeSec = 0.0;
  double onsetSec = 0.0;

  explicit AnalyzeMetricsScope(bool stageProfiling);
  ~AnalyzeMetricsScope();

  [[nodiscard]] double elapsed_seconds() const noexcept;

  template <typename Work>
  bool run_decode_work(Work&& work) {
    if (!stageProfiling) {
      return std::forward<Work>(work)();
    }

    const double onsetBefore = onsetSec;
    pfc::hires_timer decodeTimer;
    decodeTimer.start();
    const bool result = std::forward<Work>(work)();
    const double elapsed = decodeTimer.query();
    const double onsetInside = onsetSec - onsetBefore;
    decodeSec += (std::max)(0.0, elapsed - onsetInside);
    return result;
  }

  template <typename Work>
  void run_onset_work(Work&& work) {
    if (!stageProfiling) {
      std::forward<Work>(work)();
      return;
    }

    pfc::hires_timer onsetTimer;
    onsetTimer.start();
    std::forward<Work>(work)();
    onsetSec += onsetTimer.query();
  }

 private:
  pfc::hires_timer totalTimer_;
};

struct DecisionSummaryLogSnapshot {
  bool rawGlobalValid = false;
  double rawGlobalBpm = 0.0;
  double rawProjectedBpm = 0.0;
  double policyInputBpm = 0.0;
  double projectedBpm = 0.0;
  double finalBpm = 0.0;
  double targetMinBpm = 0.0;
  double targetMaxBpm = 0.0;
  double confidence = 0.0;
  const char* routeName = "";
  const char* policyPathText = "";
  const IBpmAnalyzer::RoutingLogContext* routingContext = nullptr;
  FoldingResult projected;
  PolicyProjectionReason policyReason = PolicyProjectionReason::none;
};

struct TrackTimingLogSnapshot {
  double decodeSec = 0.0;
  double onsetSec = 0.0;
  double totalSec = 0.0;
};

TrackTimingLogSnapshot make_track_timing_log(
    double decodeSec,
    double onsetSec,
    double totalSec) noexcept;

void log_analysis_sample_rate(unsigned sourceSampleRate,
                              unsigned analysisSampleRate,
                              unsigned maxAnalysisRate,
                              bool resampled,
                              double dynamicMinIoiSeconds,
                              uint64_t dynamicMinIoiSamples);
void log_telemetry_header(const char* trackLabel,
                          unsigned inputSampleRate,
                          unsigned analysisSampleRate,
                          unsigned winSize,
                          unsigned hopSize,
                          double dynamicMinIoiSeconds,
                          uint64_t dynamicMinIoiSamples);
void log_decision_summary(const char* trackLabel,
                          const DecisionSummaryLogSnapshot& snapshot);
void log_standard_decision(const char* trackLabel,
                           const DecisionSummaryLogSnapshot& snapshot,
                           bool uncertainResult,
                           const char* decisionClass);
void log_track_timing(const char* trackLabel,
                      const TrackTimingLogSnapshot& snapshot);
void log_hodgkinson_tatum_probe(const char* trackLabel, uint64_t trackKey,
                                 const HodgkinsonTatumProbeResult& result);
void log_hodgkinson_primary_segment(
    const char* trackLabel,
    const HodgkinsonTatumProbeResult& result);
void log_hodgkinson_primary_segment_candidate(
    const char* trackLabel, uint64_t trackKey,
    const HodgkinsonFullMirSegmentCandidate& candidate);
void log_hodgkinson_partial_bar_aggregate_shadow(
    const char* trackLabel, uint64_t trackKey,
    const HodgkinsonPartialBarAggregateShadow& shadow);
void log_hodgkinson_partial_bar_topk_exact_candidate(
    const char* trackLabel, uint64_t trackKey,
    const HodgkinsonPartialBarTopKExactCandidate& candidate);
void log_hodgkinson_primary_candidate_set(
    const char* trackLabel,
    const HodgkinsonTatumProbeResult& result);
void log_hodgkinson_alias_pulse_evidence(
    const char* trackLabel,
    const HodgkinsonAliasPulseEvidence& evidence);
void log_hodgkinson_continuous_refinement_evidence(
    const char* trackLabel,
    const HodgkinsonContinuousRefinementEvidence& evidence);
void log_mir_high_pulse_interval_conflict_shadow(
    const char* trackLabel,
    const MirHighPulseIntervalConflictShadow& shadow);
void log_hodgkinson_material_risk_evidence(
    const char* trackLabel,
    const HodgkinsonMaterialRiskEvidence& evidence);
void log_hodgkinson_material_risk_local_exact_probe(
    const char* trackLabel,
    const HodgkinsonMaterialRiskLocalExactProbe& probe);
void log_mir_primary_selection(
    const char* trackLabel,
    const MirPrimarySelection& shadow);
void log_mir_cluster_candidate_board_entry(
    const char* trackLabel, std::size_t index, double bpm,
    const char* aliasClasses, double support, double winnerSupport,
    double scoreSum, double bestCombinedScore, double pulseScore,
    double pulseSectionSupport, double pulsePhaseVs, double pulseAxialPhaseVs,
    double continuousScore, double continuousSectionSupport,
    double continuousScoreRatio, double continuousSectionStability,
    std::size_t rows, std::size_t totalSegmentCount);
void log_mir_raw_alias_board_entry(
    const char* trackLabel, uint64_t trackKey, std::size_t index, double bpm,
    double localExactBpm, double localExactScore, double localExactDelta,
    const char* aliasClass, double aliasMultiplier, double sourceBpm,
    double support, double winnerSupport, std::size_t segmentHits,
    std::size_t winnerHits, uint64_t segmentMask, uint64_t winnerMask,
    double scoreSum, double avgQuant, double avgMeter,
    double bestCombinedScore, double bestQuantizationScore,
    double bestMeterScore, std::size_t rows, std::size_t winnerRows,
    double syntheticSupport, double syntheticWinnerSupport,
    double pulseScore, double pulseSectionSupport, double pulsePhaseVs,
    double pulseAxialPhaseVs, double continuousScore,
    double continuousSectionSupport, double continuousScoreRatio,
    double continuousSectionStability, std::size_t totalSegmentCount,
    bool maskTruncated);
void log_mir_candidate_board_entry(
    const char* trackLabel, std::size_t index, double bpm,
    double localExactBpm, double localExactScore,
    double localExactDelta, const char* aliasClasses, double support,
    double winnerSupport,
    double scoreSum, double bestCombinedScore, double pulseScore,
    double pulseSectionSupport, double pulsePhaseVs, double pulseAxialPhaseVs,
    double continuousScore, double continuousSectionSupport,
    double continuousScoreRatio, double continuousSectionStability,
    std::size_t rows, std::size_t totalSegmentCount);
void log_mir_policy_candidate_board_entry(
    const char* trackLabel, uint64_t trackKey, std::size_t index,
    const char* origin, double clusterBpm, double localExactBpm,
    double localExactScore, double baseScore, const char* aliasClasses,
    double support, double winnerSupport, double scoreSum,
    double bestCombinedScore, double pulseScore, double pulseSectionSupport,
    double pulsePhaseVs, double pulseAxialPhaseVs, double continuousScore,
    double continuousSectionSupport, double continuousScoreRatio,
    double continuousSectionStability, std::size_t rows,
    std::size_t totalSegmentCount);
void log_mir_analysis_provenance(const char* trackLabel, uint64_t trackKey,
                                 uint32_t subsongIndex, uint64_t buildId,
                                 const char* policySchema);
void log_mir_dsp_selection(
    const char* trackLabel,
    const MirPrimarySelection& shadow,
    double priorMinBpm, double priorMaxBpm, bool genericRoute);
void log_mir_policy_decision(
    const char* trackLabel,
    const MirPolicyDecision& shadow);
void log_mir_local_exact_cache(const char* trackLabel,
                               std::size_t resultHits,
                               std::size_t resultMisses,
                               std::size_t scoreHits,
                               std::size_t scoreMisses);
void log_hodgkinson_only_family_resolver_shadow(
    const char* trackLabel,
    const HodgkinsonOnlyFamilyResolverShadow& shadow);
RuntimeCounters& runtime_counters() noexcept;
void reset_runtime_counters() noexcept;
RuntimeStatsSnapshot query_runtime_stats_snapshot() noexcept;
uint64_t seconds_to_microseconds(double seconds) noexcept;

}  // namespace smart_tempo
