#pragma once

namespace smart_tempo {

struct FoldingResult {
  double bpm = 0.0;
  double multiplier = 1.0;
  const char* reason = "native";
};

enum class PolicyProjectionReason {
  none,
  kept_native,
  projected_octave,
  projected_harmonic,
  uncertain_mode_conflict,
  uncertain_low_confidence,
  uncertain_boundary_projection,
  uncertain_weak_non_octave_rescue,
  uncertain_weak_non_octave_rescue_blocked,
  uncertain_alternate_route_rescue,
};

}  // namespace smart_tempo
