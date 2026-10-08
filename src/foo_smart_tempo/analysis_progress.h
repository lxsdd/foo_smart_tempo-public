#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace smart_tempo {

class ProgressMillisAccumulator {
 public:
  ProgressMillisAccumulator(std::atomic<uint64_t>* target,
                            double sourceSampleRate) noexcept
      : target_(target), sourceSampleRate_(sourceSampleRate) {}

  void accumulate(std::size_t frames) noexcept {
    if (target_ == nullptr || frames == 0 || !(sourceSampleRate_ > 0.0)) return;
    pendingMillis_ += static_cast<uint64_t>(
        (1000.0 * static_cast<double>(frames)) / sourceSampleRate_);
    flush(false);
  }

  void flush(bool force) noexcept {
    if (target_ == nullptr) return;
    if (pendingMillis_ == 0) return;
    if (!force && pendingMillis_ < kFlushThresholdMillis) return;
    target_->fetch_add(pendingMillis_, std::memory_order_relaxed);
    pendingMillis_ = 0;
  }

 private:
  static constexpr uint64_t kFlushThresholdMillis = 250;

  std::atomic<uint64_t>* target_ = nullptr;
  double sourceSampleRate_ = 0.0;
  uint64_t pendingMillis_ = 0;
};

}  // namespace smart_tempo
