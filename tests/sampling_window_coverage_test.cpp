// Public, source-derived geometry baseline for the existing MIR scheduler.
// No test sound, private song labels, thresholds, or candidate expectations.
// This test intentionally reports sampling redundancy without changing DSP.
#include "../src/foo_smart_tempo/analysis_pass_schedule.h"
#include "../src/foo_smart_tempo/analysis_window_config.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <utility>
#include <vector>

namespace {
struct Coverage {
  int passes = 0;
  int distinct_starts = 0;
  double processed_seconds = 0.0;
  double unique_seconds = 0.0;
  double redundancy = 0.0;
};

Coverage measure(double track_seconds, int seconds_per_pass = 20,
                 int passes = 50) {
  using namespace smart_tempo;
  const auto plan = make_analysis_window_plan(
      seconds_per_pass, passes, true, track_seconds,
      kDecodeSafetyMarginSeconds);
  if (!plan.valid()) std::abort();
  std::vector<std::pair<double, double>> windows;
  for (int i = 0; i < plan.samplePasses; ++i) {
    const auto offset = compute_analysis_pass_offset(
        i, plan.samplePasses, true, track_seconds, plan.secondsToRead,
        kAnalysisOffsetMinPct, kAnalysisOffsetMaxPct,
        kPassEndSafetyMarginSeconds);
    windows.emplace_back(offset.startSec,
                         offset.startSec + plan.secondsToRead);
  }
  std::sort(windows.begin(), windows.end());
  Coverage result;
  result.passes = static_cast<int>(windows.size());
  result.processed_seconds = plan.secondsToRead * result.passes;
  double end = 0.0;
  double prev = -1.0;
  for (const auto& window : windows) {
    if (prev < 0 || std::abs(window.first - prev) > 0.00001)
      ++result.distinct_starts;
    prev = window.first;
    result.unique_seconds +=
        std::max(0.0, window.second - std::max(window.first, end));
    end = std::max(end, window.second);
  }
  result.redundancy = result.processed_seconds / result.unique_seconds;
  return result;
}

void require(bool valid, const char* name) {
  if (!valid) {
    std::cerr << "FAIL_SAMPLING_GEOMETRY_" << name << '\n';
    std::exit(EXIT_FAILURE);
  }
}
}  // namespace

int main() {
  const auto short_track = measure(20.0);
  require(short_track.passes == 50, "SHORT_PASS_COUNT");
  require(short_track.distinct_starts == 1, "SHORT_DUPLICATE_STARTS");
  require(std::abs(short_track.processed_seconds - 950.0) < 0.0001,
          "SHORT_TOTAL_DECODE");
  require(std::abs(short_track.unique_seconds - 19.0) < 0.0001,
          "SHORT_UNIQUE_COVERAGE");
  require(std::abs(short_track.redundancy - 50.0) < 0.0001,
          "SHORT_REDUNDANCY");

  const auto medium_track = measure(180.0);
  require(medium_track.passes == 50, "MEDIUM_PASS_COUNT");
  require(medium_track.distinct_starts == 50, "MEDIUM_DISTINCT_STARTS");
  require(std::abs(medium_track.processed_seconds - 1000.0) < 0.0001,
          "MEDIUM_TOTAL_DECODE");
  require(std::abs(medium_track.unique_seconds - 128.0) < 0.001,
          "MEDIUM_UNIQUE_COVERAGE");
  require(medium_track.redundancy > 7.80 &&
              medium_track.redundancy < 7.82,
          "MEDIUM_REDUNDANCY");

  const auto single_pass = measure(180.0, 20, 1);
  require(single_pass.passes == 1 &&
              single_pass.distinct_starts == 1 &&
              std::abs(single_pass.redundancy - 1.0) < 1e-9,
          "SINGLE_PASS");
  // Same 20-second synthetic fixture versus a 120-second independent span.
  // Repeated starts are NOT independent evidence; this is pure scheduler
  // geometry, not measured CPU/RAM or an accuracy-based deduplication gate.
  for (const int n : std::array<int, 4>{1, 5, 20, 50}) {
    const auto brief = measure(20.0, 20, n);
    require(brief.passes == n && brief.distinct_starts == 1,
            "SHORT_PASS_SWEEP_STARTS");
    require(std::abs(brief.processed_seconds - 19.0 * n) < 0.0001 &&
                std::abs(brief.unique_seconds - 19.0) < 0.0001,
            "SHORT_PASS_SWEEP_SECONDS");
    const auto longer = measure(120.0, 20, n);
    require(longer.passes == n && longer.distinct_starts == n,
            "LONG_PASS_SWEEP_STARTS");
    const double expectedUnique = n == 1 ? 20.0 : 92.0;
    require(std::abs(longer.processed_seconds - 20.0 * n) < 0.0001 &&
                std::abs(longer.unique_seconds - expectedUnique) < 0.001,
            "LONG_PASS_SWEEP_SECONDS");
    std::cout << "SAMPLING_20S_" << n << "P_DISTINCT_STARTS="
              << brief.distinct_starts << '\n';
    std::cout << "SAMPLING_20S_" << n << "P_DUPLICATE_START_FRACTION="
              << 1.0 - static_cast<double>(brief.distinct_starts) / n << '\n';
    std::cout << "SAMPLING_120S_" << n << "P_DISTINCT_STARTS="
              << longer.distinct_starts << '\n';
    std::cout << "SAMPLING_120S_" << n << "P_DUPLICATE_START_FRACTION="
              << 1.0 - static_cast<double>(longer.distinct_starts) / n << '\n';
  }
  std::cout << "SAMPLING_20S_50P_UNIQUE_WINDOWS="
            << short_track.distinct_starts << '\n';
  std::cout << "SAMPLING_20S_50P_AUDIO_OVERLAP_FACTOR="
            << short_track.redundancy << '\n';
  std::cout << "SAMPLING_180S_50P_AUDIO_OVERLAP_FACTOR="
            << medium_track.redundancy << '\n';
  std::cout << "PASS_SAMPLING_WINDOW_COVERAGE_BASELINE\n";
}
