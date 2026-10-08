#pragma once

namespace smart_tempo {

// Shared decode and sampling-window boundaries for the MIR pipeline.
inline constexpr int kAnalysisOffsetMinPct = 20;
inline constexpr int kAnalysisOffsetMaxPct = 80;
inline constexpr double kMinimumTrackLengthSeconds = 3.0;
inline constexpr double kDecodeSafetyMarginSeconds = 1.0;
inline constexpr double kPassEndSafetyMarginSeconds = 0.5;

} // namespace smart_tempo
