// Synthetic MIR single-event gate regression tests; no private audio.
#include "../src/foo_smart_tempo/mir_onset_gate.h"

#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <span>

namespace {
void require(bool ok, const char* message) {
  if (!ok) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}
template <std::size_t N>
bool rejected(const std::array<float, N>& values) {
  return smart_tempo::mir_is_single_effective_onset(
      values.size(), std::span<const float>(values));
}
}

int main() {
  // Original above-average heuristic incorrectly rejects every equal-height
  // peak sequence: none of its values exceed the arithmetic mean.
  require(!rejected(std::array<float, 4>{1.f, 1.f, 1.f, 1.f}),
          "four equal rhythmic transients must be admitted");
  require(!rejected(std::array<float, 5>{1.f, 1.f, 1.f, 1.f, 1.f}),
          "five equally weighted rhythmic transients must be admitted");
  require(!rejected(std::array<float, 4>{1.f, 1.f, 1.f, 1.001f}),
          "one minimally stronger peak cannot hide the rhythmic sequence");
  require(!rejected(std::array<float, 4>{1.f, 1.f, 0.f, 0.f}),
          "two effective rhythmic events must be distinguishable from one");

  require(rejected(std::array<float, 4>{1.f, 0.f, 0.f, 0.f}),
          "one nonzero event remains rejected");
  require(rejected(std::array<float, 4>{20.f, 0.01f, 0.01f, 0.01f}),
          "one dominant event with tiny noise remains rejected");
  require(rejected(std::array<float, 4>{0.f, 0.f, 0.f, 0.f}),
          "zero positive onset support remains rejected");
  require(rejected(std::array<float, 4>{-1.f, -2.f, -1.f, -2.f}),
          "negative-only onset support remains rejected");
  require(rejected(std::array<float, 3>{1.f,
              std::numeric_limits<float>::quiet_NaN(), 1.f}),
          "NaN cannot produce a valid onset distribution");
  require(rejected(std::array<float, 3>{1.f,
              std::numeric_limits<float>::infinity(), 1.f}),
          "infinity cannot produce a valid onset distribution");

  const std::array<float, 3> valid{1.f, 1.f, 1.f};
  require(smart_tempo::mir_is_single_effective_onset(0, valid),
          "missing onset locations are rejected");
  require(smart_tempo::mir_is_single_effective_onset(2, valid),
          "inconsistent onset/magnitude count is rejected");
  require(smart_tempo::mir_is_single_effective_onset(
              0, std::span<const float>{}),
          "empty evidence is rejected");

  std::cout << "PASS_SYNTHETIC_MIR_ONSET_GATE" << std::endl;
  return EXIT_SUCCESS;
}
