#include "../src/foo_smart_tempo/analysis_progress_projection.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>

void check(bool value, const char* failure) {
  if (!value) {
    std::cerr << "SYNTHETIC_PROGRESS_FAIL " << failure << '\n';
    std::exit(EXIT_FAILURE);
  }
}

int main() {
  using smart_tempo::progress_permille;
  check(progress_permille(0, 0, 0, 0, 0) == 1000, "empty complete");
  check(progress_permille(12, 12, 5, 1.0, 650) == 1000, "exact complete");
  check(progress_permille(12, 0, 0, 0.6, 0) == 0, "not started");
  check(progress_permille(1, 0, 1, 1.0, 0) == 850, "one-track MIR reserve");
  check(progress_permille(10, 4, 2, 0.9, 0) == 570, "bounded active slots");
  check(progress_permille(10, 6, 2, 0.7, 700) == 700, "monotonic");
  check(progress_permille(10, 0, 500, 1.0, 0) == 850, "clamped slots");
  check(progress_permille(10, 9, 1, 1.0, 0) == 985, "last track reserve");
  check(progress_permille(10, 1, 2,
                          std::numeric_limits<double>::quiet_NaN(), 0)
            == 100, "NaN safe");
  check(progress_permille(10, 1, 2,
                          std::numeric_limits<double>::infinity(), 0)
            == 100, "infinity safe");
  check(progress_permille(10, 3, 2, -100, 0) == 300, "negative decode clamp");
  check(progress_permille(10, 0, 1, 0.0, 999) == 985, "resume clamp");
  std::cout << "PASS_SYNTHETIC_MONOTONIC_PROGRESS" << std::endl;
}
