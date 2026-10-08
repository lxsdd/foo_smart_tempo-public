// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cmath>
#include <cstddef>
#include <span>

namespace smart_tempo {

// An onset gate should reject one dominant transient, not a regular sequence
// of comparably weighted beats. Counting peaks strictly above their arithmetic
// mean incorrectly rejects an equal-amplitude click track (zero exceedances).
//
// The effective-event count (inverse concentration / participation ratio)
// equals N for N equally weighted positive peaks and approaches 1 when only
// one transient carries the positive onset energy. This gate does not select
// a BPM, alter MIR thresholds, or synthesize any candidates.
[[nodiscard]] inline bool mir_is_single_effective_onset(
    std::size_t peakCount, std::span<const float> magnitudes) noexcept {
  if (peakCount == 0 || magnitudes.empty() ||
      peakCount != magnitudes.size()) {
    return true;
  }

  double total = 0.0;
  double squaredTotal = 0.0;
  for (const float value : magnitudes) {
    if (!std::isfinite(value)) {
      return true;  // Invalid input must not become a measured candidate.
    }
    if (value > 0.0f) {
      const double positive = static_cast<double>(value);
      total += positive;
      squaredTotal += positive * positive;
    }
  }
  if (!(total > 0.0) || !(squaredTotal > 0.0) ||
      !std::isfinite(total) || !std::isfinite(squaredTotal)) {
    return true;
  }

  // Below 1.5 effective events, one positive onset dominates; retain the
  // original guard's intent while removing its equal-height false rejection.
  constexpr double kSingleEventEffectiveCount = 1.5;
  const double effectiveCount = (total * total) / squaredTotal;
  return !std::isfinite(effectiveCount) ||
         effectiveCount < kSingleEventEffectiveCount;
}

}  // namespace smart_tempo
