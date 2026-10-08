// Synthetic, header-only scheduling tests. No user audio or BPM reference.
#include "../src/foo_smart_tempo/analysis_pass_schedule.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {
void require(bool ok, const char* message) {
  if (!ok) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}
bool near(double a, double b) { return std::abs(a - b) < 0.0001; }
}

int main() {
  using namespace smart_tempo;
  const auto original = make_analysis_window_plan(20, 50, true, 180.0, 1.0);
  require(original.valid() && near(original.secondsToRead, 20.0) &&
              original.samplePasses == 50, "backward-compatible defaults");

  const auto quicker = make_analysis_window_plan(10, 5, true, 180.0, 1.0);
  require(quicker.valid() && near(quicker.secondsToRead, 10.0) &&
              quicker.samplePasses == 5, "shorter user window and fewer passes");

  const auto unknown = make_analysis_window_plan(30, 20, false, 0.0, 1.0);
  require(unknown.valid() && near(unknown.secondsToRead, 30.0) &&
              unknown.samplePasses == 1, "unknown duration never seeks many passes");

  const auto brief = make_analysis_window_plan(60, 50, true, 12.0, 1.0);
  require(brief.valid() && near(brief.secondsToRead, 11.0),
          "short track retains safe decoding margin");

  const auto invalid = make_analysis_window_plan(0, 3, true, 60.0, 1.0);
  require(!invalid.valid(), "reject a nonpositive decode window");

  const auto first = compute_analysis_pass_offset(
      0, 5, true, 180.0, 10.0, 20, 80, 0.5);
  const auto last = compute_analysis_pass_offset(
      4, 5, true, 180.0, 10.0, 20, 80, 0.5);
  require(near(first.startSec, 36.0) && last.startSec > first.startSec &&
              last.startSec <= 169.5, "offsets stay ordered and inside track");

  const auto endGuard = compute_analysis_pass_offset(
      49, 50, true, 12.0, 11.0, 20, 80, 0.5);
  require(endGuard.startSec <= 0.5, "end margin clamps short-track offset");

  std::cout << "PASS_SYNTHETIC_MIR_WINDOWS" << std::endl;
  return EXIT_SUCCESS;
}
