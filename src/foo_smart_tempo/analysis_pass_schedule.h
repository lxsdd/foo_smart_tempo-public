#pragma once

#include <algorithm>

namespace smart_tempo {

struct AnalysisWindowPlan {
  double secondsToRead = 0.0;
  int samplePasses = 1;
  const char* error = nullptr;

  [[nodiscard]] bool valid() const noexcept { return error == nullptr; }
};

inline AnalysisWindowPlan make_analysis_window_plan(
    int secondsToRead, int samplePasses, bool hasKnownTrackLength,
    double trackLengthSeconds, double decodeSafetyMarginSeconds) noexcept {
  AnalysisWindowPlan plan;
  plan.secondsToRead = static_cast<double>(secondsToRead);
  plan.samplePasses = samplePasses;

  if (hasKnownTrackLength) {
    plan.secondsToRead = (std::min)(plan.secondsToRead, trackLengthSeconds);
  }
  if (hasKnownTrackLength && trackLengthSeconds > decodeSafetyMarginSeconds) {
    plan.secondsToRead =
        (std::min)(plan.secondsToRead,
                   trackLengthSeconds - decodeSafetyMarginSeconds);
  } else if (!hasKnownTrackLength) {
    plan.samplePasses = 1;
  }

  if (!(plan.secondsToRead > 0.0)) {
    plan.error = "Invalid analysis window";
  }
  return plan;
}

struct AnalysisPassOffset {
  double startSec = 0.0;
  double offsetPct = 0.0;
};

inline AnalysisPassOffset compute_analysis_pass_offset(
    int pass, int samplePasses, bool hasKnownTrackLength,
    double trackLengthSeconds, double secondsToRead, int offsetMinPct,
    int offsetMaxPct, double passEndSafetyMarginSeconds) noexcept {
  AnalysisPassOffset offset;
  if (!hasKnownTrackLength) {
    return offset;
  }

  const double offsetRange =
      static_cast<double>(offsetMaxPct - offsetMinPct);
  const double t = (samplePasses <= 1)
                       ? 0.5
                       : (static_cast<double>(pass) /
                          static_cast<double>(samplePasses - 1));
  offset.offsetPct = offsetMinPct + offsetRange * t;
  if (offset.offsetPct >= offsetMaxPct) {
    offset.offsetPct = (std::max)(0.0, offsetMaxPct - 0.000001);
  }

  offset.startSec = trackLengthSeconds * (offset.offsetPct / 100.0);
  const double maxAllowedStartSec =
      (std::max)(0.0, trackLengthSeconds - secondsToRead -
                          passEndSafetyMarginSeconds);
  offset.startSec = std::clamp(offset.startSec, 0.0, maxAllowedStartSec);
  return offset;
}

}  // namespace smart_tempo
