// Public, synthetic-only test: no private audio, tracks, reference BPMs.
#include "../src/foo_smart_tempo/mir_hold_diagnostics.h"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {
void expect_stage(std::size_t peaks, std::size_t scored,
                  std::size_t candidates, bool hold,
                  std::string_view message, const char* name) {
  const std::string_view observed = smart_tempo::mir_no_write_stage_message(
      peaks, scored, candidates, hold);
  if (observed != message) {
    std::cerr << "FAILED synthetic stage: " << name << '\n';
    std::exit(EXIT_FAILURE);
  }
}
} // namespace

int main() {
  expect_stage(0, 0, 0, false,
               "MIR: insufficient onset evidence; BPM write blocked",
               "no onset evidence");
  expect_stage(1, 1, 0, true,
               "MIR: insufficient onset evidence; BPM write blocked",
               "single onset still insufficient");
  expect_stage(30, 0, 0, false,
               "MIR: no scored rhythm segments; BPM write blocked",
               "no viable segments");
  expect_stage(400, 15, 0, false,
               "MIR: measured candidate board empty; BPM write blocked",
               "empty board despite rich onset evidence");
  expect_stage(400, 15, 3, true,
               "MIR: ambiguous candidates; safety review hold",
               "nonempty board with safety hold");
  expect_stage(400, 15, 3, false,
               "MIR: no admissible primary candidate; BPM write blocked",
               "nonempty board without admissible selection");
  expect_stage(0, 0, 3, true,
               "MIR: ambiguous candidates; safety review hold",
               "measured candidates must take precedence");
  std::cout << "PASS_SYNTHETIC_MIR_HOLD_STAGES" << std::endl;
  return EXIT_SUCCESS;
}
