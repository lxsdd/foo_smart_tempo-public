#pragma once

// Header-only, source-agnostic diagnostics for the MIR candidate frontend.
// These counters classify where candidates became unavailable. They neither
// manufacture tempo evidence nor influence the selector or writer.
#include <cstddef>

namespace smart_tempo {

struct MirFrontendCoverage {
  std::size_t odf_frames = 0;
  std::size_t odf_peaks = 0;
  std::size_t complete_tatum_keys = 0;
  std::size_t complete_division_hypotheses = 0;
  std::size_t partial_tatum_keys = 0;
  std::size_t partial_division_hypotheses = 0;
  int fitted_complete_tatums = 0;
  std::size_t complete_candidate_rows = 0;
  std::size_t partial_candidate_rows = 0;
};

[[nodiscard]] constexpr const char*
classify_mir_frontend_coverage(const MirFrontendCoverage& stats) noexcept {
  if (stats.complete_candidate_rows > 0) {
    return "complete_candidates_present";
  }
  if (stats.partial_candidate_rows > 0) {
    return "partial_candidates_without_complete";
  }
  if (stats.odf_frames < 8) {
    return "odf_unavailable";
  }
  if (stats.odf_peaks < 3) {
    return "insufficient_onsets";
  }
  if (stats.complete_division_hypotheses == 0) {
    return stats.partial_division_hypotheses > 0
               ? "partial_geometry_only"
               : "no_geometry";
  }
  if (stats.fitted_complete_tatums <= 0) {
    return stats.partial_division_hypotheses > 0
               ? "full_tatum_fit_unavailable_partial_geometry_exists"
               : "full_tatum_fit_unavailable";
  }
  return "fit_without_candidate_rows";
}

}  // namespace smart_tempo
