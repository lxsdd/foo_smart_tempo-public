#pragma once

// Source-only, bounded estimate of decoder work implied by the *existing*
// analysis window scheduler. This is not elapsed time, CPU time, onset work,
// MIR work, or an accuracy claim. Never select or alter BPM candidates here.
#include "analysis_pass_schedule.h"
#include "analysis_window_config.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

namespace smart_tempo {

struct AnalysisWindowWorkEstimate {
  bool valid = false;
  bool coverageKnown = false;
  std::size_t passCount = 0;
  std::size_t distinctStarts = 0;
  double secondsPerPass = 0.0;
  double requestedDecodeSeconds = 0.0;
  double uniqueCoverageSeconds = 0.0;
  double redundantDecodeSeconds = 0.0;
};

// Known lengths permit exact *scheduled* interval coverage, not a guarantee
// that a decoder produced all requested frames. For seekless/unknown-length
// inputs, the existing scheduler uses one pass and actual coverage is unknown.
// O(1) memory; no media, metadata or runtime worker state is inspected.
inline AnalysisWindowWorkEstimate estimate_analysis_window_work(
    int secondsToRead, int requestedPasses, bool knownLength,
    double trackSeconds) noexcept {
  AnalysisWindowWorkEstimate result;
  // Bound malformed inputs independently of the normal preferences clamps.
  constexpr int kMaxInspectablePasses = 10000;
  if (secondsToRead <= 0 || requestedPasses <= 0 ||
      requestedPasses > kMaxInspectablePasses ||
      (knownLength && (!std::isfinite(trackSeconds) || trackSeconds <= 0.0))) {
    return result;
  }

  const auto plan = make_analysis_window_plan(
      secondsToRead, requestedPasses, knownLength, trackSeconds,
      kDecodeSafetyMarginSeconds);
  if (!plan.valid() || plan.samplePasses <= 0 ||
      !std::isfinite(plan.secondsToRead) || plan.secondsToRead <= 0.0) {
    return result;
  }

  result.passCount = static_cast<std::size_t>(plan.samplePasses);
  result.secondsPerPass = plan.secondsToRead;
  result.requestedDecodeSeconds = plan.secondsToRead * result.passCount;
  if (!std::isfinite(result.requestedDecodeSeconds)) return {};

  if (!knownLength) {
    result.valid = true;
    result.coverageKnown = false;
    return result;
  }

  double coveredUntil = 0.0;
  double previousStart = 0.0;
  bool first = true;
  for (int pass = 0; pass < plan.samplePasses; ++pass) {
    const auto offset = compute_analysis_pass_offset(
        pass, plan.samplePasses, true, trackSeconds, plan.secondsToRead,
        kAnalysisOffsetMinPct, kAnalysisOffsetMaxPct,
        kPassEndSafetyMarginSeconds);
    const double start = offset.startSec;
    const double end = start + plan.secondsToRead;
    if (!std::isfinite(start) || !std::isfinite(end) || start < 0.0 ||
        end <= start || (!first && start + 1e-6 < previousStart)) {
      return {};
    }
    if (first || std::abs(start - previousStart) > 1e-6)
      ++result.distinctStarts;
    result.uniqueCoverageSeconds +=
        (std::max)(0.0, end - (std::max)(start, coveredUntil));
    coveredUntil = (std::max)(coveredUntil, end);
    previousStart = start;
    first = false;
  }

  result.redundantDecodeSeconds =
      (std::max)(0.0, result.requestedDecodeSeconds -
                          result.uniqueCoverageSeconds);
  result.valid = std::isfinite(result.uniqueCoverageSeconds) &&
                 result.uniqueCoverageSeconds > 0.0 &&
                 result.uniqueCoverageSeconds <=
                     result.requestedDecodeSeconds + 1e-6;
  result.coverageKnown = result.valid;
  return result;
}

}  // namespace smart_tempo
