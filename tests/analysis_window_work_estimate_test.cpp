// Deterministic source-only baseline for honest ETA work budgeting.
// These are scheduled audio intervals, not measured CPU/decoder/MIR durations.
#include "../src/foo_smart_tempo/analysis_window_work_estimate.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>

namespace {
void require(bool ok, const char* label) {
  if (!ok) {
    std::cerr << "FAIL_WINDOW_WORK_ESTIMATE_" << label << '\n';
    std::exit(EXIT_FAILURE);
  }
}
bool near(double actual, double expected, double tolerance = 1e-6) {
  return std::abs(actual - expected) <= tolerance;
}
}  // namespace

int main() {
  using smart_tempo::estimate_analysis_window_work;

  const auto shortTrack = estimate_analysis_window_work(20, 50, true, 20.0);
  require(shortTrack.valid && shortTrack.coverageKnown, "SHORT_VALID");
  require(shortTrack.passCount == 50 && shortTrack.distinctStarts == 1,
          "SHORT_DUPLICATE_STARTS");
  require(near(shortTrack.requestedDecodeSeconds, 950.0) &&
              near(shortTrack.uniqueCoverageSeconds, 19.0) &&
              near(shortTrack.redundantDecodeSeconds, 931.0),
          "SHORT_DUPLICATE_WORK");

  const auto medium = estimate_analysis_window_work(20, 50, true, 180.0);
  require(medium.valid && medium.coverageKnown, "MEDIUM_VALID");
  require(medium.passCount == 50 && medium.distinctStarts == 50,
          "MEDIUM_DISTINCT_STARTS");
  require(near(medium.requestedDecodeSeconds, 1000.0) &&
              near(medium.uniqueCoverageSeconds, 128.0, 0.001) &&
              near(medium.redundantDecodeSeconds, 872.0, 0.001),
          "MEDIUM_OVERLAP_WORK");

  const auto single = estimate_analysis_window_work(20, 1, true, 180.0);
  require(single.valid && single.passCount == 1 &&
              single.distinctStarts == 1 &&
              near(single.requestedDecodeSeconds, 20.0) &&
              near(single.uniqueCoverageSeconds, 20.0) &&
              near(single.redundantDecodeSeconds, 0.0),
          "SINGLE_PASS");

  const auto seekless = estimate_analysis_window_work(20, 50, false, 0.0);
  require(seekless.valid && !seekless.coverageKnown &&
              seekless.passCount == 1 &&
              near(seekless.requestedDecodeSeconds, 20.0) &&
              near(seekless.uniqueCoverageSeconds, 0.0),
          "UNKNOWN_LENGTH_NO_FALSE_COVERAGE");

  require(!estimate_analysis_window_work(0, 50, true, 180.0).valid,
          "ZERO_WINDOW");
  require(!estimate_analysis_window_work(20, 0, true, 180.0).valid,
          "ZERO_PASSES");
  require(!estimate_analysis_window_work(20, 10001, true, 180.0).valid,
          "EXCESSIVE_PASSES");
  require(!estimate_analysis_window_work(
               20, 50, true, std::numeric_limits<double>::quiet_NaN()).valid,
          "NAN_LENGTH");
  require(!estimate_analysis_window_work(
               20, 50, true, std::numeric_limits<double>::infinity()).valid,
          "INFINITE_LENGTH");
  require(!estimate_analysis_window_work(20, 50, true, -1.0).valid,
          "NEGATIVE_LENGTH");

  std::cout << "PASS_WINDOW_WORK_ESTIMATE\n";
}
