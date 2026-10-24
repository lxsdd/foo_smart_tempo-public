#pragma once

// Diagnostic-only classification for an MIR analysis that did not produce
// a writable BPM. Never feeds a candidate, changes the write safety gate,
// or reads metadata or evaluation references. No raw paths or track labels.
#include <cstddef>

namespace smart_tempo {

// These are UI-facing, source-stage indicators, not a proof of why a
// rhythm estimator failed or a reason to bypass a safe review hold.
[[nodiscard]] constexpr const char* mir_no_write_stage_message(
    std::size_t phase_onset_peaks,
    std::size_t scored_segments,
    std::size_t raw_measured_candidates,
    bool safety_review_hold) noexcept {
  if (raw_measured_candidates == 0) {
    if (phase_onset_peaks < 2) {
      return "MIR: insufficient onset evidence; BPM write blocked";
    }
    if (scored_segments == 0) {
      return "MIR: no scored rhythm segments; BPM write blocked";
    }
    return "MIR: measured candidate board empty; BPM write blocked";
  }
  if (safety_review_hold) {
    return "MIR: ambiguous candidates; safety review hold";
  }
  return "MIR: no admissible primary candidate; BPM write blocked";
}

} // namespace smart_tempo
