#pragma once

namespace smart_tempo {

// Canonical persisted/UI choices for MIR sampling. Keep preferences, UI and
// synthetic tests on this single source of truth.
inline constexpr int kDefaultAnalysisSecondsToRead = 20;
inline constexpr int kDefaultAnalysisSamplePasses = 50;
inline constexpr int kAnalysisSecondsChoices[] = {5, 10, 15, 20, 30, 45, 60, 90};
inline constexpr int kAnalysisSamplePassChoices[] = {1, 3, 5, 10, 20, 50};

inline constexpr bool is_supported_analysis_seconds(int value) noexcept {
  for (const int choice : kAnalysisSecondsChoices) {
    if (choice == value) return true;
  }
  return false;
}

inline constexpr bool is_supported_analysis_sample_passes(int value) noexcept {
  for (const int choice : kAnalysisSamplePassChoices) {
    if (choice == value) return true;
  }
  return false;
}

// Shared decode and sampling-window boundaries for the MIR pipeline.
inline constexpr int kAnalysisOffsetMinPct = 20;
inline constexpr int kAnalysisOffsetMaxPct = 80;
inline constexpr double kMinimumTrackLengthSeconds = 3.0;
inline constexpr double kDecodeSafetyMarginSeconds = 1.0;
inline constexpr double kPassEndSafetyMarginSeconds = 0.5;

} // namespace smart_tempo
