#include "stdafx.h"

#include "analysis_decision_support.h"

namespace smart_tempo {

const char* policy_projection_reason_name(PolicyProjectionReason reason) noexcept {
  switch (reason) {
    case PolicyProjectionReason::kept_native:
      return "kept_native";
    case PolicyProjectionReason::projected_octave:
      return "projected_octave";
    case PolicyProjectionReason::projected_harmonic:
      return "projected_harmonic";
    case PolicyProjectionReason::uncertain_mode_conflict:
      return "uncertain_mode_conflict";
    case PolicyProjectionReason::uncertain_low_confidence:
      return "uncertain_low_confidence";
    case PolicyProjectionReason::uncertain_boundary_projection:
      return "uncertain_boundary_projection";
    case PolicyProjectionReason::uncertain_weak_non_octave_rescue:
      return "uncertain_weak_non_octave_rescue";
    case PolicyProjectionReason::uncertain_weak_non_octave_rescue_blocked:
      return "uncertain_weak_non_octave_rescue_blocked";
    case PolicyProjectionReason::uncertain_alternate_route_rescue:
      return "uncertain_alternate_route_rescue";
    case PolicyProjectionReason::none:
    default:
      return "none";
  }
}

}  // namespace smart_tempo
