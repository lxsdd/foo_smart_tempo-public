#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace smart_tempo {

template <typename SampleContainer>
bool append_spaced_sample(SampleContainer& samples,
                          uint64_t& lastSample,
                          bool& hasLastSample,
                          uint64_t sample,
                          uint64_t minSpacingSamples) {
  const bool spacingOk =
      !hasLastSample || sample >= lastSample + minSpacingSamples;
  if (!spacingOk) return false;
  if (!samples.empty() && sample <= samples.back()) return false;
  samples.push_back(sample);
  lastSample = sample;
  hasLastSample = true;
  return true;
}

template <typename SampleContainer>
uint64_t suppress_secondary_burst_onsets(SampleContainer& samples,
                                         std::vector<double>& strengths,
                                         uint32_t hopSize,
                                         uint64_t minSpacingSamples) {
  if (samples.empty() || samples.size() != strengths.size()) return 0;

  const uint64_t localMaxWindowSamples =
      (std::max)(static_cast<uint64_t>(hopSize * 2u),
                 minSpacingSamples / 2u);
  const uint64_t burstWindowSamples =
      (std::max)(static_cast<uint64_t>(hopSize * 2u),
                 minSpacingSamples / 3u);

  std::vector<uint8_t> keep(samples.size(), 1);
  for (size_t i = 0; i < samples.size(); ++i) {
    const uint64_t center = samples[i];
    double localMax = strengths[i];
    for (size_t j = i; j > 0; --j) {
      const size_t idx = j - 1;
      if (center - samples[idx] > localMaxWindowSamples) break;
      localMax = (std::max)(localMax, strengths[idx]);
    }
    for (size_t j = i + 1; j < samples.size(); ++j) {
      if (samples[j] - center > localMaxWindowSamples) break;
      localMax = (std::max)(localMax, strengths[j]);
    }
    if (localMax > 0.0 && strengths[i] + 1e-12 < localMax * 0.62) {
      keep[i] = 0;
    }
  }

  bool havePrevKept = false;
  uint64_t prevKeptSample = 0;
  double prevKeptStrength = 0.0;
  for (size_t i = 0; i < samples.size(); ++i) {
    if (keep[i] == 0) continue;
    const uint64_t sample = samples[i];
    const double strength = strengths[i];
    if (havePrevKept && sample > prevKeptSample &&
        sample - prevKeptSample < burstWindowSamples &&
        strength < prevKeptStrength * 0.90) {
      keep[i] = 0;
      continue;
    }
    havePrevKept = true;
    prevKeptSample = sample;
    prevKeptStrength = strength;
  }

  SampleContainer filteredSamples;
  std::vector<double> filteredStrengths;
  filteredSamples.reserve(samples.size());
  filteredStrengths.reserve(strengths.size());
  uint64_t suppressed = 0;
  for (size_t i = 0; i < samples.size(); ++i) {
    if (keep[i] == 0) {
      ++suppressed;
      continue;
    }
    filteredSamples.push_back(samples[i]);
    filteredStrengths.push_back(strengths[i]);
  }

  if (suppressed > 0) {
    samples = std::move(filteredSamples);
    strengths = std::move(filteredStrengths);
  }
  return suppressed;
}

template <typename DestinationContainer, typename SourceContainer>
void append_offset_samples(DestinationContainer& destination,
                           const SourceContainer& source,
                           uint64_t offsetSamples) {
  if (source.empty()) return;
  destination.reserve(destination.size() + source.size());
  for (uint64_t localSample : source) {
    destination.push_back(offsetSamples + localSample);
  }
}

}  // namespace smart_tempo
