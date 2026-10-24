#pragma once

// GUI-only progress presentation: the meter is an approximation of decoded
// work plus completed tracks, not a prediction of MIR computation time.
// Monotonic, bounded until the actual last track completes, and testable
// without foobar/Windows, audio or worker threads.
#include <algorithm>
#include <cmath>
#include <cstddef>

namespace smart_tempo {

inline int progress_permille(std::size_t total, std::size_t completed,
                            std::size_t worker_slots, double decoded_fraction,
                            int previous_permille) noexcept {
  if (total == 0 || completed >= total) return 1000;
  completed = (std::min)(completed, total);
  worker_slots = (std::min)(worker_slots, total - completed);
  const double finished = static_cast<double>(completed) /
                          static_cast<double>(total);
  const double active_window = static_cast<double>(worker_slots) /
                               static_cast<double>(total);
  const double decode = std::isfinite(decoded_fraction)
                            ? std::clamp(decoded_fraction, 0.0, 1.0)
                            : finished;
  // A slot's decode may be done while the more expensive MIR engine is busy.
  const double bound = finished + active_window * 0.85;
  const double fraction = std::clamp((std::max)(finished,
                                               (std::min)(decode, bound)),
                                     0.0, 0.985);
  const int candidate = static_cast<int>(fraction * 1000.0);
  return std::clamp((std::max)(candidate, previous_permille), 0, 985);
}

} // namespace smart_tempo
