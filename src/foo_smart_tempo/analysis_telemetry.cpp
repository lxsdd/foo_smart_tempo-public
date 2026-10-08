#include "stdafx.h"

#include "analysis_telemetry.h"
#include "foo_smart_tempo.h"
#include "hodgkinson_full_mir.h"
#include "hodgkinson_tatum_probe.h"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>

namespace smart_tempo {
namespace {
constexpr std::size_t kDecisionRoutingValueMaxChars = 160;

// Fixed-point formatting saturates for BPM-sized values at 17 decimals.
// General-format to_chars preserves a locale-independent double round trip.
pfc::string8 format_double_roundtrip(double value) {
  char buffer[64]{};
  const auto result = std::to_chars(
      std::begin(buffer), std::end(buffer), value, std::chars_format::general,
      std::numeric_limits<double>::max_digits10);
  if (result.ec != std::errc{}) {
    return format_float_locale(value, 15);
  }
  pfc::string8 text;
  text.add_string(buffer, static_cast<t_size>(result.ptr - buffer));
  return text;
}

struct HopQuantizationInfo {
  uint64_t hopBin = 0;
  double hopBinBpm = 0.0;
  double bpmError = 0.0;
};

HopQuantizationInfo compute_hop_quantization(double bpm,
                                             unsigned analysisSampleRate,
                                             unsigned hopSize) {
  HopQuantizationInfo info{};
  if (bpm <= 0.0 || !std::isfinite(bpm) || analysisSampleRate == 0 ||
      hopSize == 0) {
    return info;
  }

  const double hopCount =
      (60.0 * static_cast<double>(analysisSampleRate)) /
      (bpm * static_cast<double>(hopSize));
  if (!std::isfinite(hopCount) || hopCount <= 0.0) {
    return info;
  }

  const double roundedHop = std::round(hopCount);
  const uint64_t nearestHop =
      static_cast<uint64_t>(roundedHop < 1.0 ? 1.0 : roundedHop);
  const double quantizedBpm =
      (60.0 * static_cast<double>(analysisSampleRate)) /
      (static_cast<double>(nearestHop) * static_cast<double>(hopSize));
  if (!std::isfinite(quantizedBpm) || quantizedBpm <= 0.0) {
    return info;
  }

  info.hopBin = nearestHop;
  info.hopBinBpm = quantizedBpm;
  info.bpmError = bpm - quantizedBpm;
  return info;
}

pfc::string8 format_decision_log_value(const char* value) {
  if (value == nullptr || *value == '\0') return pfc::string8("<none>");
  pfc::string8 out;
  std::size_t chars = 0;
  bool truncated = false;
  for (const char* p = value; *p != '\0'; ++p) {
    if (chars >= kDecisionRoutingValueMaxChars) {
      truncated = true;
      break;
    }
    switch (*p) {
      case '\\':
        out << "\\\\";
        break;
      case '"':
        out << "\\\"";
        break;
      case '\r':
      case '\n':
      case '\t':
        out << ' ';
        break;
      default:
        out.add_byte(*p);
        break;
    }
    ++chars;
  }
  if (truncated) out << "...";
  return out;
}
}  // namespace

bool verbose_console_logging_enabled() noexcept {
  return cfg_smart_tempo_verbose_logging != 0;
}

RuntimeCounters& runtime_counters() noexcept {
  static RuntimeCounters counters;
  return counters;
}

void reset_runtime_counters() noexcept {
  auto& counters = runtime_counters();
  counters.tempo_allocations.store(0, std::memory_order_relaxed);
  counters.analyze_calls.store(0, std::memory_order_relaxed);
  counters.decode_us.store(0, std::memory_order_relaxed);
  counters.onset_us.store(0, std::memory_order_relaxed);
  counters.total_us.store(0, std::memory_order_relaxed);
  counters.hodgkinson_material_risk_candidates.store(
      0, std::memory_order_relaxed);
  counters.hodgkinson_material_risk_review_holds.store(
      0, std::memory_order_relaxed);
  counters.mir_primary_auto_candidates.store(
      0, std::memory_order_relaxed);
  counters.mir_primary_review_holds.store(
      0, std::memory_order_relaxed);
  counters.mir_primary_sparse_acapella_review_holds.store(
      0, std::memory_order_relaxed);
  counters.mir_primary_runtime_applied.store(
      0, std::memory_order_relaxed);
  counters.mir_policy_writer_overrides.store(
      0, std::memory_order_relaxed);
  counters.mir_policy_review_hold_promotions.store(
      0, std::memory_order_relaxed);
  counters
      .mir_policy_keep_current_micro_upgrades.store(
          0, std::memory_order_relaxed);
  counters.mir_policy_review_holds.store(
      0, std::memory_order_relaxed);
  counters
      .mir_policy_family_conflict_review_holds.store(
          0, std::memory_order_relaxed);
}

RuntimeStatsSnapshot query_runtime_stats_snapshot() noexcept {
  const auto& counters = runtime_counters();
  RuntimeStatsSnapshot out;
  out.tempo_allocations =
      counters.tempo_allocations.load(std::memory_order_relaxed);
  out.analyze_calls = counters.analyze_calls.load(std::memory_order_relaxed);
  out.hodgkinson_material_risk_candidates =
      counters.hodgkinson_material_risk_candidates.load(
          std::memory_order_relaxed);
  out.hodgkinson_material_risk_review_holds =
      counters.hodgkinson_material_risk_review_holds.load(
          std::memory_order_relaxed);
  out.mir_primary_auto_candidates =
      counters.mir_primary_auto_candidates.load(
          std::memory_order_relaxed);
  out.mir_primary_review_holds =
      counters.mir_primary_review_holds.load(
          std::memory_order_relaxed);
  out.mir_primary_sparse_acapella_review_holds =
      counters
          .mir_primary_sparse_acapella_review_holds.load(
              std::memory_order_relaxed);
  out.mir_primary_runtime_applied =
      counters.mir_primary_runtime_applied.load(
          std::memory_order_relaxed);
  out.mir_policy_writer_overrides =
      counters.mir_policy_writer_overrides.load(
          std::memory_order_relaxed);
  out.mir_policy_review_hold_promotions =
      counters.mir_policy_review_hold_promotions.load(
          std::memory_order_relaxed);
  out.mir_policy_keep_current_micro_upgrades =
      counters
          .mir_policy_keep_current_micro_upgrades.load(
              std::memory_order_relaxed);
  out.mir_policy_review_holds =
      counters.mir_policy_review_holds.load(
          std::memory_order_relaxed);
  out.mir_policy_family_conflict_review_holds =
      counters
          .mir_policy_family_conflict_review_holds.load(
              std::memory_order_relaxed);
  out.decode_seconds =
      static_cast<double>(counters.decode_us.load(std::memory_order_relaxed)) /
      1000000.0;
  out.onset_seconds =
      static_cast<double>(counters.onset_us.load(std::memory_order_relaxed)) /
      1000000.0;
  out.total_seconds =
      static_cast<double>(counters.total_us.load(std::memory_order_relaxed)) /
      1000000.0;
  return out;
}

uint64_t seconds_to_microseconds(double seconds) noexcept {
  if (!(seconds > 0.0) || !std::isfinite(seconds)) return 0;
  const double us = seconds * 1000000.0;
  if (us >= static_cast<double>((std::numeric_limits<uint64_t>::max)())) {
    return (std::numeric_limits<uint64_t>::max)();
  }
  return static_cast<uint64_t>(us + 0.5);
}

AnalyzeMetricsScope::AnalyzeMetricsScope(bool p_stageProfiling)
    : stageProfiling(p_stageProfiling) {
  totalTimer_.start();
}

AnalyzeMetricsScope::~AnalyzeMetricsScope() {
  auto& counters = runtime_counters();
  counters.analyze_calls.fetch_add(1, std::memory_order_relaxed);
  if (stageProfiling) {
    counters.decode_us.fetch_add(seconds_to_microseconds(decodeSec),
                                 std::memory_order_relaxed);
    counters.onset_us.fetch_add(seconds_to_microseconds(onsetSec),
                                std::memory_order_relaxed);
  }
  counters.total_us.fetch_add(seconds_to_microseconds(totalTimer_.query()),
                              std::memory_order_relaxed);
}

double AnalyzeMetricsScope::elapsed_seconds() const noexcept {
  return totalTimer_.query();
}

TrackTimingLogSnapshot make_track_timing_log(
    double decodeSec,
    double onsetSec,
    double totalSec) noexcept {
  TrackTimingLogSnapshot out;
  out.decodeSec = decodeSec;
  out.onsetSec = onsetSec;
  out.totalSec = totalSec;
  return out;
}

void log_hodgkinson_tatum_probe(const char* trackLabel, uint64_t trackKey,
                                 const HodgkinsonTatumProbeResult& result) {
  const double confidence = result.loop_fit_confidence;
  const double score = result.loop_fit_score;
  const std::size_t totalSegmentCount =
      result.total_segment_count > 0 ? result.total_segment_count
                                     : result.segment_count;
  const std::size_t fitSegmentCount =
      result.fit_segment_count > 0 ? result.fit_segment_count
                                   : result.segment_count;
  const std::size_t candidateSegmentCount = result.candidate_segment_count;
  const double consensusSupport =
      fitSegmentCount > 0
          ? static_cast<double>(result.consensus_support) /
                static_cast<double>(fitSegmentCount)
          : 0.0;
  const double candidateConsensusSupport =
      candidateSegmentCount > 0
          ? static_cast<double>(result.candidate_consensus_support) /
                static_cast<double>(candidateSegmentCount)
          : 0.0;
  const double candidateSegmentRatio =
      totalSegmentCount > 0
          ? static_cast<double>(candidateSegmentCount) /
                static_cast<double>(totalSegmentCount)
          : 0.0;
  const double familyScoreMargin =
      result.runner_up_family_score > 0.0
          ? result.family_score - result.runner_up_family_score
          : result.family_score;
  const double familyScoreDominance =
      result.runner_up_family_score > 0.0
          ? result.family_score / result.runner_up_family_score
          : 0.0;
  const double runnerUpScoreRatio =
      result.family_score > 0.0
          ? result.runner_up_family_score / result.family_score
          : 0.0;
  pfc::string8 familyRank;
  pfc::string8 familyRankCandidate;
  const std::size_t familyRankCount =
      (std::min)(result.family_rank_count, result.family_rank_bpm.size());
  for (std::size_t i = 0; i < familyRankCount; ++i) {
    if (i > 0) {
      familyRank << "|";
      familyRankCandidate << "|";
    }
    familyRank << "r" << static_cast<uint64_t>(i + 1) << ":"
               << format_float_locale(result.family_rank_bpm[i], 3) << ":"
               << format_float_locale(result.family_rank_score[i], 6) << ":"
               << static_cast<uint64_t>(result.family_rank_support[i]);
    familyRankCandidate << "r" << static_cast<uint64_t>(i + 1) << ":"
                        << format_float_locale(result.family_rank_bpm[i], 3)
                        << ":"
                        << static_cast<uint64_t>(
                               result.family_rank_candidate_support[i]);
  }
  if (familyRank.is_empty()) {
    familyRank = "none";
  }
  if (familyRankCandidate.is_empty()) {
    familyRankCandidate = "none";
  }
  FB2K_console_formatter()
      << "foo_smart_tempo: [" << trackLabel << "] HodgkinsonTatumProbe"
      << " enabled=" << (result.enabled ? 1 : 0)
      << " candidate=" << (result.candidate ? 1 : 0)
      << " track_key=" << trackKey
      << " source=" << result.frontend_source
      << " input_bpm=" << format_float_locale(result.input_bpm, 3)
      << " candidate_bpm=" << format_float_locale(result.candidate_bpm, 3)
      << " confidence=" << format_float_locale(confidence, 6)
      << " score=" << format_float_locale(score, 6)
      << " loop_fit=" << format_float_locale(result.loop_fit_score, 6)
      << " family_bpm=" << format_float_locale(result.family_bpm, 3)
      << " family_score=" << format_float_locale(result.family_score, 6)
      << " family_score_margin="
      << format_float_locale(familyScoreMargin, 6)
      << " family_score_dominance="
      << format_float_locale(familyScoreDominance, 6)
      << " candidate_ratio_to_input="
      << format_float_locale(result.candidate_ratio_to_input, 6)
      << " candidate_ratio_class=" << result.candidate_ratio_class
      << " family_ratio_to_input="
      << format_float_locale(result.family_ratio_to_input, 6)
      << " family_ratio_class=" << result.family_ratio_class
      << " runner_up_family_bpm="
      << format_float_locale(result.runner_up_family_bpm, 3)
      << " runner_up_family_score="
      << format_float_locale(result.runner_up_family_score, 6)
      << " runner_up_score_ratio="
      << format_float_locale(runnerUpScoreRatio, 6)
      << " runner_up_family_support="
      << static_cast<uint64_t>(result.runner_up_family_support)
      << " family_rank_count=" << static_cast<uint64_t>(familyRankCount)
      << " family_rank=" << familyRank
      << " tatum_count=" << static_cast<uint64_t>(result.tatum_count)
      << " segment_index=" << static_cast<uint64_t>(result.segment_index)
      << " segment_count=" << static_cast<uint64_t>(result.segment_count)
      << " consensus_support=" << format_float_locale(consensusSupport, 6)
      << " onset_count=" << static_cast<uint64_t>(result.onset_count)
      << " odf_peak_count=" << static_cast<uint64_t>(result.odf_peak_count)
      << " segment_start_sec="
      << format_float_locale(result.segment_start_sec, 3)
      << " segment_duration_sec="
      << format_float_locale(result.segment_duration_sec, 3)
      << " meter=" << result.meter
      << " class=" << result.family_class
      << " reason=" << result.reason;
}

void log_hodgkinson_primary_segment(
    const char* trackLabel,
    const HodgkinsonTatumProbeResult& result) {
  FB2K_console_formatter()
      << "foo_smart_tempo: [" << trackLabel << "] HodgkinsonPrimarySegment"
      << " enabled=" << (result.enabled ? 1 : 0)
      << " candidate=" << (result.candidate ? 1 : 0)
      << " source=" << result.frontend_source
      << " segment_index=" << static_cast<uint64_t>(result.segment_index)
      << " segment_count=" << static_cast<uint64_t>(result.segment_count)
      << " segment_start_sec="
      << format_float_locale(result.segment_start_sec, 3)
      << " segment_duration_sec="
      << format_float_locale(result.segment_duration_sec, 3)
      << " candidate_bpm=" << format_float_locale(result.candidate_bpm, 3)
      << " score=" << format_float_locale(result.loop_fit_score, 6)
      << " confidence=" << format_float_locale(result.loop_fit_confidence, 6)
      << " tatum_count=" << static_cast<uint64_t>(result.tatum_count)
      << " meter=" << result.meter
      << " onset_count=" << static_cast<uint64_t>(result.onset_count)
      << " odf_peak_count=" << static_cast<uint64_t>(result.odf_peak_count)
      << " reason=" << result.reason;
}

void log_hodgkinson_primary_segment_candidate(
    const char* trackLabel, uint64_t trackKey,
    const HodgkinsonFullMirSegmentCandidate& candidate) {
  FB2K_console_formatter()
      << "foo_smart_tempo: [" << trackLabel
      << "] HodgkinsonPrimarySegmentCandidate"
      << " enabled=" << (candidate.enabled ? 1 : 0)
      << " track_key=" << trackKey
      << " source=" << candidate.frontend_source
      << " segment_index=" << static_cast<uint64_t>(candidate.segment_index)
      << " segment_count=" << static_cast<uint64_t>(candidate.segment_count)
      << " segment_start_sec="
      << format_float_locale(candidate.segment_start_sec, 3)
      << " segment_duration_sec="
      << format_float_locale(candidate.segment_duration_sec, 3)
      << " rank=" << static_cast<uint64_t>(candidate.rank)
      << " candidate_bpm=" << format_float_locale(candidate.candidate_bpm, 3)
      << " combined_score="
      << format_float_locale(candidate.combined_score, 6)
      << " quantization_score="
      << format_float_locale(candidate.quantization_score, 6)
      << " meter_score=" << format_float_locale(candidate.meter_score, 6)
      << " quantization_error="
      << format_float_locale(candidate.quantization_error, 6)
      << " tatum_count=" << static_cast<uint64_t>(candidate.tatum_count)
      << " num_bars=" << static_cast<uint64_t>(candidate.num_bars)
      << " beats_per_bar=" << static_cast<uint64_t>(candidate.beats_per_bar)
      << " total_beats=" << static_cast<uint64_t>(candidate.total_beats)
      << " trailing_beats="
      << static_cast<uint64_t>(candidate.trailing_beats)
      << " meter=" << candidate.meter
      << " onset_count=" << static_cast<uint64_t>(candidate.onset_count)
      << " odf_peak_count=" << static_cast<uint64_t>(candidate.odf_peak_count)
      << " winner=" << (candidate.winner ? 1 : 0)
      << " reason=" << candidate.reason;
}

void log_hodgkinson_partial_bar_aggregate_shadow(
    const char* trackLabel, uint64_t trackKey,
    const HodgkinsonPartialBarAggregateShadow& shadow) {
  FB2K_console_formatter()
      << "foo_smart_tempo: [" << trackLabel
      << "] HodgkinsonPartialBarAggregateShadow"
      << " enabled=" << (shadow.enabled ? 1 : 0)
      << " track_key=" << trackKey
      << " candidate=" << (shadow.candidate ? 1 : 0)
      << " coarse_bpm=" << format_float_locale(shadow.coarse_bpm, 3)
      << " local_exact_bpm="
      << format_float_locale(shadow.local_exact_bpm, 3)
      << " local_exact_score="
      << format_float_locale(shadow.local_exact_score, 6)
      << " score_sum=" << format_float_locale(shadow.score_sum, 6)
      << " runner_up_bpm="
      << format_float_locale(shadow.runner_up_bpm, 3)
      << " runner_up_score_sum="
      << format_float_locale(shadow.runner_up_score_sum, 6)
      << " runner_up_score_ratio="
      << format_float_locale(shadow.runner_up_score_ratio, 6)
      << " rank1_hits=" << static_cast<uint64_t>(shadow.rank1_hits)
      << " top5_hits=" << static_cast<uint64_t>(shadow.top5_hits)
      << " segment_count=" << static_cast<uint64_t>(shadow.segment_count)
      << " total_segment_count="
      << static_cast<uint64_t>(shadow.total_segment_count)
      << " row_count=" << static_cast<uint64_t>(shadow.row_count)
      << " mean_quantization_score="
      << format_float_locale(shadow.mean_quantization_score, 6)
      << " mean_meter_score="
      << format_float_locale(shadow.mean_meter_score, 6)
      << " reason=" << shadow.reason;
}

void log_hodgkinson_partial_bar_topk_exact_candidate(
    const char* trackLabel, uint64_t trackKey,
    const HodgkinsonPartialBarTopKExactCandidate& candidate) {
  FB2K_console_formatter()
      << "foo_smart_tempo: [" << trackLabel
      << "] HodgkinsonPartialBarTopKExactCandidate"
      << " enabled=1"
      << " track_key=" << trackKey
      << " family_rank=" << static_cast<uint64_t>(candidate.family_rank)
      << " family_count=" << static_cast<uint64_t>(candidate.family_count)
      << " coarse_bpm=" << format_float_locale(candidate.coarse_bpm, 3)
      << " local_exact_bpm="
      << format_float_locale(candidate.local_exact_bpm, 3)
      << " local_exact_score="
      << format_float_locale(candidate.local_exact_score, 6)
      << " score_sum=" << format_float_locale(candidate.score_sum, 6)
      << " score_ratio_to_top="
      << format_float_locale(candidate.score_ratio_to_top, 6)
      << " rank1_hits=" << static_cast<uint64_t>(candidate.rank1_hits)
      << " top5_hits=" << static_cast<uint64_t>(candidate.top5_hits)
      << " segment_count="
      << static_cast<uint64_t>(candidate.segment_count)
      << " total_segment_count="
      << static_cast<uint64_t>(candidate.total_segment_count)
      << " row_count=" << static_cast<uint64_t>(candidate.row_count)
      << " mean_quantization_score="
      << format_float_locale(candidate.mean_quantization_score, 6)
      << " mean_meter_score="
      << format_float_locale(candidate.mean_meter_score, 6)
      << " fullboard_bpm="
      << format_float_locale(candidate.fullboard_bpm, 3)
      << " ratio_to_fullboard="
      << format_float_locale(candidate.ratio_to_fullboard, 6)
      << " reason=" << candidate.reason;
}

void log_hodgkinson_primary_candidate_set(
    const char* trackLabel,
    const HodgkinsonTatumProbeResult& result) {
  const std::size_t totalSegmentCount =
      result.total_segment_count > 0 ? result.total_segment_count
                                     : result.segment_count;
  const std::size_t fitSegmentCount =
      result.fit_segment_count > 0 ? result.fit_segment_count
                                   : result.segment_count;
  const std::size_t candidateSegmentCount = result.candidate_segment_count;
  const double consensusSupport =
      fitSegmentCount > 0
          ? static_cast<double>(result.consensus_support) /
                static_cast<double>(fitSegmentCount)
          : 0.0;
  const double candidateConsensusSupport =
      candidateSegmentCount > 0
          ? static_cast<double>(result.candidate_consensus_support) /
                static_cast<double>(candidateSegmentCount)
          : 0.0;
  const double candidateSegmentRatio =
      totalSegmentCount > 0
          ? static_cast<double>(candidateSegmentCount) /
                static_cast<double>(totalSegmentCount)
          : 0.0;
  const double familyScoreMargin =
      result.runner_up_family_score > 0.0
          ? result.family_score - result.runner_up_family_score
          : result.family_score;
  const double familyScoreDominance =
      result.runner_up_family_score > 0.0
          ? result.family_score / result.runner_up_family_score
          : 0.0;
  const double runnerUpScoreRatio =
      result.family_score > 0.0
          ? result.runner_up_family_score / result.family_score
          : 0.0;

  pfc::string8 familyRank;
  pfc::string8 familyRankCandidate;
  const std::size_t familyRankCount =
      (std::min)(result.family_rank_count, result.family_rank_bpm.size());
  for (std::size_t i = 0; i < familyRankCount; ++i) {
    if (i > 0) {
      familyRank << "|";
      familyRankCandidate << "|";
    }
    familyRank << "r" << static_cast<uint64_t>(i + 1) << ":"
               << format_float_locale(result.family_rank_bpm[i], 3) << ":"
               << format_float_locale(result.family_rank_score[i], 6) << ":"
               << static_cast<uint64_t>(result.family_rank_support[i]);
    familyRankCandidate << "r" << static_cast<uint64_t>(i + 1) << ":"
                        << format_float_locale(result.family_rank_bpm[i], 3)
                        << ":"
                        << static_cast<uint64_t>(
                               result.family_rank_candidate_support[i]);
  }
  if (familyRank.is_empty()) {
    familyRank = "none";
  }
  if (familyRankCandidate.is_empty()) {
    familyRankCandidate = "none";
  }

  FB2K_console_formatter()
      << "foo_smart_tempo: [" << trackLabel
      << "] HodgkinsonPrimaryCandidateSet"
      << " enabled=" << (result.enabled ? 1 : 0)
      << " candidate=" << (result.candidate ? 1 : 0)
      << " source=" << result.frontend_source
      << " top_bpm=" << format_float_locale(result.family_bpm, 3)
      << " top_score=" << format_float_locale(result.family_score, 6)
      << " runner_up_bpm="
      << format_float_locale(result.runner_up_family_bpm, 3)
      << " runner_up_score="
      << format_float_locale(result.runner_up_family_score, 6)
      << " runner_up_score_ratio="
      << format_float_locale(runnerUpScoreRatio, 6)
      << " family_score_margin="
      << format_float_locale(familyScoreMargin, 6)
      << " family_score_dominance="
      << format_float_locale(familyScoreDominance, 6)
      << " consensus_support=" << format_float_locale(consensusSupport, 6)
      << " candidate_consensus_support="
      << format_float_locale(candidateConsensusSupport, 6)
      << " total_segment_count=" << static_cast<uint64_t>(totalSegmentCount)
      << " fit_segment_count=" << static_cast<uint64_t>(fitSegmentCount)
      << " candidate_segment_count="
      << static_cast<uint64_t>(candidateSegmentCount)
      << " candidate_segment_ratio="
      << format_float_locale(candidateSegmentRatio, 6)
      << " family_support_ratio_fit="
      << format_float_locale(consensusSupport, 6)
      << " family_support_ratio_candidate="
      << format_float_locale(candidateConsensusSupport, 6)
      << " accepted_segment_count="
      << static_cast<uint64_t>(fitSegmentCount)
      << " candidate_consensus_count="
      << static_cast<uint64_t>(result.candidate_consensus_support)
      << " odf_peak_count=" << static_cast<uint64_t>(result.odf_peak_count)
      << " family_rank_count=" << static_cast<uint64_t>(familyRankCount)
      << " family_rank=" << familyRank
      << " family_rank_candidate=" << familyRankCandidate
      << " class=" << result.family_class
      << " reason=" << result.reason;
}

void log_hodgkinson_alias_pulse_evidence(
    const char* trackLabel,
    const HodgkinsonAliasPulseEvidence& evidence) {
  FB2K_console_formatter()
      << "foo_smart_tempo: [" << trackLabel
      << "] HodgkinsonAliasPulseEvidence"
      << " enabled=" << (evidence.enabled ? 1 : 0)
      << " source=audacity_mir_full_only"
      << " family_rank=" << static_cast<uint64_t>(evidence.family_rank)
      << " family_bpm=" << format_float_locale(evidence.family_bpm, 3)
      << " family_score=" << format_float_locale(evidence.family_score, 6)
      << " family_support="
      << static_cast<uint64_t>(evidence.family_support)
      << " family_candidate_support="
      << static_cast<uint64_t>(evidence.family_candidate_support)
      << " alias_bpm=" << format_float_locale(evidence.alias_bpm, 3)
      << " alias_class=" << evidence.alias_class
      << " alias_multiplier="
      << format_float_locale(evidence.alias_multiplier, 6)
      << " route_valid=" << (evidence.route_valid ? 1 : 0)
      << " phase_vs=" << format_float_locale(evidence.phase_vs, 6)
      << " axial_phase_vs="
      << format_float_locale(evidence.axial_phase_vs, 6)
      << " beat_salience="
      << format_float_locale(evidence.beat_salience, 6)
      << " tatum_salience="
      << format_float_locale(evidence.tatum_salience, 6)
      << " beat_tatum_ratio="
      << format_float_locale(evidence.beat_tatum_ratio, 6)
      << " normalized_onset_density="
      << format_float_locale(evidence.normalized_onset_density, 6)
      << " section_support="
      << format_float_locale(evidence.section_support, 6)
      << " section_stability="
      << format_float_locale(evidence.section_stability, 6)
      << " pulse_score=" << format_float_locale(evidence.pulse_score, 6)
      << " reason=" << evidence.reason;
}

void log_hodgkinson_continuous_refinement_evidence(
    const char* trackLabel,
    const HodgkinsonContinuousRefinementEvidence& evidence) {
  FB2K_console_formatter()
      << "foo_smart_tempo: [" << trackLabel
      << "] HodgkinsonContinuousRefinementEvidence"
      << " enabled=" << (evidence.enabled ? 1 : 0)
      << " source=audacity_mir_full_only"
      << " family_rank=" << static_cast<uint64_t>(evidence.family_rank)
      << " family_bpm=" << format_float_locale(evidence.family_bpm, 3)
      << " family_score=" << format_float_locale(evidence.family_score, 6)
      << " family_support="
      << static_cast<uint64_t>(evidence.family_support)
      << " alias_bpm=" << format_float_locale(evidence.alias_bpm, 3)
      << " alias_class=" << evidence.alias_class
      << " alias_multiplier="
      << format_float_locale(evidence.alias_multiplier, 6)
      << " route_valid=" << (evidence.route_valid ? 1 : 0)
      << " scan_start_bpm="
      << format_float_locale(evidence.scan_start_bpm, 3)
      << " scan_end_bpm=" << format_float_locale(evidence.scan_end_bpm, 3)
      << " scan_step_bpm=" << format_float_locale(evidence.scan_step_bpm, 3)
      << " scan_count=" << static_cast<uint64_t>(evidence.scan_count)
      << " base_score=" << format_float_locale(evidence.base_score, 6)
      << " best_bpm=" << format_float_locale(evidence.best_bpm, 3)
      << " best_score=" << format_float_locale(evidence.best_score, 6)
      << " score_delta=" << format_float_locale(evidence.score_delta, 6)
      << " score_ratio=" << format_float_locale(evidence.score_ratio, 6)
      << " runner_up_bpm="
      << format_float_locale(evidence.runner_up_bpm, 3)
      << " runner_up_score="
      << format_float_locale(evidence.runner_up_score, 6)
      << " peak_separation_bpm="
      << format_float_locale(evidence.peak_separation_bpm, 3)
      << " best_phase_vs="
      << format_float_locale(evidence.best_phase_vs, 6)
      << " best_axial_phase_vs="
      << format_float_locale(evidence.best_axial_phase_vs, 6)
      << " best_density=" << format_float_locale(evidence.best_density, 6)
      << " section_support="
      << format_float_locale(evidence.section_support, 6)
      << " section_stability="
      << format_float_locale(evidence.section_stability, 6)
      << " odf_peak_count="
      << static_cast<uint64_t>(evidence.odf_peak_count)
      << " accepted_segment_count="
      << static_cast<uint64_t>(evidence.accepted_segment_count)
      << " peak1_bpm=" << format_float_locale(evidence.peak1_bpm, 3)
      << " peak1_score=" << format_float_locale(evidence.peak1_score, 6)
      << " peak2_bpm=" << format_float_locale(evidence.peak2_bpm, 3)
      << " peak2_score=" << format_float_locale(evidence.peak2_score, 6)
      << " peak3_bpm=" << format_float_locale(evidence.peak3_bpm, 3)
      << " peak3_score=" << format_float_locale(evidence.peak3_score, 6)
      << " fine_scan_start_bpm="
      << format_float_locale(evidence.fine_scan_start_bpm, 3)
      << " fine_scan_end_bpm="
      << format_float_locale(evidence.fine_scan_end_bpm, 3)
      << " fine_scan_step_bpm="
      << format_float_locale(evidence.fine_scan_step_bpm, 3)
      << " fine_scan_count="
      << static_cast<uint64_t>(evidence.fine_scan_count)
      << " fine_best_bpm=" << format_float_locale(evidence.fine_best_bpm, 3)
      << " fine_best_score="
      << format_float_locale(evidence.fine_best_score, 6)
      << " fine_score_delta="
      << format_float_locale(evidence.fine_score_delta, 6)
      << " fine_score_ratio="
      << format_float_locale(evidence.fine_score_ratio, 6)
      << " fine_segment_count="
      << static_cast<uint64_t>(evidence.fine_segment_count)
      << " fine_segment_median_bpm="
      << format_float_locale(evidence.fine_segment_median_bpm, 3)
      << " fine_segment_q25_bpm="
      << format_float_locale(evidence.fine_segment_q25_bpm, 3)
      << " fine_segment_q75_bpm="
      << format_float_locale(evidence.fine_segment_q75_bpm, 3)
      << " fine_segment_iqr_bpm="
      << format_float_locale(evidence.fine_segment_iqr_bpm, 3)
      << " fine_segment_min_bpm="
      << format_float_locale(evidence.fine_segment_min_bpm, 3)
      << " fine_segment_max_bpm="
      << format_float_locale(evidence.fine_segment_max_bpm, 3)
      << " fine_segment_support_005="
      << format_float_locale(evidence.fine_segment_support_005, 6)
      << " fine_segment_support_010="
      << format_float_locale(evidence.fine_segment_support_010, 6)
      << " range_segment_count="
      << static_cast<uint64_t>(evidence.range_segment_count)
      << " range_segment_median_bpm="
      << format_float_locale(evidence.range_segment_median_bpm, 3)
      << " range_segment_q25_bpm="
      << format_float_locale(evidence.range_segment_q25_bpm, 3)
      << " range_segment_q75_bpm="
      << format_float_locale(evidence.range_segment_q75_bpm, 3)
      << " range_segment_iqr_bpm="
      << format_float_locale(evidence.range_segment_iqr_bpm, 3)
      << " range_segment_min_bpm="
      << format_float_locale(evidence.range_segment_min_bpm, 3)
      << " range_segment_max_bpm="
      << format_float_locale(evidence.range_segment_max_bpm, 3)
      << " range_segment_support_best_050="
      << format_float_locale(evidence.range_segment_support_best_050, 6)
      << " range_segment_support_alias_050="
      << format_float_locale(evidence.range_segment_support_alias_050, 6)
      << " range_segment_cluster1_bpm="
      << format_float_locale(evidence.range_segment_cluster1_bpm, 3)
      << " range_segment_cluster1_support="
      << format_float_locale(evidence.range_segment_cluster1_support, 6)
      << " range_segment_cluster2_bpm="
      << format_float_locale(evidence.range_segment_cluster2_bpm, 3)
      << " range_segment_cluster2_support="
      << format_float_locale(evidence.range_segment_cluster2_support, 6)
      << " range_segment_cluster3_bpm="
      << format_float_locale(evidence.range_segment_cluster3_bpm, 3)
      << " range_segment_cluster3_support="
      << format_float_locale(evidence.range_segment_cluster3_support, 6)
      << " interval_scan_start_bpm="
      << format_float_locale(evidence.interval_scan_start_bpm, 3)
      << " interval_scan_end_bpm="
      << format_float_locale(evidence.interval_scan_end_bpm, 3)
      << " interval_scan_step_bpm="
      << format_float_locale(evidence.interval_scan_step_bpm, 3)
      << " interval_scan_count="
      << static_cast<uint64_t>(evidence.interval_scan_count)
      << " interval_best_bpm="
      << format_float_locale(evidence.interval_best_bpm, 3)
      << " interval_best_score="
      << format_float_locale(evidence.interval_best_score, 6)
      << " interval_alias_score="
      << format_float_locale(evidence.interval_alias_score, 6)
      << " interval_continuous_best_score="
      << format_float_locale(evidence.interval_continuous_best_score, 6)
      << " interval_peak1_bpm="
      << format_float_locale(evidence.interval_peak1_bpm, 3)
      << " interval_peak1_score="
      << format_float_locale(evidence.interval_peak1_score, 6)
      << " interval_peak2_bpm="
      << format_float_locale(evidence.interval_peak2_bpm, 3)
      << " interval_peak2_score="
      << format_float_locale(evidence.interval_peak2_score, 6)
      << " interval_peak3_bpm="
      << format_float_locale(evidence.interval_peak3_bpm, 3)
      << " interval_peak3_score="
      << format_float_locale(evidence.interval_peak3_score, 6)
      << " interval_fine_scan_start_bpm="
      << format_float_locale(evidence.interval_fine_scan_start_bpm, 3)
      << " interval_fine_scan_end_bpm="
      << format_float_locale(evidence.interval_fine_scan_end_bpm, 3)
      << " interval_fine_scan_step_bpm="
      << format_float_locale(evidence.interval_fine_scan_step_bpm, 3)
      << " interval_fine_scan_count="
      << static_cast<uint64_t>(evidence.interval_fine_scan_count)
      << " interval_fine_best_bpm="
      << format_float_locale(evidence.interval_fine_best_bpm, 3)
      << " interval_fine_best_score="
      << format_float_locale(evidence.interval_fine_best_score, 6)
      << " interval_segment_count="
      << static_cast<uint64_t>(evidence.interval_segment_count)
      << " interval_segment_median_bpm="
      << format_float_locale(evidence.interval_segment_median_bpm, 3)
      << " interval_segment_q25_bpm="
      << format_float_locale(evidence.interval_segment_q25_bpm, 3)
      << " interval_segment_q75_bpm="
      << format_float_locale(evidence.interval_segment_q75_bpm, 3)
      << " interval_segment_iqr_bpm="
      << format_float_locale(evidence.interval_segment_iqr_bpm, 3)
      << " interval_segment_min_bpm="
      << format_float_locale(evidence.interval_segment_min_bpm, 3)
      << " interval_segment_max_bpm="
      << format_float_locale(evidence.interval_segment_max_bpm, 3)
      << " interval_segment_support_best_050="
      << format_float_locale(evidence.interval_segment_support_best_050, 6)
      << " interval_segment_support_alias_050="
      << format_float_locale(evidence.interval_segment_support_alias_050, 6)
      << " interval_segment_cluster1_bpm="
      << format_float_locale(evidence.interval_segment_cluster1_bpm, 3)
      << " interval_segment_cluster1_support="
      << format_float_locale(evidence.interval_segment_cluster1_support, 6)
      << " interval_segment_cluster2_bpm="
      << format_float_locale(evidence.interval_segment_cluster2_bpm, 3)
      << " interval_segment_cluster2_support="
      << format_float_locale(evidence.interval_segment_cluster2_support, 6)
      << " interval_segment_cluster3_bpm="
      << format_float_locale(evidence.interval_segment_cluster3_bpm, 3)
      << " interval_segment_cluster3_support="
      << format_float_locale(evidence.interval_segment_cluster3_support, 6)
      << " reason=" << evidence.reason;
}

void log_mir_high_pulse_interval_conflict_shadow(
    const char* trackLabel,
    const MirHighPulseIntervalConflictShadow& shadow) {
  FB2K_console_formatter()
      << "foo_smart_tempo: [" << trackLabel
      << "] MirHighPulseIntervalConflictShadow"
      << " enabled=" << (shadow.enabled ? 1 : 0)
      << " candidate=" << (shadow.candidate ? 1 : 0)
      << " write_candidate=" << (shadow.write_candidate ? 1 : 0)
      << " review_candidate=" << (shadow.review_candidate ? 1 : 0)
      << " runtime_available=" << (shadow.runtime_available ? 1 : 0)
      << " same_direction=" << (shadow.same_direction ? 1 : 0)
      << " runtime_bpm=" << format_float_locale(shadow.runtime_bpm, 3)
      << " phase_bpm=" << format_float_locale(shadow.phase_bpm, 3)
      << " interval_bpm=" << format_float_locale(shadow.interval_bpm, 3)
      << " interval_iqr_bpm="
      << format_float_locale(shadow.interval_iqr_bpm, 3)
      << " interval_cluster_support="
      << format_float_locale(shadow.interval_cluster_support, 6)
      << " interval_alias_support_050="
      << format_float_locale(shadow.interval_alias_support_050, 6)
      << " runtime_interval_delta="
      << format_float_locale(shadow.runtime_interval_delta, 3)
      << " runtime_phase_delta="
      << format_float_locale(shadow.runtime_phase_delta, 3)
      << " decision_class=" << shadow.decision_class
      << " reason=" << shadow.reason;
}

void log_hodgkinson_material_risk_evidence(
    const char* trackLabel,
    const HodgkinsonMaterialRiskEvidence& evidence) {
  FB2K_console_formatter()
      << "foo_smart_tempo: [" << trackLabel
      << "] HodgkinsonMaterialRiskEvidence"
      << " enabled=" << (evidence.enabled ? 1 : 0)
      << " source=audacity_mir_full_topk"
      << " candidate=" << (evidence.candidate ? 1 : 0)
      << " route_valid=" << (evidence.route_valid ? 1 : 0)
      << " route_matched=" << (evidence.route_matched ? 1 : 0)
      << " generic_route=" << (evidence.generic_route ? 1 : 0)
      << " review_hold_candidate="
      << (evidence.review_hold_candidate ? 1 : 0)
      << " selected_context_bpm="
      << format_float_locale(evidence.selected_context_bpm, 3)
      << " route_min=" << format_float_locale(evidence.route_min_bpm, 3)
      << " route_max=" << format_float_locale(evidence.route_max_bpm, 3)
      << " route_center=" << format_float_locale(evidence.route_center_bpm, 3)
      << " segment_count=" << static_cast<uint64_t>(evidence.segment_count)
      << " support_108=" << format_float_locale(evidence.support_108, 6)
      << " support_120=" << format_float_locale(evidence.support_120, 6)
      << " support_132=" << format_float_locale(evidence.support_132, 6)
      << " support_135=" << format_float_locale(evidence.support_135, 6)
      << " support_136=" << format_float_locale(evidence.support_136, 6)
      << " support_144=" << format_float_locale(evidence.support_144, 6)
      << " winner_support_144="
      << format_float_locale(evidence.winner_support_144, 6)
      << " decision_class=" << evidence.decision_class
      << " reason=" << evidence.reason;
}

void log_hodgkinson_material_risk_local_exact_probe(
    const char* trackLabel,
    const HodgkinsonMaterialRiskLocalExactProbe& probe) {
  FB2K_console_formatter()
      << "foo_smart_tempo: [" << trackLabel
      << "] HodgkinsonMaterialRiskLocalExactProbe"
      << " enabled=" << (probe.enabled ? 1 : 0)
      << " source=mir_phase_peaks"
      << " candidate=" << (probe.candidate ? 1 : 0)
      << " review_hold_candidate="
      << (probe.review_hold_candidate ? 1 : 0)
      << " release_candidate=" << (probe.release_candidate ? 1 : 0)
      << " selected_context_bpm="
      << format_float_locale(probe.selected_context_bpm, 3)
      << " current_bpm=" << format_float_locale(probe.current_bpm, 3)
      << " current_local_exact_score="
      << format_float_locale(probe.current_local_exact_score, 6)
      << " scan_start_bpm="
      << format_float_locale(probe.scan_start_bpm, 3)
      << " scan_end_bpm=" << format_float_locale(probe.scan_end_bpm, 3)
      << " scan_step_bpm=" << format_float_locale(probe.scan_step_bpm, 3)
      << " scan_count=" << static_cast<uint64_t>(probe.scan_count)
      << " best_bpm=" << format_float_locale(probe.best_bpm, 3)
      << " best_score=" << format_float_locale(probe.best_score, 6)
      << " best_anchor_bpm="
      << format_float_locale(probe.best_anchor_bpm, 3)
      << " best_boundary_hit=" << (probe.best_boundary_hit ? 1 : 0)
      << " probe_138_bpm=" << format_float_locale(probe.probe_138_bpm, 3)
      << " probe_138_score="
      << format_float_locale(probe.probe_138_score, 6)
      << " probe_138_boundary_hit="
      << (probe.probe_138_boundary_hit ? 1 : 0)
      << " best_score_ratio_to_current="
      << format_float_locale(probe.best_score_ratio_to_current, 6)
      << " probe_138_score_ratio_to_current="
      << format_float_locale(probe.probe_138_score_ratio_to_current, 6)
      << " target_family_candidate_count="
      << static_cast<uint64_t>(probe.target_family_candidate_count)
      << " top1_bpm=" << format_float_locale(probe.top1_bpm, 3)
      << " top1_score=" << format_float_locale(probe.top1_score, 6)
      << " top1_anchor_bpm="
      << format_float_locale(probe.top1_anchor_bpm, 3)
      << " top1_boundary_hit=" << (probe.top1_boundary_hit ? 1 : 0)
      << " top2_bpm=" << format_float_locale(probe.top2_bpm, 3)
      << " top2_score=" << format_float_locale(probe.top2_score, 6)
      << " top2_anchor_bpm="
      << format_float_locale(probe.top2_anchor_bpm, 3)
      << " top2_boundary_hit=" << (probe.top2_boundary_hit ? 1 : 0)
      << " top3_bpm=" << format_float_locale(probe.top3_bpm, 3)
      << " top3_score=" << format_float_locale(probe.top3_score, 6)
      << " top3_anchor_bpm="
      << format_float_locale(probe.top3_anchor_bpm, 3)
      << " top3_boundary_hit=" << (probe.top3_boundary_hit ? 1 : 0)
      << " non_boundary_top_count="
      << static_cast<uint64_t>(probe.non_boundary_top_count)
      << " top_span_bpm=" << format_float_locale(probe.top_span_bpm, 3)
      << " peak_count=" << static_cast<uint64_t>(probe.peak_count)
      << " support_136=" << format_float_locale(probe.support_136, 6)
      << " support_144=" << format_float_locale(probe.support_144, 6)
      << " winner_support_144="
      << format_float_locale(probe.winner_support_144, 6)
      << " decision_class=" << probe.decision_class
      << " reason=" << probe.reason;
}

void log_mir_primary_selection(
    const char* trackLabel,
    const MirPrimarySelection& shadow) {
  FB2K_console_formatter()
      << "foo_smart_tempo: [" << trackLabel
      << "] MirPrimarySelection"
      << " enabled=" << (shadow.enabled ? 1 : 0)
      << " source=audacity_mir_full_only"
      << " selector_profile=measured_candidate"
      << " candidate=" << (shadow.candidate ? 1 : 0)
      << " auto_candidate=" << (shadow.auto_candidate ? 1 : 0)
      << " review_hold=" << (shadow.review_hold ? 1 : 0)
      << " route_valid=" << (shadow.route_valid ? 1 : 0)
      << " sparse_acapella_review_hold="
      << (shadow.sparse_acapella_review_hold ? 1 : 0)
      << " material_risk_review_hold="
      << (shadow.material_risk_review_hold ? 1 : 0)
      << " high_family_conflict_review_hold="
      << (shadow.high_family_conflict_review_hold ? 1 : 0)
      << " low_pulse_exactness_review_hold="
      << (shadow.low_pulse_exactness_review_hold ? 1 : 0)
      << " soft_center_conflict_review_hold="
      << (shadow.soft_center_conflict_review_hold ? 1 : 0)
      << " soft_center_alias_review_hold="
      << (shadow.soft_center_alias_review_hold ? 1 : 0)
      << " review_hold_alias_release="
      << (shadow.review_hold_alias_release ? 1 : 0)
      << " review_hold_alias_release_bpm="
      << format_float_locale(shadow.review_hold_alias_release_bpm, 3)
      << " review_hold_alias_release_candidate_segment_ratio="
      << format_float_locale(
             shadow.review_hold_alias_release_candidate_segment_ratio, 6)
      << " review_hold_alias_release_rank_candidate_support_ratio="
      << format_float_locale(
             shadow.review_hold_alias_release_rank_candidate_support_ratio, 6)
      << " review_hold_alias_release_runner_up_score_ratio="
      << format_float_locale(
             shadow.review_hold_alias_release_runner_up_score_ratio, 6)
      << " selected_bpm=" << format_float_locale(shadow.selected_bpm, 3)
      << " selected_cluster_bpm="
      << format_float_locale(shadow.selected_cluster_bpm, 3)
      << " selected_local_exact_bpm="
      << format_float_locale(shadow.selected_local_exact_bpm, 3)
      << " selected_local_exact_score="
      << format_float_locale(shadow.selected_local_exact_score, 6)
      << " selected_local_exact_delta="
      << format_float_locale(shadow.selected_local_exact_delta, 3)
      << " route_min=" << format_float_locale(shadow.route_min_bpm, 3)
      << " route_max=" << format_float_locale(shadow.route_max_bpm, 3)
      << " route_center=" << format_float_locale(shadow.route_center_bpm, 3)
      << " support=" << format_float_locale(shadow.support, 6)
      << " winner_support="
      << format_float_locale(shadow.winner_support, 6)
      << " score_sum=" << format_float_locale(shadow.score_sum, 6)
      << " best_combined_score="
      << format_float_locale(shadow.best_combined_score, 6)
      << " pulse_score=" << format_float_locale(shadow.pulse_score, 6)
      << " pulse_section_support="
      << format_float_locale(shadow.pulse_section_support, 6)
      << " continuous_score="
      << format_float_locale(shadow.continuous_score, 6)
      << " continuous_section_support="
      << format_float_locale(shadow.continuous_section_support, 6)
      << " continuous_score_ratio="
      << format_float_locale(shadow.continuous_score_ratio, 6)
      << " measured_evidence_score="
      << format_float_locale(shadow.measured_evidence_score, 6)
      << " soft_center_prior_applied="
      << (shadow.soft_center_prior_applied ? 1 : 0)
      << " soft_center_bonus="
      << format_float_locale(shadow.soft_center_bonus, 6)
      << " soft_center_score="
      << format_float_locale(shadow.soft_center_score, 6)
      << " soft_center_runner_up_score="
      << format_float_locale(shadow.soft_center_runner_up_score, 6)
      << " soft_center_dominance_margin="
      << format_float_locale(shadow.soft_center_dominance_margin, 6)
      << " soft_center_high_pulse_veto="
      << (shadow.soft_center_high_pulse_veto ? 1 : 0)
      << " lane_best_support="
      << format_float_locale(shadow.lane_best_support, 6)
      << " lane_high_direct_support="
      << format_float_locale(shadow.lane_high_direct_support, 6)
      << " lane_low_support="
      << format_float_locale(shadow.lane_low_support, 6)
      << " lane_mid_support="
      << format_float_locale(shadow.lane_mid_support, 6)
      << " lane_high_support="
      << format_float_locale(shadow.lane_high_support, 6)
      << " candidate_count=" << static_cast<uint64_t>(shadow.candidate_count)
      << " total_segment_count="
      << static_cast<uint64_t>(shadow.total_segment_count)
      << " odf_peak_count=" << static_cast<uint64_t>(shadow.odf_peak_count)
      << " alias_classes="
      << (shadow.alias_classes.empty() ? "none"
                                      : shadow.alias_classes.c_str())
      << " lane=" << shadow.lane
      << " lane_variant=" << shadow.lane_variant
      << " decision_class=" << shadow.decision_class
      << " reason=" << shadow.reason;
}

void log_mir_cluster_candidate_board_entry(
    const char* trackLabel, std::size_t index, double bpm,
    const char* aliasClasses, double support, double winnerSupport,
    double scoreSum, double bestCombinedScore, double pulseScore,
    double pulseSectionSupport, double pulsePhaseVs, double pulseAxialPhaseVs,
    double continuousScore, double continuousSectionSupport,
    double continuousScoreRatio, double continuousSectionStability,
    std::size_t rows, std::size_t totalSegmentCount) {
  FB2K_console_formatter()
      << "foo_smart_tempo: [" << trackLabel
      << "] HodgkinsonMirPrimaryCandidateBoard"
      << " enabled=1"
      << " source=final_measured_candidate_board"
      << " index=" << static_cast<uint64_t>(index)
      << " bpm=" << format_float_locale(bpm, 3)
      << " alias_classes=" << (aliasClasses != nullptr ? aliasClasses : "none")
      << " support=" << format_float_locale(support, 6)
      << " winner_support=" << format_float_locale(winnerSupport, 6)
      << " score_sum=" << format_float_locale(scoreSum, 6)
      << " best_combined_score=" << format_float_locale(bestCombinedScore, 6)
      << " pulse_score=" << format_float_locale(pulseScore, 6)
      << " pulse_section_support=" << format_float_locale(pulseSectionSupport, 6)
      << " pulse_phase_vs=" << format_float_locale(pulsePhaseVs, 6)
      << " pulse_axial_phase_vs=" << format_float_locale(pulseAxialPhaseVs, 6)
      << " continuous_score=" << format_float_locale(continuousScore, 6)
      << " continuous_section_support="
      << format_float_locale(continuousSectionSupport, 6)
      << " continuous_score_ratio="
      << format_float_locale(continuousScoreRatio, 6)
      << " continuous_section_stability="
      << format_float_locale(continuousSectionStability, 6)
      << " rows=" << static_cast<uint64_t>(rows)
      << " total_segment_count=" << static_cast<uint64_t>(totalSegmentCount);
}

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
    bool maskTruncated) {
  FB2K_console_formatter()
      << "foo_smart_tempo: [" << trackLabel
      << "] MirRawAliasBoard"
      << " enabled=1"
      << " source=route_independent_raw_alias_board"
      << " track_key=" << trackKey
      << " index=" << static_cast<uint64_t>(index)
      << " bpm=" << format_float_locale(bpm, 3)
      << " local_exact_bpm=" << format_float_locale(localExactBpm, 3)
      << " local_exact_score=" << format_float_locale(localExactScore, 6)
      << " local_exact_delta=" << format_float_locale(localExactDelta, 3)
      << " alias_class=" << (aliasClass != nullptr ? aliasClass : "invalid")
      << " alias_multiplier=" << format_float_locale(aliasMultiplier, 6)
      << " source_bpm=" << format_float_locale(sourceBpm, 3)
      << " support=" << format_float_locale(support, 6)
      << " winner_support=" << format_float_locale(winnerSupport, 6)
      << " segment_hits=" << static_cast<uint64_t>(segmentHits)
      << " winner_hits=" << static_cast<uint64_t>(winnerHits)
      << " segment_mask=" << segmentMask
      << " winner_mask=" << winnerMask
      << " score_sum=" << format_float_locale(scoreSum, 6)
      << " avg_quant=" << format_float_locale(avgQuant, 6)
      << " avg_meter=" << format_float_locale(avgMeter, 6)
      << " best_combined_score=" << format_float_locale(bestCombinedScore, 6)
      << " best_quantization_score="
      << format_float_locale(bestQuantizationScore, 6)
      << " best_meter_score=" << format_float_locale(bestMeterScore, 6)
      << " rows=" << static_cast<uint64_t>(rows)
      << " winner_rows=" << static_cast<uint64_t>(winnerRows)
      << " synthetic_support=" << format_float_locale(syntheticSupport, 6)
      << " synthetic_winner_support="
      << format_float_locale(syntheticWinnerSupport, 6)
      << " pulse_score=" << format_float_locale(pulseScore, 6)
      << " pulse_section_support=" << format_float_locale(pulseSectionSupport, 6)
      << " pulse_phase_vs=" << format_float_locale(pulsePhaseVs, 6)
      << " pulse_axial_phase_vs=" << format_float_locale(pulseAxialPhaseVs, 6)
      << " continuous_score=" << format_float_locale(continuousScore, 6)
      << " continuous_section_support="
      << format_float_locale(continuousSectionSupport, 6)
      << " continuous_score_ratio="
      << format_float_locale(continuousScoreRatio, 6)
      << " continuous_section_stability="
      << format_float_locale(continuousSectionStability, 6)
      << " total_segment_count=" << static_cast<uint64_t>(totalSegmentCount)
      << " mask_truncated=" << (maskTruncated ? 1 : 0);
}

void log_mir_candidate_board_entry(
    const char* trackLabel, std::size_t index, double bpm,
    double localExactBpm, double localExactScore,
    double localExactDelta, const char* aliasClasses, double support,
    double winnerSupport,
    double scoreSum, double bestCombinedScore, double pulseScore,
    double pulseSectionSupport, double pulsePhaseVs, double pulseAxialPhaseVs,
    double continuousScore, double continuousSectionSupport,
    double continuousScoreRatio, double continuousSectionStability,
    std::size_t rows, std::size_t totalSegmentCount) {
  FB2K_console_formatter()
      << "foo_smart_tempo: [" << trackLabel
      << "] MirCandidateBoard"
      << " enabled=1"
      << " source=route_independent_full_board"
      << " index=" << static_cast<uint64_t>(index)
      << " bpm=" << format_float_locale(bpm, 3)
      << " local_exact_bpm=" << format_float_locale(localExactBpm, 3)
      << " local_exact_score=" << format_float_locale(localExactScore, 6)
      << " local_exact_delta=" << format_float_locale(localExactDelta, 3)
      << " alias_classes=" << (aliasClasses != nullptr ? aliasClasses : "none")
      << " support=" << format_float_locale(support, 6)
      << " winner_support=" << format_float_locale(winnerSupport, 6)
      << " score_sum=" << format_float_locale(scoreSum, 6)
      << " best_combined_score=" << format_float_locale(bestCombinedScore, 6)
      << " pulse_score=" << format_float_locale(pulseScore, 6)
      << " pulse_section_support=" << format_float_locale(pulseSectionSupport, 6)
      << " pulse_phase_vs=" << format_float_locale(pulsePhaseVs, 6)
      << " pulse_axial_phase_vs=" << format_float_locale(pulseAxialPhaseVs, 6)
      << " continuous_score=" << format_float_locale(continuousScore, 6)
      << " continuous_section_support="
      << format_float_locale(continuousSectionSupport, 6)
      << " continuous_score_ratio="
      << format_float_locale(continuousScoreRatio, 6)
      << " continuous_section_stability="
      << format_float_locale(continuousSectionStability, 6)
      << " rows=" << static_cast<uint64_t>(rows)
      << " total_segment_count=" << static_cast<uint64_t>(totalSegmentCount);
}

void log_mir_policy_candidate_board_entry(
    const char* trackLabel, uint64_t trackKey, std::size_t index,
    const char* origin, double clusterBpm, double localExactBpm,
    double localExactScore, double baseScore, const char* aliasClasses,
    double support, double winnerSupport, double scoreSum,
    double bestCombinedScore, double pulseScore, double pulseSectionSupport,
    double pulsePhaseVs, double pulseAxialPhaseVs, double continuousScore,
    double continuousSectionSupport, double continuousScoreRatio,
    double continuousSectionStability, std::size_t rows,
    std::size_t totalSegmentCount) {
  FB2K_console_formatter()
      << "foo_smart_tempo: [" << trackLabel
      << "] MirPolicyCandidateBoard"
      << " enabled=1"
      << " schema=mir_policy_candidate_board_v1"
      << " track_key=" << trackKey
      << " index=" << static_cast<uint64_t>(index)
      << " origin=" << (origin != nullptr ? origin : "unknown")
      << " cluster_bpm=" << format_double_roundtrip(clusterBpm)
      << " local_exact_bpm=" << format_double_roundtrip(localExactBpm)
      << " local_exact_score=" << format_double_roundtrip(localExactScore)
      << " base_score=" << format_double_roundtrip(baseScore)
      << " alias_classes=" << (aliasClasses != nullptr ? aliasClasses : "none")
      << " support=" << format_double_roundtrip(support)
      << " winner_support=" << format_double_roundtrip(winnerSupport)
      << " score_sum=" << format_double_roundtrip(scoreSum)
      << " best_combined_score=" << format_double_roundtrip(bestCombinedScore)
      << " pulse_score=" << format_double_roundtrip(pulseScore)
      << " pulse_section_support=" << format_double_roundtrip(pulseSectionSupport)
      << " pulse_phase_vs=" << format_double_roundtrip(pulsePhaseVs)
      << " pulse_axial_phase_vs=" << format_double_roundtrip(pulseAxialPhaseVs)
      << " continuous_score=" << format_double_roundtrip(continuousScore)
      << " continuous_section_support="
      << format_double_roundtrip(continuousSectionSupport)
      << " continuous_score_ratio="
      << format_double_roundtrip(continuousScoreRatio)
      << " continuous_section_stability="
      << format_double_roundtrip(continuousSectionStability)
      << " rows=" << static_cast<uint64_t>(rows)
      << " total_segment_count=" << static_cast<uint64_t>(totalSegmentCount);
}

void log_mir_analysis_provenance(const char* trackLabel, uint64_t trackKey,
                                 uint32_t subsongIndex, uint64_t buildId,
                                 const char* policySchema) {
  FB2K_console_formatter()
      << "foo_smart_tempo: [" << trackLabel << "] MirAnalysisProvenance"
      << " enabled=1"
      << " track_key=" << trackKey
      << " subsong=" << static_cast<uint64_t>(subsongIndex)
      << " build_id=" << buildId
      << " policy_schema="
      << (policySchema != nullptr ? policySchema : "unknown");
}

void log_mir_dsp_selection(
    const char* trackLabel,
    const MirPrimarySelection& shadow,
    double priorMinBpm, double priorMaxBpm, bool genericRoute) {
  const double priorCenter =
      priorMinBpm > 0.0 && priorMaxBpm > priorMinBpm
          ? (priorMinBpm + priorMaxBpm) * 0.5
          : 0.0;
  FB2K_console_formatter()
      << "foo_smart_tempo: [" << trackLabel
      << "] MirDspSelection"
      << " enabled=" << (shadow.enabled ? 1 : 0)
      << " source=route_independent_full_board"
      << " selector_version=" << shadow.selector_version
      << " candidate=" << (shadow.candidate ? 1 : 0)
      << " auto_candidate=" << (shadow.auto_candidate ? 1 : 0)
      << " review_hold=" << (shadow.review_hold ? 1 : 0)
      << " selected_bpm=" << format_float_locale(shadow.selected_bpm, 3)
      << " selected_cluster_bpm="
      << format_float_locale(shadow.selected_cluster_bpm, 3)
      << " selected_local_exact_bpm="
      << format_float_locale(shadow.selected_local_exact_bpm, 3)
      << " selected_local_exact_score="
      << format_float_locale(shadow.selected_local_exact_score, 6)
      << " selected_local_exact_delta="
      << format_float_locale(shadow.selected_local_exact_delta, 3)
      << " dsp_min=40.000 dsp_max=220.000"
      << " prior_min=" << format_float_locale(priorMinBpm, 3)
      << " prior_max=" << format_float_locale(priorMaxBpm, 3)
      << " prior_center=" << format_float_locale(priorCenter, 3)
      << " generic_route=" << (genericRoute ? 1 : 0)
      << " support=" << format_float_locale(shadow.support, 6)
      << " winner_support=" << format_float_locale(shadow.winner_support, 6)
      << " score_sum=" << format_float_locale(shadow.score_sum, 6)
      << " best_combined_score="
      << format_float_locale(shadow.best_combined_score, 6)
      << " pulse_score=" << format_float_locale(shadow.pulse_score, 6)
      << " pulse_section_support="
      << format_float_locale(shadow.pulse_section_support, 6)
      << " continuous_score="
      << format_float_locale(shadow.continuous_score, 6)
      << " continuous_section_support="
      << format_float_locale(shadow.continuous_section_support, 6)
      << " continuous_score_ratio="
      << format_float_locale(shadow.continuous_score_ratio, 6)
      << " measured_evidence_score="
      << format_float_locale(shadow.measured_evidence_score, 6)
      << " soft_center_prior_applied="
      << (shadow.soft_center_prior_applied ? 1 : 0)
      << " soft_center_bonus="
      << format_float_locale(shadow.soft_center_bonus, 6)
      << " soft_center_score="
      << format_float_locale(shadow.soft_center_score, 6)
      << " soft_center_runner_up_score="
      << format_float_locale(shadow.soft_center_runner_up_score, 6)
      << " soft_center_dominance_margin="
      << format_float_locale(shadow.soft_center_dominance_margin, 6)
      << " soft_center_high_pulse_veto="
      << (shadow.soft_center_high_pulse_veto ? 1 : 0)
      << " lane_best_support="
      << format_float_locale(shadow.lane_best_support, 6)
      << " lane_high_direct_support="
      << format_float_locale(shadow.lane_high_direct_support, 6)
      << " lane_low_support=" << format_float_locale(shadow.lane_low_support, 6)
      << " lane_mid_support=" << format_float_locale(shadow.lane_mid_support, 6)
      << " lane_high_support="
      << format_float_locale(shadow.lane_high_support, 6)
      << " candidate_count=" << static_cast<uint64_t>(shadow.candidate_count)
      << " total_segment_count="
      << static_cast<uint64_t>(shadow.total_segment_count)
      << " odf_peak_count=" << static_cast<uint64_t>(shadow.odf_peak_count)
      << " alias_classes="
      << (shadow.alias_classes.empty() ? "none" : shadow.alias_classes.c_str())
      << " lane=" << shadow.lane
      << " lane_variant=" << shadow.lane_variant
      << " decision_class=" << shadow.decision_class
      << " reason=" << shadow.reason;
}

void log_mir_policy_decision(
    const char* trackLabel,
    const MirPolicyDecision& shadow) {
  FB2K_console_formatter()
      << "foo_smart_tempo: [" << trackLabel
      << "] MirPolicyDecision"
      << " enabled=" << (shadow.enabled ? 1 : 0)
      << " source=route_independent_measured_fullboard"
      << " selector_version=" << shadow.selector_version
      << " source_candidate=" << (shadow.source_candidate ? 1 : 0)
      << " current_auto_candidate="
      << (shadow.current_auto_candidate ? 1 : 0)
      << " current_review_hold=" << (shadow.current_review_hold ? 1 : 0)
      << " protected_review_hold="
      << (shadow.protected_review_hold ? 1 : 0)
      << " soft_center_conflict_review_hold="
      << (shadow.soft_center_conflict_review_hold ? 1 : 0)
      << " soft_center_alias_review_hold="
      << (shadow.soft_center_alias_review_hold ? 1 : 0)
      << " low_pulse_exactness_review_hold="
      << (shadow.low_pulse_exactness_review_hold ? 1 : 0)
      << " sparse_acapella_review_hold="
      << (shadow.sparse_acapella_review_hold ? 1 : 0)
      << " writer_override_candidate="
      << (shadow.writer_override_candidate ? 1 : 0)
      << " review_hold_promotion_candidate="
      << (shadow.review_hold_promotion_candidate ? 1 : 0)
      << " material_risk_local_exact_release_candidate="
      << (shadow.material_risk_local_exact_release_candidate ? 1 : 0)
      << " partial_bar_family_conflict_release_candidate="
      << (shadow.partial_bar_family_conflict_release_candidate ? 1 : 0)
      << " partial_bar_topk_exact_release_candidate="
      << (shadow.partial_bar_topk_exact_release_candidate ? 1 : 0)
      << " family_conflict_review_hold_candidate="
      << (shadow.family_conflict_review_hold_candidate ? 1 : 0)
      << " broad_route_conflict_review_hold_candidate="
      << (shadow.broad_route_conflict_review_hold_candidate ? 1 : 0)
      << " output_would_write=" << (shadow.output_would_write ? 1 : 0)
      << " output_would_review_hold="
      << (shadow.output_would_review_hold ? 1 : 0)
      << " route_matched=" << (shadow.route_matched ? 1 : 0)
      << " generic_route=" << (shadow.generic_route ? 1 : 0)
      << " current_bpm=" << format_float_locale(shadow.current_bpm, 3)
      << " output_bpm=" << format_float_locale(shadow.output_bpm, 3)
      << " prior_min=" << format_float_locale(shadow.prior_min_bpm, 3)
      << " prior_max=" << format_float_locale(shadow.prior_max_bpm, 3)
      << " prior_center=" << format_float_locale(shadow.prior_center_bpm, 3)
      << " prior_spread=" << format_float_locale(shadow.prior_spread_bpm, 3)
      << " measured_winner_bpm="
      << format_float_locale(shadow.measured_winner_bpm, 3)
      << " measured_winner_cluster_bpm="
      << format_float_locale(shadow.measured_winner_cluster_bpm, 3)
      << " measured_winner_base_score="
      << format_float_locale(shadow.measured_winner_base_score, 6)
      << " measured_runner_up_base_score="
      << format_float_locale(shadow.measured_runner_up_base_score, 6)
      << " measured_base_margin="
      << format_float_locale(shadow.measured_base_margin, 6)
      << " measured_winner_support="
      << format_float_locale(shadow.measured_winner_support, 6)
      << " measured_winner_winner_support="
      << format_float_locale(shadow.measured_winner_winner_support, 6)
      << " measured_winner_local_exact_score="
      << format_float_locale(shadow.measured_winner_local_exact_score, 6)
      << " current_local_exact_score="
      << format_float_locale(shadow.current_local_exact_score, 6)
      << " current_support="
      << format_float_locale(shadow.current_support, 6)
      << " candidate_bpm=" << format_float_locale(shadow.candidate_bpm, 3)
      << " candidate_cluster_bpm="
      << format_float_locale(shadow.candidate_cluster_bpm, 3)
      << " candidate_base_score="
      << format_float_locale(shadow.candidate_base_score, 6)
      << " candidate_support="
      << format_float_locale(shadow.candidate_support, 6)
      << " candidate_winner_support="
      << format_float_locale(shadow.candidate_winner_support, 6)
      << " candidate_local_exact_score="
      << format_float_locale(shadow.candidate_local_exact_score, 6)
      << " candidate_pulse_score="
      << format_float_locale(shadow.candidate_pulse_score, 6)
      << " candidate_pulse_section_support="
      << format_float_locale(shadow.candidate_pulse_section_support, 6)
      << " candidate_continuous_score="
      << format_float_locale(shadow.candidate_continuous_score, 6)
      << " candidate_continuous_section_support="
      << format_float_locale(shadow.candidate_continuous_section_support, 6)
      << " candidate_continuous_section_stability="
      << format_float_locale(shadow.candidate_continuous_section_stability, 6)
      << " candidate_continuous_score_ratio="
      << format_float_locale(shadow.candidate_continuous_score_ratio, 6)
      << " candidate_recovery_evidence_ratio="
      << format_float_locale(shadow.candidate_recovery_evidence_ratio, 6)
      << " candidate_current_delta="
      << format_float_locale(shadow.candidate_current_delta, 3)
      << " candidate_local_score_ratio="
      << format_float_locale(shadow.candidate_local_score_ratio, 6)
      << " candidate_support_ratio="
      << format_float_locale(shadow.candidate_support_ratio, 6)
      << " material_risk_probe_score_ratio="
      << format_float_locale(shadow.material_risk_probe_score_ratio, 6)
      << " material_risk_probe_top_span_bpm="
      << format_float_locale(shadow.material_risk_probe_top_span_bpm, 3)
      << " material_risk_probe_target_candidate_count="
      << static_cast<uint64_t>(
             shadow.material_risk_probe_target_candidate_count)
      << " material_risk_probe_non_boundary_top_count="
      << static_cast<uint64_t>(
             shadow.material_risk_probe_non_boundary_top_count)
      << " harmonic_center_improvement="
      << format_float_locale(shadow.harmonic_center_improvement, 3)
      << " harmonic_score_ratio="
      << format_float_locale(shadow.harmonic_score_ratio, 6)
      << " family_conflict_alternative_bpm="
      << format_float_locale(shadow.family_conflict_alternative_bpm, 3)
      << " family_conflict_center_improvement="
      << format_float_locale(shadow.family_conflict_center_improvement, 3)
      << " family_conflict_local_score_ratio="
      << format_float_locale(shadow.family_conflict_local_score_ratio, 6)
      << " family_conflict_base_score_ratio="
      << format_float_locale(shadow.family_conflict_base_score_ratio, 6)
      << " family_conflict_support="
      << format_float_locale(shadow.family_conflict_support, 6)
      << " family_conflict_winner_support="
      << format_float_locale(shadow.family_conflict_winner_support, 6)
      << " family_conflict_local_exact_score="
      << format_float_locale(shadow.family_conflict_local_exact_score, 6)
      << " family_conflict_pulse_score="
      << format_float_locale(shadow.family_conflict_pulse_score, 6)
      << " family_conflict_pulse_section_support="
      << format_float_locale(
             shadow.family_conflict_pulse_section_support, 6)
      << " family_conflict_continuous_score="
      << format_float_locale(shadow.family_conflict_continuous_score, 6)
      << " family_conflict_continuous_section_support="
      << format_float_locale(
             shadow.family_conflict_continuous_section_support, 6)
      << " family_conflict_continuous_score_ratio="
      << format_float_locale(
             shadow.family_conflict_continuous_score_ratio, 6)
      << " candidate_count=" << static_cast<uint64_t>(shadow.candidate_count)
      << " measured_winner_alias_classes="
      << (shadow.measured_winner_alias_classes.empty()
              ? "none"
              : shadow.measured_winner_alias_classes.c_str())
      << " candidate_alias_classes="
      << (shadow.candidate_alias_classes.empty()
              ? "none"
              : shadow.candidate_alias_classes.c_str())
      << " family_conflict_alias_classes="
      << (shadow.family_conflict_alias_classes.empty()
              ? "none"
              : shadow.family_conflict_alias_classes.c_str())
      << " action=" << shadow.action
      << " decision_class=" << shadow.decision_class
      << " reason=" << shadow.reason;
}

void log_hodgkinson_only_family_resolver_shadow(
    const char* trackLabel,
    const HodgkinsonOnlyFamilyResolverShadow& shadow) {
  pfc::string8 aliasOptions;
  const std::size_t optionCount =
      (std::min)(shadow.route_alias_option_count,
                 shadow.route_alias_options.size());
  for (std::size_t i = 0; i < optionCount; ++i) {
    if (i > 0) {
      aliasOptions << "|";
    }
    const HodgkinsonFamilyAliasOption& option = shadow.route_alias_options[i];
    aliasOptions << option.multiplier_class << ":"
                 << format_float_locale(option.bpm, 3);
  }
  if (aliasOptions.is_empty()) {
    aliasOptions = "none";
  }

  FB2K_console_formatter()
      << "foo_smart_tempo: [" << trackLabel
      << "] HodgkinsonOnlyFamilyResolverShadow"
      << " enabled=" << (shadow.enabled ? 1 : 0)
      << " candidate=" << (shadow.candidate ? 1 : 0)
      << " input_valid=" << (shadow.input_valid ? 1 : 0)
      << " input_bpm=" << format_float_locale(shadow.input_bpm, 3)
      << " route_min=" << format_float_locale(shadow.route_min_bpm, 3)
      << " route_max=" << format_float_locale(shadow.route_max_bpm, 3)
      << " route_alias_option_count="
      << static_cast<uint64_t>(shadow.route_alias_option_count)
      << " route_alias_options=" << aliasOptions.get_ptr()
      << " unique_alias_bpm="
      << format_float_locale(shadow.unique_alias_bpm, 3)
      << " unique_alias_multiplier="
      << format_float_locale(shadow.unique_alias_multiplier, 6)
      << " unique_alias_class=" << shadow.unique_alias_class
      << " center_alias_bpm="
      << format_float_locale(shadow.center_alias_bpm, 3)
      << " center_alias_multiplier="
      << format_float_locale(shadow.center_alias_multiplier, 6)
      << " center_alias_class=" << shadow.center_alias_class
      << " center_distance_bpm="
      << format_float_locale(shadow.center_distance_bpm, 3)
      << " family_score=" << format_float_locale(shadow.family_score, 6)
      << " family_score_dominance="
      << format_float_locale(shadow.family_score_dominance, 6)
      << " runner_up_score_ratio="
      << format_float_locale(shadow.runner_up_score_ratio, 6)
      << " consensus_support="
      << format_float_locale(shadow.consensus_support, 6)
      << " decision_class=" << shadow.decision_class
      << " reason=" << shadow.reason;
}

void log_analysis_sample_rate(unsigned sourceSampleRate,
                              unsigned analysisSampleRate,
                              unsigned maxAnalysisRate,
                              bool resampled,
                              double dynamicMinIoiSeconds,
                              uint64_t dynamicMinIoiSamples) {
  FB2K_console_formatter()
      << "foo_smart_tempo: analysis sample rate source=" << sourceSampleRate
      << "Hz, target=" << analysisSampleRate << "Hz, max=" << maxAnalysisRate
      << "Hz, policy=fixed-production-48k"
      << ", final_analysis_sr=" << analysisSampleRate
      << ", resampled=" << (resampled ? 1 : 0)
      << ", resampler=" << (resampled ? "foobar-preferred-hq" : "native")
      << ", minioi_s=" << format_float_locale(dynamicMinIoiSeconds, 6)
      << ", minioi_samples=" << dynamicMinIoiSamples;
}

void log_telemetry_header(const char* trackLabel,
                          unsigned inputSampleRate,
                          unsigned analysisSampleRate,
                          unsigned winSize,
                          unsigned hopSize,
                          double dynamicMinIoiSeconds,
                          uint64_t dynamicMinIoiSamples) {
  FB2K_console_formatter()
      << "foo_smart_tempo: [" << trackLabel
      << "] Telemetry Header: engine=hodgkinson-mir"
      << ", primarySelector=measured-candidate"
      << ", fullBoardPolicy=soft-center"
      << ", input_sample_rate=" << inputSampleRate
      << ", analysis_sample_rate=" << analysisSampleRate
      << ", buf_size=" << static_cast<uint64_t>(winSize)
      << ", hop_size=" << static_cast<uint64_t>(hopSize)
      << ", minioi_s=" << format_float_locale(dynamicMinIoiSeconds, 6)
      << ", minioi_samples=" << dynamicMinIoiSamples;
}

void log_decision_summary(const char* trackLabel,
                          const DecisionSummaryLogSnapshot& snapshot) {
  const pfc::string8 rawGlobalText =
      snapshot.rawGlobalValid ? pfc::string8(format_float_locale(snapshot.rawGlobalBpm, 2))
                              : pfc::string8("n/a");
  const IBpmAnalyzer::RoutingLogContext emptyRoutingContext;
  const IBpmAnalyzer::RoutingLogContext& routing =
      snapshot.routingContext != nullptr ? *snapshot.routingContext : emptyRoutingContext;
  const pfc::string8 confidenceText =
      (std::isfinite(snapshot.confidence) && snapshot.confidence >= 0.0)
          ? pfc::string8(format_float_locale(snapshot.confidence, 1))
          : pfc::string8("n/a");
  FB2K_console_formatter()
      << "foo_smart_tempo: [" << trackLabel
      << "] Decision Summary: raw_global=" << rawGlobalText
      << ", raw_projected="
      << (snapshot.rawGlobalValid ? format_float_locale(snapshot.rawProjectedBpm, 2)
                                  : pfc::string8("n/a"))
      << ", policy_input=" << format_float_locale(snapshot.policyInputBpm, 2)
      << ", projected=" << format_float_locale(snapshot.projectedBpm, 2)
      << ", final=" << format_float_locale(snapshot.finalBpm, 2)
      << ", route=\"" << snapshot.routeName << "\""
      << ", route_center="
      << format_float_locale(routing.is_generic_match
                                ? 0.0
                                : routing.primary_center_bpm > 0.0
                                ? routing.primary_center_bpm
                                : (snapshot.targetMinBpm + snapshot.targetMaxBpm) * 0.5,
                            2)
      << ", route_spread="
      << format_float_locale(routing.is_generic_match
                                ? 0.0
                                : routing.primary_spread_bpm > 0.0
                                ? routing.primary_spread_bpm
                                : (snapshot.targetMaxBpm - snapshot.targetMinBpm) * 0.5,
                            2)
      << ", multiplier=" << format_float_locale(snapshot.projected.multiplier, 6)
      << ", reason=" << snapshot.projected.reason
      << ", policy_path=" << snapshot.policyPathText
      << ", policy_reason=" << policy_projection_reason_name(snapshot.policyReason)
      << ", source_genres=\"" << format_decision_log_value(routing.source_genres) << "\""
      << ", normalized_genres=\""
      << format_decision_log_value(routing.normalized_genres) << "\""
      << ", matched_rule=\"" << format_decision_log_value(routing.matched_rule) << "\""
      << ", matched_token=\"" << format_decision_log_value(routing.matched_token) << "\""
      << ", route_matched=" << (routing.route_matched ? 1 : 0)
      << ", is_generic_match=" << (routing.is_generic_match ? 1 : 0)
      << ", rule_index=" << static_cast<uint64_t>(routing.rule_index)
      << ", specificity_words=" << static_cast<uint64_t>(routing.specificity_words)
      << ", specificity_norm_len=" << static_cast<uint64_t>(routing.specificity_norm_len)
      << ", candidates_considered="
      << static_cast<uint64_t>(routing.candidates_considered)
      << ", confidence=" << confidenceText;
}

void log_standard_decision(const char* trackLabel,
                           const DecisionSummaryLogSnapshot& snapshot,
                           bool uncertainResult,
                           const char* decisionClass) {
  const pfc::string8 rawGlobalText =
      snapshot.rawGlobalValid ? pfc::string8(format_float_locale(snapshot.rawGlobalBpm, 2))
                              : pfc::string8("n/a");
  const pfc::string8 rawProjectedText =
      snapshot.rawGlobalValid ? pfc::string8(format_float_locale(snapshot.rawProjectedBpm, 2))
                              : pfc::string8("n/a");
  const pfc::string8 confidenceText =
      (std::isfinite(snapshot.confidence) && snapshot.confidence >= 0.0)
          ? pfc::string8(format_float_locale(snapshot.confidence, 1))
          : pfc::string8("n/a");
  const IBpmAnalyzer::RoutingLogContext emptyRoutingContext;
  const IBpmAnalyzer::RoutingLogContext& routing =
      snapshot.routingContext != nullptr ? *snapshot.routingContext : emptyRoutingContext;
  FB2K_console_formatter()
      << "foo_smart_tempo: [" << trackLabel
      << "] Decision: raw_global=" << rawGlobalText
      << ", raw_projected=" << rawProjectedText
      << ", policy_input=" << format_float_locale(snapshot.policyInputBpm, 2)
      << ", projected=" << format_float_locale(snapshot.projectedBpm, 2)
      << ", final=" << format_float_locale(snapshot.finalBpm, 2)
      << ", route=\"" << snapshot.routeName << "\""
      << ", route_center="
      << format_float_locale(routing.is_generic_match
                                ? 0.0
                                : routing.primary_center_bpm > 0.0
                                ? routing.primary_center_bpm
                                : (snapshot.targetMinBpm + snapshot.targetMaxBpm) * 0.5,
                            2)
      << ", route_spread="
      << format_float_locale(routing.is_generic_match
                                ? 0.0
                                : routing.primary_spread_bpm > 0.0
                                ? routing.primary_spread_bpm
                                : (snapshot.targetMaxBpm - snapshot.targetMinBpm) * 0.5,
                            2)
      << ", multiplier=" << format_float_locale(snapshot.projected.multiplier, 6)
      << ", reason=" << snapshot.projected.reason
      << ", policy_path=" << snapshot.policyPathText
      << ", policy_reason=" << policy_projection_reason_name(snapshot.policyReason)
      << ", confidence=" << confidenceText
      << ", uncertain=" << (uncertainResult ? 1 : 0)
      << ", decision_class=" << (decisionClass != nullptr ? decisionClass : "unknown");
}

void log_track_timing(const char* trackLabel,
                      const TrackTimingLogSnapshot& snapshot) {
  FB2K_console_formatter()
      << "foo_smart_tempo: [" << trackLabel
      << "] Track Timing: decode_ms="
      << format_float_locale(snapshot.decodeSec * 1000.0, 3)
      << ", onset_ms=" << format_float_locale(snapshot.onsetSec * 1000.0, 3)
      << ", total_ms=" << format_float_locale(snapshot.totalSec * 1000.0, 3);
}

void log_mir_local_exact_cache(const char* trackLabel,
                               std::size_t resultHits,
                               std::size_t resultMisses,
                               std::size_t scoreHits,
                               std::size_t scoreMisses) {
  FB2K_console_formatter()
      << "foo_smart_tempo: [" << trackLabel
      << "] MirLocalExactCache result_hits=" << resultHits
      << " result_misses=" << resultMisses
      << " score_hits=" << scoreHits
      << " score_misses=" << scoreMisses;
}

}  // namespace smart_tempo
