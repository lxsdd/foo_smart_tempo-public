#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace smart_tempo {

struct LowFrequencyEnergyFeatures {
  double broadbandRms = 0.0;
  double lowFrequencyRms = 0.0;
  double lowFrequencyRatio = 0.0;
};

template <typename OutputSample, typename InputSample>
[[nodiscard]] OutputSample mixdown_interleaved_frame_as(
    const InputSample* data, size_t frame, unsigned channelCount) noexcept {
  OutputSample mono = 0;
  const size_t base = frame * channelCount;
  for (unsigned c = 0; c < channelCount; ++c) {
    mono += static_cast<OutputSample>(data[base + c]);
  }
  return mono / static_cast<OutputSample>(channelCount);
}

class LowFrequencyEnergyTracker {
 public:
  LowFrequencyEnergyTracker() noexcept = default;

  explicit LowFrequencyEnergyTracker(double sampleRate,
                                     double lowpassCutoffHz = 120.0) noexcept {
    configure(sampleRate, lowpassCutoffHz);
  }

  void configure(double sampleRate, double lowpassCutoffHz = 120.0) noexcept {
    if (sampleRate > 0.0 && std::isfinite(sampleRate)) {
      lowpassAlpha_ =
          std::exp(-2.0 * kPi * lowpassCutoffHz / sampleRate);
    }
  }

  void observe(double sample) noexcept {
    if (!std::isfinite(sample)) return;
    broadbandEnergyAccum_ += sample * sample;
    if (!lowpassStateInit_) {
      lowpassState_ = sample;
      lowpassStateInit_ = true;
    } else {
      lowpassState_ = (1.0 - lowpassAlpha_) * sample +
                      lowpassAlpha_ * lowpassState_;
    }
    lowFrequencyEnergyAccum_ += lowpassState_ * lowpassState_;
    ++sampleCount_;
  }

  [[nodiscard]] LowFrequencyEnergyFeatures features() const noexcept {
    LowFrequencyEnergyFeatures out;
    if (sampleCount_ == 0) return out;
    const double count = static_cast<double>(sampleCount_);
    out.broadbandRms = std::sqrt(broadbandEnergyAccum_ / count);
    out.lowFrequencyRms = std::sqrt(lowFrequencyEnergyAccum_ / count);
    out.lowFrequencyRatio =
        (out.broadbandRms > 1e-12)
            ? std::clamp(out.lowFrequencyRms / out.broadbandRms, 0.0, 4.0)
            : 0.0;
    return out;
  }

 private:
  static constexpr double kPi = 3.14159265358979323846;

  double lowpassAlpha_ = 0.0;
  double lowpassState_ = 0.0;
  double broadbandEnergyAccum_ = 0.0;
  double lowFrequencyEnergyAccum_ = 0.0;
  uint64_t sampleCount_ = 0;
  bool lowpassStateInit_ = false;
};

}  // namespace smart_tempo
