// SPDX-License-Identifier: GPL-2.0-or-later
// Algorithmic port based on Audacity lib-music-information-retrieval
// (Matthieu Hodgkinson). See docs/reports/
// v21_hodgkinson_tatum_full_mir_port_20260530.md for provenance.

#include "stdafx.h"

#include "hodgkinson_full_mir.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <unordered_map>
#include <vector>

extern "C" {
#include "kiss_fftr.h"
}

namespace smart_tempo {
namespace {

// Port note:
// The algorithmic structure in this file follows Audacity 3.7.7
// lib-music-information-retrieval (Matthieu Hodgkinson,
// SPDX-License-Identifier: GPL-2.0-or-later):
//   StftFrameProvider -> GetOnsetDetectionFunction ->
//   GetMeterUsingTatumQuantizationFit.
// Project-local replacements are used for Audacity-only infrastructure
// (PFFFT/PowerSpectrumGetter/IteratorX/MemoryX).

enum class MirTimeSignature {
  TwoTwo,
  FourFour,
  ThreeFour,
  SixEight,
};

struct BarDivision {
  int num_bars = 0;
  int beats_per_bar = 0;
  int total_beats = 0;
  int trailing_beats = 0;
};

struct OnsetQuantization {
  double error = std::numeric_limits<double>::infinity();
  int lag = 0;
  int num_divisions = 0;
};

struct MusicalMeter {
  double bpm = 0.0;
  std::optional<MirTimeSignature> time_signature;
};

struct MirSegmentCandidateInternal {
  HodgkinsonFullMirSegmentCandidate row;
};

using PossibleDivHierarchies =
    std::unordered_map<int, std::vector<BarDivision>>;

[[nodiscard]] constexpr int division_num_beats(
    const BarDivision& division) noexcept {
  return division.total_beats > 0
             ? division.total_beats
             : division.num_bars * division.beats_per_bar;
}

constexpr double kMinBpm = 50.0;
constexpr double kMaxBpm = 200.0;
constexpr int kMinTatumsPerMinute = 100;
constexpr int kMaxTatumsPerMinute = 700;
constexpr int kMinBeatsPerBar = 2;
constexpr int kMaxBeatsPerBar = 4;
constexpr double kLenientLoopThreshold = 0.7129778875046098;
constexpr double kPi = 3.1415926535897932384626433832795;
constexpr std::array<std::pair<int, int>, 9> kPossibleTatumsPerBeat = {{
    {1, 1}, {2, 1}, {3, 1}, {4, 1}, {6, 1},
    {1, 2}, {1, 3}, {1, 4}, {1, 6},
}};

[[nodiscard]] constexpr bool is_partial_bar_hypothesis(
    double duration, int totalBeats, int beatsPerBar) noexcept {
  if (!(duration > 0.0) || totalBeats <= 0 ||
      beatsPerBar < kMinBeatsPerBar || beatsPerBar > kMaxBeatsPerBar ||
      totalBeats % beatsPerBar == 0) {
    return false;
  }
  const double bpm = 60.0 * static_cast<double>(totalBeats) / duration;
  const double barDuration = 60.0 * static_cast<double>(beatsPerBar) / bpm;
  return bpm >= kMinBpm && bpm <= kMaxBpm &&
         barDuration >= 1.0 && barDuration <= 4.0;
}

static_assert(is_partial_bar_hypothesis(20.0, 46, 4));  // 138 BPM.
static_assert(is_partial_bar_hypothesis(20.0, 41, 4));  // 123 BPM.
static_assert(!is_partial_bar_hypothesis(20.0, 48, 4)); // Complete bars.

struct FftDeleter {
  void operator()(kiss_fftr_cfg cfg) const noexcept {
    if (cfg != nullptr) {
      kiss_fftr_free(cfg);
    }
  }
};

using FftStatePtr = std::unique_ptr<kiss_fftr_state, FftDeleter>;

[[nodiscard]] bool is_finite_positive(double value) noexcept {
  return value > 0.0 && std::isfinite(value);
}

[[nodiscard]] bool is_pow2(std::size_t value) noexcept {
  return value > 0 && (value & (value - 1)) == 0;
}

[[nodiscard]] std::vector<float> make_normalized_hann(std::size_t size) {
  std::vector<float> window(size);
  if (size == 0) {
    return window;
  }
  for (std::size_t n = 0; n < size; ++n) {
    window[n] = static_cast<float>(0.5 * (1.0 - std::cos(2.0 * kPi *
                                                        static_cast<double>(n) /
                                                        static_cast<double>(size))));
  }
  const float sum = std::accumulate(window.begin(), window.end(), 0.0f);
  if (sum > 0.0f) {
    for (float& value : window) {
      value /= sum;
    }
  }
  return window;
}

struct OdfFftWorkspace {
  int fft_size = 0;
  FftStatePtr fft;
  std::vector<float> window;
  std::vector<kiss_fft_scalar> time_frame;
  std::vector<kiss_fft_cpx> spectrum;
  std::vector<float> previous_power;
  std::vector<float> current_power;
  std::vector<float> first_power;

  [[nodiscard]] bool ensure(int requestedFftSize) {
    if (requestedFftSize == fft_size && fft != nullptr) {
      return true;
    }

    FftStatePtr requested(
        kiss_fftr_alloc(requestedFftSize, 0, nullptr, nullptr));
    if (requested == nullptr) {
      return false;
    }

    fft_size = requestedFftSize;
    fft = std::move(requested);
    window = make_normalized_hann(static_cast<std::size_t>(fft_size));
    time_frame.resize(static_cast<std::size_t>(fft_size));
    spectrum.resize(static_cast<std::size_t>(fft_size / 2 + 1));
    previous_power.resize(spectrum.size());
    current_power.resize(spectrum.size());
    first_power.reserve(spectrum.size());
    return true;
  }
};

[[nodiscard]] OdfFftWorkspace& odf_fft_workspace() {
  thread_local OdfFftWorkspace workspace;
  return workspace;
}

[[nodiscard]] int frame_size_for_sample_rate(double sampleRate) noexcept {
  if (!is_finite_positive(sampleRate)) {
    return 0;
  }
  const int exponent =
      11 + static_cast<int>(std::lround(std::log2(sampleRate / 44100.0)));
  return 1 << std::clamp(exponent, 7, 15);
}

[[nodiscard]] double hop_size_for_segment(double sampleRate,
                                          std::size_t numSamples) noexcept {
  if (!is_finite_positive(sampleRate) || numSamples == 0) {
    return 0.0;
  }
  const double idealHopSize = 0.01 * sampleRate;
  const int exponent = static_cast<int>(
      std::lround(std::log2(static_cast<double>(numSamples) / idealHopSize)));
  if (exponent < 0) {
    return 0.0;
  }
  const std::size_t numFrames = std::size_t{1} << std::min(exponent, 24);
  return static_cast<double>(numSamples) / static_cast<double>(numFrames);
}

[[nodiscard]] double frame_rate(double sampleRate, double hopSize) noexcept {
  return hopSize > 0.0 ? sampleRate / hopSize : 0.0;
}

void read_circular_window(std::span<const float> samples,
                          int fftSize,
                          double hopSize,
                          int frameIndex,
                          std::span<const float> window,
                          std::vector<kiss_fft_scalar>& out) {
  if (samples.empty() || fftSize <= 0 || hopSize <= 0.0) {
    out.assign(static_cast<std::size_t>(std::max(0, fftSize)),
               kiss_fft_scalar{});
    return;
  }
  out.resize(static_cast<std::size_t>(fftSize));

  const auto numSamples = static_cast<long long>(samples.size());
  const int firstReadPosition = static_cast<int>(std::lround(hopSize)) - fftSize;
  long long start = static_cast<long long>(
      std::llround(static_cast<double>(firstReadPosition) +
                   static_cast<double>(frameIndex) * hopSize));
  while (start < 0) {
    start += numSamples;
  }
  start %= numSamples;

  if (static_cast<std::size_t>(fftSize) > samples.size()) {
    for (int i = 0; i < fftSize; ++i) {
      const auto sourceIndex =
          static_cast<std::size_t>((start + i) % numSamples);
      const float w = (static_cast<std::size_t>(i) < window.size()) ? window[i]
                                                                   : 1.0f;
      out[static_cast<std::size_t>(i)] =
          static_cast<kiss_fft_scalar>(samples[sourceIndex] * w);
    }
    return;
  }

  const auto firstSource = static_cast<std::size_t>(start);
  const auto firstCount = std::min(
      static_cast<std::size_t>(fftSize), samples.size() - firstSource);
  for (std::size_t i = 0; i < firstCount; ++i) {
    const float w = (static_cast<std::size_t>(i) < window.size()) ? window[i]
                                                                 : 1.0f;
    out[i] = static_cast<kiss_fft_scalar>(samples[firstSource + i] * w);
  }
  for (std::size_t i = firstCount; i < static_cast<std::size_t>(fftSize); ++i) {
    const float w = (i < window.size()) ? window[i] : 1.0f;
    out[i] = static_cast<kiss_fft_scalar>(samples[i - firstCount] * w);
  }
}

void power_spectrum(kiss_fftr_cfg fft,
                    const std::vector<kiss_fft_scalar>& timeFrame,
                    std::vector<kiss_fft_cpx>& spectrum,
                    std::vector<float>& power) {
  kiss_fftr(fft, timeFrame.data(), spectrum.data());
  power.resize(spectrum.size());
  for (std::size_t i = 0; i < spectrum.size(); ++i) {
    const double re = static_cast<double>(spectrum[i].r);
    const double im = static_cast<double>(spectrum[i].i);
    power[i] = static_cast<float>(re * re + im * im);
  }
}

[[nodiscard]] float novelty_measure(const std::vector<float>& prev,
                                    const std::vector<float>& current) {
  const std::size_t n = std::min(prev.size(), current.size());
  float novelty = 0.0f;
  for (std::size_t i = 0; i < n; ++i) {
    novelty += std::max(0.0f, current[i] - prev[i]);
  }
  return novelty;
}

[[nodiscard]] std::vector<float> moving_average(const std::vector<float>& x,
                                                double hopRate) {
  if (x.empty() || !(hopRate > 0.0)) {
    return std::vector<float>(x.size(), 0.0f);
  }
  constexpr double kSmoothingWindowDuration = 0.2;
  const int halfWidth =
      static_cast<int>(std::lround(kSmoothingWindowDuration * hopRate / 4.0)) *
          2 +
      1;
  const std::vector<float> window =
      make_normalized_hann(static_cast<std::size_t>(2 * halfWidth + 1));
  std::vector<float> average(x.size(), 0.0f);
  for (std::size_t n = 0; n < x.size(); ++n) {
    float y = 0.0f;
    for (int offset = -halfWidth; offset <= halfWidth; ++offset) {
      long long k = static_cast<long long>(n) + offset;
      while (k < 0) {
        k += static_cast<long long>(x.size());
      }
      while (k >= static_cast<long long>(x.size())) {
        k -= static_cast<long long>(x.size());
      }
      y += x[static_cast<std::size_t>(k)] *
           window[static_cast<std::size_t>(offset + halfWidth)];
    }
    constexpr float kThresholdRaiser = 1.5f;
    average[n] = y * kThresholdRaiser;
  }
  return average;
}

[[nodiscard]] std::vector<float> onset_detection_function(
    std::span<const float> samples,
    double sampleRate,
    double* outFrameRate = nullptr) {
  if (samples.empty() || !is_finite_positive(sampleRate)) {
    return {};
  }
  const int fftSize = frame_size_for_sample_rate(sampleRate);
  const double hopSize = hop_size_for_segment(sampleRate, samples.size());
  const double odfFrameRate = frame_rate(sampleRate, hopSize);
  if (outFrameRate != nullptr) {
    *outFrameRate = odfFrameRate;
  }
  if (fftSize <= 0 || hopSize <= 0.0 || odfFrameRate <= 0.0) {
    return {};
  }
  const int numFrames = static_cast<int>(
      std::lround(static_cast<double>(samples.size()) / hopSize));
  if (numFrames <= 1 || !is_pow2(static_cast<std::size_t>(numFrames))) {
    return {};
  }

  OdfFftWorkspace& workspace = odf_fft_workspace();
  if (!workspace.ensure(fftSize)) {
    return {};
  }
  auto& timeFrame = workspace.time_frame;
  auto& spectrum = workspace.spectrum;
  auto& previousPower = workspace.previous_power;
  auto& currentPower = workspace.current_power;
  auto& firstPower = workspace.first_power;
  std::fill(previousPower.begin(), previousPower.end(), 0.0f);
  firstPower.clear();
  std::vector<float> odf;
  odf.reserve(static_cast<std::size_t>(numFrames));

  for (int frame = 0; frame < numFrames; ++frame) {
    read_circular_window(samples, fftSize, hopSize, frame, workspace.window,
                         timeFrame);
    power_spectrum(workspace.fft.get(), timeFrame, spectrum, currentPower);
    constexpr float kGamma = 100.0f;
    for (float& value : currentPower) {
      value = std::log2(1.0f + kGamma * std::sqrt(std::max(0.0f, value)));
    }

    if (firstPower.empty()) {
      firstPower = currentPower;
    } else {
      odf.push_back(novelty_measure(previousPower, currentPower));
    }
    previousPower.swap(currentPower);
  }
  if (!firstPower.empty()) {
    odf.push_back(novelty_measure(previousPower, firstPower));
  }
  if (!is_pow2(odf.size())) {
    return {};
  }

  const std::vector<float> avg = moving_average(odf, odfFrameRate);
  for (std::size_t i = 0; i < odf.size(); ++i) {
    odf[i] = std::max(0.0f, odf[i] - avg[i]);
  }
  return odf;
}

[[nodiscard]] std::vector<int> peak_indices(const std::vector<float>& x) {
  std::vector<int> peaks;
  if (x.size() < 3) {
    return peaks;
  }
  for (std::size_t j = 0; j < x.size(); ++j) {
    const std::size_t i = (j == 0) ? x.size() - 1 : j - 1;
    const std::size_t k = (j + 1 == x.size()) ? 0 : j + 1;
    if (x[i] < x[j] && x[j] > x[k]) {
      peaks.push_back(static_cast<int>(j));
    }
  }
  return peaks;
}

[[nodiscard]] bool is_single_event(const std::vector<int>& peaks,
                                   const std::vector<float>& values) {
  if (peaks.empty() || values.empty()) {
    return true;
  }
  const double sum = std::accumulate(values.begin(), values.end(), 0.0);
  const double average = sum / static_cast<double>(values.size());
  const auto aboveAverage = std::count_if(
      values.begin(), values.end(),
      [average](float value) { return static_cast<double>(value) > average; });
  return aboveAverage <= 1;
}

[[nodiscard]] PossibleDivHierarchies possible_div_hierarchies(
    double audioDuration) {
  PossibleDivHierarchies possible;
  if (!is_finite_positive(audioDuration)) {
    return possible;
  }
  constexpr double kMinBarDuration = 1.0;
  constexpr double kMaxBarDuration = 4.0;
  const int minNumBars = std::max(
      static_cast<int>(std::lround(audioDuration / kMaxBarDuration)), 1);
  const int maxNumBars =
      static_cast<int>(std::lround(audioDuration / kMinBarDuration));
  for (int numBars = minNumBars; numBars <= maxNumBars; ++numBars) {
    const double barDuration = audioDuration / static_cast<double>(numBars);
    const int minBpb = std::clamp(
        static_cast<int>(std::floor(kMinBpm * barDuration / 60.0)),
        kMinBeatsPerBar, kMaxBeatsPerBar);
    const int maxBpb = std::clamp(
        static_cast<int>(std::ceil(kMaxBpm * barDuration / 60.0)),
        kMinBeatsPerBar, kMaxBeatsPerBar);
    for (int beatsPerBar = minBpb; beatsPerBar <= maxBpb; ++beatsPerBar) {
      for (const auto& [tatumNum, tatumDen] : kPossibleTatumsPerBeat) {
        if ((beatsPerBar * tatumNum) % tatumDen != 0) {
          continue;
        }
        const int tatumsPerBar = beatsPerBar * tatumNum / tatumDen;
        const int numTatums = tatumsPerBar * numBars;
        const double tatumRate =
            60.0 * static_cast<double>(numTatums) / audioDuration;
        if (kMinTatumsPerMinute < tatumRate &&
            tatumRate < kMaxTatumsPerMinute) {
          possible[numTatums].push_back(
              BarDivision{numBars, beatsPerBar,
                          numBars * beatsPerBar, 0});
        }
      }
    }
  }
  return possible;
}

[[nodiscard]] PossibleDivHierarchies partial_bar_div_hierarchies(
    double audioDuration) {
  PossibleDivHierarchies possible;
  if (!is_finite_positive(audioDuration)) {
    return possible;
  }

  const int minBeats = static_cast<int>(
      std::ceil(kMinBpm * audioDuration / 60.0));
  const int maxBeats = static_cast<int>(
      std::floor(kMaxBpm * audioDuration / 60.0));
  for (int totalBeats = minBeats; totalBeats <= maxBeats; ++totalBeats) {
    for (int beatsPerBar = kMinBeatsPerBar;
         beatsPerBar <= kMaxBeatsPerBar; ++beatsPerBar) {
      if (!is_partial_bar_hypothesis(audioDuration, totalBeats,
                                     beatsPerBar)) {
        continue;
      }
      const int trailingBeats = totalBeats % beatsPerBar;
      const int completeBars = totalBeats / beatsPerBar;
      for (const auto& [tatumNum, tatumDen] : kPossibleTatumsPerBeat) {
        if ((totalBeats * tatumNum) % tatumDen != 0) {
          continue;
        }
        const int numTatums = totalBeats * tatumNum / tatumDen;
        const double tatumRate =
            60.0 * static_cast<double>(numTatums) / audioDuration;
        if (kMinTatumsPerMinute < tatumRate &&
            tatumRate < kMaxTatumsPerMinute) {
          possible[numTatums].push_back(BarDivision{
              completeBars, beatsPerBar, totalBeats, trailingBeats});
        }
      }
    }
  }
  return possible;
}

[[nodiscard]] int onset_lag(const std::vector<float>& odf, int numTatums) {
  if (odf.empty() || numTatums <= 0) {
    return 0;
  }
  const double pulsePeriod =
      static_cast<double>(odf.size()) / static_cast<double>(numTatums);
  float maxValue = std::numeric_limits<float>::lowest();
  int lag = 0;
  while (lag < static_cast<int>(odf.size())) {
    float value = 0.0f;
    for (int i = 0; i < numTatums; ++i) {
      const int j = static_cast<int>(std::lround(i * pulsePeriod)) + lag;
      if (j >= 0 && j < static_cast<int>(odf.size())) {
        value += odf[static_cast<std::size_t>(j)];
      }
    }
    if (value < maxValue) {
      break;
    }
    maxValue = value;
    ++lag;
  }
  return std::max(0, lag - 1);
}

[[nodiscard]] double quantization_distance(
    const std::vector<int>& peakIndices,
    const std::vector<float>& peakValues,
    std::size_t size,
    int numDivisions,
    int lag) {
  if (peakIndices.empty() || peakValues.size() != peakIndices.size() ||
      size == 0 || numDivisions <= 0) {
    return std::numeric_limits<double>::infinity();
  }
  const double peakSum =
      std::accumulate(peakValues.begin(), peakValues.end(), 0.0);
  if (!(peakSum > 0.0)) {
    return std::numeric_limits<double>::infinity();
  }
  const double odfSamplesPerDivision =
      static_cast<double>(size) / static_cast<double>(numDivisions);
  double weightedDistance = 0.0;
  for (std::size_t i = 0; i < peakIndices.size(); ++i) {
    const double shifted = static_cast<double>(peakIndices[i] - lag);
    const double closest = std::round(shifted / odfSamplesPerDivision);
    const double distance =
        (shifted - closest * odfSamplesPerDivision) / odfSamplesPerDivision;
    weightedDistance +=
        (2.0 * std::abs(distance)) * static_cast<double>(peakValues[i]);
  }
  return weightedDistance / peakSum;
}

[[nodiscard]] const OnsetQuantization& cached_onset_quantization(
    const std::vector<float>& odf,
    const std::vector<int>& peaks,
    const std::vector<float>& peakValues,
    int numTatums,
    std::unordered_map<int, OnsetQuantization>& cache) {
  auto [it, inserted] = cache.try_emplace(numTatums);
  if (inserted) {
    it->second.lag = onset_lag(odf, numTatums);
    it->second.error = quantization_distance(
        peaks, peakValues, odf.size(), numTatums, it->second.lag);
    it->second.num_divisions = numTatums;
  }
  return it->second;
}

[[nodiscard]] OnsetQuantization run_quantization_experiment(
    const std::vector<float>& odf,
    const std::vector<int>& peaks,
    const std::vector<float>& peakValues,
    const std::vector<int>& possibleTatums,
    std::unordered_map<int, OnsetQuantization>& cache) {
  OnsetQuantization best;
  for (const int numTatums : possibleTatums) {
    const OnsetQuantization& current = cached_onset_quantization(
        odf, peaks, peakValues, numTatums, cache);
    if (current.error < best.error) {
      best = current;
    }
  }
  return best;
}

[[nodiscard]] std::optional<MirTimeSignature> time_signature(
    const BarDivision& div,
    int numTatums) {
  const int numBeats = division_num_beats(div);
  if (numBeats <= 0) {
    return std::nullopt;
  }
  const double tatumsPerBeat =
      static_cast<double>(numTatums) / static_cast<double>(numBeats);
  switch (div.beats_per_bar) {
    case 2:
      return (tatumsPerBeat == 3.0) ? MirTimeSignature::SixEight
                                    : MirTimeSignature::TwoTwo;
    case 3:
      return MirTimeSignature::ThreeFour;
    case 4:
      return MirTimeSignature::FourFour;
    default:
      return std::nullopt;
  }
}

[[nodiscard]] const char* time_signature_label(
    const std::optional<MirTimeSignature>& ts) noexcept {
  if (!ts.has_value()) {
    return "unknown";
  }
  switch (*ts) {
    case MirTimeSignature::TwoTwo:
      return "2/2";
    case MirTimeSignature::FourFour:
      return "4/4";
    case MirTimeSignature::ThreeFour:
      return "3/4";
    case MirTimeSignature::SixEight:
      return "6/8";
  }
  return "unknown";
}

[[nodiscard]] double time_signature_likelihood(
    const std::optional<MirTimeSignature>& ts,
    double bpm) {
  if (!ts.has_value()) {
    return 0.0;
  }
  double mu = 115.0;
  double sigma = 25.0;
  double prior = 0.45;
  switch (*ts) {
    case MirTimeSignature::TwoTwo:
      prior = 0.1;
      break;
    case MirTimeSignature::FourFour:
      prior = 0.45;
      break;
    case MirTimeSignature::ThreeFour:
      mu = 140.0;
      prior = 0.2;
      break;
    case MirTimeSignature::SixEight:
      mu = 64.0;
      sigma = 15.0;
      prior = 0.25;
      break;
  }
  const double z = (bpm - mu) / sigma;
  return std::exp(-0.5 * z * z) * prior;
}

struct AutocorrFftWorkspace {
  int size = 0;
  FftStatePtr forward;
  FftStatePtr inverse;
  std::vector<kiss_fft_scalar> time;
  std::vector<kiss_fft_cpx> spectrum;
  std::vector<kiss_fft_scalar> correlation;

  [[nodiscard]] bool ensure(int requestedSize) {
    if (requestedSize == size && forward != nullptr && inverse != nullptr) {
      return true;
    }

    FftStatePtr requestedForward(
        kiss_fftr_alloc(requestedSize, 0, nullptr, nullptr));
    FftStatePtr requestedInverse(
        kiss_fftr_alloc(requestedSize, 1, nullptr, nullptr));
    if (requestedForward == nullptr || requestedInverse == nullptr) {
      return false;
    }

    size = requestedSize;
    forward = std::move(requestedForward);
    inverse = std::move(requestedInverse);
    time.resize(static_cast<std::size_t>(size));
    spectrum.resize(static_cast<std::size_t>(size / 2 + 1));
    correlation.resize(static_cast<std::size_t>(size));
    return true;
  }
};

[[nodiscard]] AutocorrFftWorkspace& autocorr_fft_workspace() {
  thread_local AutocorrFftWorkspace workspace;
  return workspace;
}

[[nodiscard]] std::vector<float> normalized_circular_autocorr(
    const std::vector<float>& x) {
  if (x.empty() || !is_pow2(x.size())) {
    return {};
  }
  if (std::all_of(x.begin(), x.end(), [](float value) {
        return value == 0.0f;
      })) {
    return x;
  }

  const int n = static_cast<int>(x.size());
  AutocorrFftWorkspace& workspace = autocorr_fft_workspace();
  if (!workspace.ensure(n)) {
    return {};
  }
  std::copy(x.begin(), x.end(), workspace.time.begin());
  kiss_fftr(workspace.forward.get(), workspace.time.data(),
            workspace.spectrum.data());
  for (auto& bin : workspace.spectrum) {
    const double re = static_cast<double>(bin.r);
    const double im = static_cast<double>(bin.i);
    bin.r = static_cast<kiss_fft_scalar>(re * re + im * im);
    bin.i = kiss_fft_scalar{};
  }
  kiss_fftri(workspace.inverse.get(), workspace.spectrum.data(),
             workspace.correlation.data());
  const double normalizer = static_cast<double>(workspace.correlation[0]);
  if (!(normalizer > 0.0) || !std::isfinite(normalizer)) {
    return {};
  }
  std::vector<float> result(static_cast<std::size_t>(n / 2 + 1));
  for (std::size_t i = 0; i < result.size(); ++i) {
    result[i] = static_cast<float>(
        static_cast<double>(workspace.correlation[i]) / normalizer);
  }
  return result;
}

[[nodiscard]] int map_to_positive_half_index(int index, int fullSize) noexcept {
  if (fullSize <= 0) {
    return 0;
  }
  index %= fullSize;
  if (index < 0) {
    index += fullSize;
  }
  if (index > fullSize / 2) {
    index = fullSize - index;
  }
  return index;
}

[[nodiscard]] double beat_self_similarity_score(
    double odfAutoCorrSampleRate,
    double bpm,
    const std::vector<float>& odfAutoCorr,
    int odfAutocorrFullSize) {
  if (!is_finite_positive(odfAutoCorrSampleRate) ||
      !is_finite_positive(bpm) || odfAutoCorr.empty() ||
      odfAutocorrFullSize <= 0) {
    return 0.0;
  }
  const double lag = odfAutoCorrSampleRate * 60.0 / bpm;
  int periodIndex = 1;
  double sum = 0.0;
  int count = 0;
  while (true) {
    int j = static_cast<int>(periodIndex++ * lag + 0.5);
    if (j >= static_cast<int>(odfAutoCorr.size())) {
      break;
    }
    int guard = 0;
    while (++guard < odfAutocorrFullSize) {
      const int i = map_to_positive_half_index(j - 1, odfAutocorrFullSize);
      const int k = map_to_positive_half_index(j + 1, odfAutocorrFullSize);
      if (odfAutoCorr[static_cast<std::size_t>(i)] <=
              odfAutoCorr[static_cast<std::size_t>(j)] &&
          odfAutoCorr[static_cast<std::size_t>(j)] >=
              odfAutoCorr[static_cast<std::size_t>(k)]) {
        break;
      }
      j = odfAutoCorr[static_cast<std::size_t>(i)] >
                  odfAutoCorr[static_cast<std::size_t>(k)]
              ? i
              : k;
    }
    sum += odfAutoCorr[static_cast<std::size_t>(j)];
    ++count;
  }
  return count > 0 ? sum / static_cast<double>(count) : 0.0;
}

[[nodiscard]] double cached_beat_self_similarity_score(
    int numBeats,
    double bpm,
    double autocorrRate,
    const std::vector<float>& autocorr,
    int fullSize,
    std::unordered_map<int, double>& cache) {
  auto [it, inserted] = cache.try_emplace(numBeats);
  if (inserted) {
    it->second =
        beat_self_similarity_score(autocorrRate, bpm, autocorr, fullSize);
  }
  return it->second;
}

[[nodiscard]] std::size_t best_bar_division_index(
    const std::vector<BarDivision>& divisions,
    double audioDuration,
    int numTatums,
    const std::vector<float>& autocorr,
    std::unordered_map<int, double>& beatScoreCache) {
  if (divisions.empty() || !is_finite_positive(audioDuration)) {
    return 0;
  }
  if (autocorr.empty()) {
    return 0;
  }
  const int fullSize = 2 * (static_cast<int>(autocorr.size()) - 1);
  const double autocorrRate = static_cast<double>(fullSize) / audioDuration;
  std::vector<double> scores(divisions.size(), 0.0);
  for (std::size_t i = 0; i < divisions.size(); ++i) {
    const BarDivision& div = divisions[i];
    const int numBeats = division_num_beats(div);
    const auto ts = time_signature(div, numTatums);
    const double bpm =
        static_cast<double>(numBeats) / audioDuration * 60.0;
    const double likelihood = time_signature_likelihood(ts, bpm);
    scores[i] = likelihood * cached_beat_self_similarity_score(
                                 numBeats, bpm, autocorrRate, autocorr,
                                 fullSize, beatScoreCache);
  }
  return static_cast<std::size_t>(
      std::max_element(scores.begin(), scores.end()) - scores.begin());
}

[[nodiscard]] MusicalMeter most_likely_meter(
    const std::vector<float>& autocorr,
    int numTatums,
    std::vector<BarDivision> divisions,
    double audioDuration,
    std::unordered_map<int, double>& beatScoreCache) {
  std::vector<BarDivision> fourFour;
  for (const BarDivision& div : divisions) {
    if (time_signature(div, numTatums) == MirTimeSignature::FourFour) {
      fourFour.push_back(div);
    }
  }
  if (!fourFour.empty()) {
    divisions.swap(fourFour);
  }
  const std::size_t winner =
      best_bar_division_index(divisions, audioDuration, numTatums, autocorr,
                              beatScoreCache);
  const BarDivision& div = divisions[std::min(winner, divisions.size() - 1)];
  const int numBeats = division_num_beats(div);
  const auto signature = time_signature(div, numTatums);
  const double bpm = 60.0 * static_cast<double>(numBeats) / audioDuration;
  return MusicalMeter{bpm, signature};
}

[[nodiscard]] std::vector<BarDivision> four_four_preferred_divisions(
    int numTatums,
    const std::vector<BarDivision>& divisions) {
  std::vector<BarDivision> fourFour;
  for (const BarDivision& div : divisions) {
    if (time_signature(div, numTatums) == MirTimeSignature::FourFour) {
      fourFour.push_back(div);
    }
  }
  return fourFour.empty() ? divisions : fourFour;
}

template <std::size_t Capacity, bool UniqueBpmOnly = false>
[[nodiscard]] std::array<HodgkinsonFullMirSegmentCandidate, Capacity>
select_top_k_candidates(std::vector<HodgkinsonFullMirSegmentCandidate> rows,
                        const HodgkinsonFullMirSegmentCandidate& winner,
                        std::size_t& outCount) {
  outCount = 0;
  std::array<HodgkinsonFullMirSegmentCandidate, Capacity> result{};
  if (rows.empty() && !is_finite_positive(winner.candidate_bpm)) {
    return result;
  }

  auto sameCandidate = [](const HodgkinsonFullMirSegmentCandidate& a,
                          const HodgkinsonFullMirSegmentCandidate& b) {
    if (std::abs(a.candidate_bpm - b.candidate_bpm) > 0.001) {
      return false;
    }
    if constexpr (UniqueBpmOnly) {
      return true;
    }
    return a.tatum_count == b.tatum_count &&
           a.beats_per_bar == b.beats_per_bar &&
           a.total_beats == b.total_beats &&
           a.trailing_beats == b.trailing_beats;
  };

  std::stable_sort(rows.begin(), rows.end(),
                   [](const HodgkinsonFullMirSegmentCandidate& a,
                      const HodgkinsonFullMirSegmentCandidate& b) {
                     if (a.combined_score != b.combined_score) {
                       return a.combined_score > b.combined_score;
                     }
                     if (a.quantization_score != b.quantization_score) {
                       return a.quantization_score > b.quantization_score;
                     }
                     return a.candidate_bpm < b.candidate_bpm;
                   });

  std::vector<HodgkinsonFullMirSegmentCandidate> selected;
  selected.reserve(result.size());
  for (const auto& row : rows) {
    const bool duplicate = std::any_of(
        selected.begin(), selected.end(),
        [&](const auto& existing) { return sameCandidate(existing, row); });
    if (!duplicate) {
      selected.push_back(row);
    }
    if (selected.size() >= result.size()) {
      break;
    }
  }

  const bool winnerSelected = std::any_of(
      selected.begin(), selected.end(),
      [&](const auto& row) { return sameCandidate(row, winner); });
  if (!winnerSelected && is_finite_positive(winner.candidate_bpm)) {
    if (selected.size() < result.size()) {
      selected.push_back(winner);
    } else if (!selected.empty()) {
      selected.back() = winner;
    }
  }

  std::stable_sort(selected.begin(), selected.end(),
                   [](const HodgkinsonFullMirSegmentCandidate& a,
                      const HodgkinsonFullMirSegmentCandidate& b) {
                     if (a.combined_score != b.combined_score) {
                       return a.combined_score > b.combined_score;
                     }
                     if (a.quantization_score != b.quantization_score) {
                       return a.quantization_score > b.quantization_score;
                     }
                     if (a.winner != b.winner) {
                       return a.winner;
                     }
                     return a.candidate_bpm < b.candidate_bpm;
                   });

  outCount = std::min(result.size(), selected.size());
  for (std::size_t i = 0; i < outCount; ++i) {
    selected[i].rank = i + 1;
    result[i] = selected[i];
  }
  return result;
}

void populate_top_k_segment_candidates(
    HodgkinsonFullMirSegmentEvaluation& evaluation,
    const PossibleDivHierarchies& possible,
    const std::vector<float>& odf,
    const std::vector<float>& autocorr,
    const std::vector<int>& peaks,
    const std::vector<float>& peakValues,
    std::unordered_map<int, OnsetQuantization>& quantizationCache,
    std::unordered_map<int, double>& beatScoreCache) {
  auto& result = evaluation.result;
  if (possible.empty() || odf.empty() || peaks.empty()) {
    return;
  }

  const int fullSize =
      autocorr.empty() ? 0 : 2 * (static_cast<int>(autocorr.size()) - 1);
  const double autocorrRate =
      fullSize > 0 && is_finite_positive(result.segment_duration_sec)
          ? static_cast<double>(fullSize) / result.segment_duration_sec
          : 0.0;
  std::vector<HodgkinsonFullMirSegmentCandidate> rows;

  HodgkinsonFullMirSegmentCandidate winner;
  winner.enabled = true;
  winner.winner = true;
  winner.frontend_source = result.frontend_source;
  winner.segment_start_sec = result.segment_start_sec;
  winner.segment_duration_sec = result.segment_duration_sec;
  winner.segment_index = result.segment_index;
  winner.segment_count = result.segment_count;
  winner.candidate_bpm = result.candidate_bpm;
  winner.quantization_score = result.loop_fit_score;
  winner.combined_score = result.loop_fit_score;
  winner.tatum_count = result.tatum_count;
  winner.meter = result.meter;
  winner.onset_count = result.onset_count;
  winner.odf_peak_count = result.odf_peak_count;
  winner.reason = "audacity_mir_full_topk_segment_winner";

  for (const auto& [numTatums, rawDivisions] : possible) {
    if (numTatums <= 0 || rawDivisions.empty()) {
      continue;
    }
    const double quantizationError =
        cached_onset_quantization(odf, peaks, peakValues, numTatums,
                                  quantizationCache)
            .error;
    if (!std::isfinite(quantizationError)) {
      continue;
    }
    const double quantizationScore =
        std::clamp(1.0 - quantizationError, 0.0, 1.0);
    const auto divisions = four_four_preferred_divisions(numTatums, rawDivisions);
    for (const BarDivision& div : divisions) {
      const int numBeats = division_num_beats(div);
      if (numBeats <= 0) {
        continue;
      }
      const double bpm =
          60.0 * static_cast<double>(numBeats) / result.segment_duration_sec;
      if (!is_finite_positive(bpm)) {
        continue;
      }
      const auto signature = time_signature(div, numTatums);
      const double likelihood = time_signature_likelihood(signature, bpm);
      const double meterScore =
          likelihood * cached_beat_self_similarity_score(
                           numBeats, bpm, autocorrRate, autocorr, fullSize,
                           beatScoreCache);
      HodgkinsonFullMirSegmentCandidate row;
      row.enabled = true;
      row.frontend_source = result.frontend_source;
      row.segment_start_sec = result.segment_start_sec;
      row.segment_duration_sec = result.segment_duration_sec;
      row.segment_index = result.segment_index;
      row.segment_count = result.segment_count;
      row.candidate_bpm = bpm;
      row.quantization_score = quantizationScore;
      row.meter_score = meterScore;
      row.quantization_error = quantizationError;
      row.combined_score = quantizationScore * std::max(0.0, meterScore);
      row.tatum_count = static_cast<std::size_t>(numTatums);
      row.num_bars = static_cast<std::size_t>(std::max(0, div.num_bars));
      row.beats_per_bar =
          static_cast<std::size_t>(std::max(0, div.beats_per_bar));
      row.total_beats =
          static_cast<std::size_t>(std::max(0, numBeats));
      row.trailing_beats =
          static_cast<std::size_t>(std::max(0, div.trailing_beats));
      row.meter = time_signature_label(signature);
      row.onset_count = result.onset_count;
      row.odf_peak_count = result.odf_peak_count;
      row.reason = "audacity_mir_full_topk_segment_candidate";
      if (std::abs(row.candidate_bpm - result.candidate_bpm) <= 0.001 &&
          row.tatum_count == result.tatum_count) {
        row.winner = true;
        winner = row;
        winner.reason = "audacity_mir_full_topk_segment_winner";
      }
      rows.push_back(row);
    }
  }

  evaluation.top_k_candidates =
      select_top_k_candidates<5>(std::move(rows), winner,
                                 evaluation.top_k_count);
}

void populate_partial_bar_segment_candidates(
    HodgkinsonFullMirSegmentEvaluation& evaluation,
    const PossibleDivHierarchies& possible,
    const std::vector<float>& odf,
    const std::vector<float>& autocorr,
    const std::vector<int>& peaks,
    const std::vector<float>& peakValues,
    std::unordered_map<int, OnsetQuantization>& quantizationCache,
    std::unordered_map<int, double>& beatScoreCache) {
  const auto& result = evaluation.result;
  if (possible.empty() || odf.empty() || peaks.empty()) {
    return;
  }

  const int fullSize =
      autocorr.empty() ? 0 : 2 * (static_cast<int>(autocorr.size()) - 1);
  const double autocorrRate =
      fullSize > 0 && is_finite_positive(result.segment_duration_sec)
          ? static_cast<double>(fullSize) / result.segment_duration_sec
          : 0.0;
  std::vector<HodgkinsonFullMirSegmentCandidate> rows;

  for (const auto& [numTatums, rawDivisions] : possible) {
    if (numTatums <= 0 || rawDivisions.empty()) {
      continue;
    }
    const double quantizationError =
        cached_onset_quantization(odf, peaks, peakValues, numTatums,
                                  quantizationCache)
            .error;
    if (!std::isfinite(quantizationError)) {
      continue;
    }
    const double quantizationScore =
        std::clamp(1.0 - quantizationError, 0.0, 1.0);
    const auto divisions =
        four_four_preferred_divisions(numTatums, rawDivisions);
    for (const BarDivision& div : divisions) {
      const int numBeats = division_num_beats(div);
      if (numBeats <= 0 || div.trailing_beats <= 0) {
        continue;
      }
      const double bpm =
          60.0 * static_cast<double>(numBeats) / result.segment_duration_sec;
      if (!is_finite_positive(bpm)) {
        continue;
      }
      const auto signature = time_signature(div, numTatums);
      const double likelihood = time_signature_likelihood(signature, bpm);
      const double beatScore = cached_beat_self_similarity_score(
          numBeats, bpm, autocorrRate, autocorr, fullSize, beatScoreCache);

      HodgkinsonFullMirSegmentCandidate row;
      row.enabled = true;
      row.frontend_source = "audacity_mir_partial_bar_shadow";
      row.segment_start_sec = result.segment_start_sec;
      row.segment_duration_sec = result.segment_duration_sec;
      row.segment_index = result.segment_index;
      row.segment_count = result.segment_count;
      row.candidate_bpm = bpm;
      row.quantization_score = quantizationScore;
      row.meter_score = likelihood * beatScore;
      row.quantization_error = quantizationError;
      row.combined_score =
          quantizationScore * std::max(0.0, row.meter_score);
      row.tatum_count = static_cast<std::size_t>(numTatums);
      row.num_bars =
          static_cast<std::size_t>(std::max(0, div.num_bars));
      row.beats_per_bar =
          static_cast<std::size_t>(std::max(0, div.beats_per_bar));
      row.total_beats = static_cast<std::size_t>(numBeats);
      row.trailing_beats =
          static_cast<std::size_t>(div.trailing_beats);
      row.meter = time_signature_label(signature);
      row.onset_count = result.onset_count;
      row.odf_peak_count = result.odf_peak_count;
      row.reason = "partial_bar_shadow";
      rows.push_back(row);
    }
  }

  const HodgkinsonFullMirSegmentCandidate noWinner;
  evaluation.partial_bar_candidates =
      select_top_k_candidates<64, true>(std::move(rows), noWinner,
                                        evaluation.partial_bar_count);
}

[[nodiscard]] HodgkinsonFullMirSegmentEvaluation evaluate_full_mir_meter(
    std::span<const float> samples,
    double sampleRate,
    double segmentStartSec,
    std::size_t segmentIndex,
    std::size_t segmentCount,
    bool includePartialBarCandidates = false) {
  HodgkinsonFullMirSegmentEvaluation evaluation;
  HodgkinsonTatumProbeResult& result = evaluation.result;
  result.enabled = true;
  result.frontend_source = "audacity_mir_full";
  result.segment_start_sec = segmentStartSec;
  result.segment_duration_sec =
      is_finite_positive(sampleRate)
          ? static_cast<double>(samples.size()) / sampleRate
          : 0.0;
  result.segment_index = segmentIndex;
  result.segment_count = segmentCount;

  if (samples.size() < 1024 || !is_finite_positive(sampleRate)) {
    result.reason = "invalid_input";
    return evaluation;
  }

  double odfFrameRate = 0.0;
  const std::vector<float> odf =
      onset_detection_function(samples, sampleRate, &odfFrameRate);
  if (odf.size() < 8) {
    result.reason = "insufficient_odf";
    return evaluation;
  }
  const std::vector<int> peaks = peak_indices(odf);
  result.odf_peak_count = peaks.size();
  result.onset_count = peaks.size();
  if (peaks.size() < 3) {
    result.reason = "insufficient_onsets";
    return evaluation;
  }
  std::vector<float> peakValues(peaks.size());
  std::transform(peaks.begin(), peaks.end(), peakValues.begin(),
                 [&](int index) { return odf[static_cast<std::size_t>(index)]; });
  if (is_single_event(peaks, peakValues)) {
    result.reason = "single_event";
    return evaluation;
  }

  const double duration = result.segment_duration_sec;
  const PossibleDivHierarchies possible = possible_div_hierarchies(duration);
  if (possible.empty()) {
    result.reason = "no_loop_hypotheses";
    return evaluation;
  }
  std::vector<int> possibleTatums;
  possibleTatums.reserve(possible.size());
  for (const auto& entry : possible) {
    possibleTatums.push_back(entry.first);
  }

  std::unordered_map<int, OnsetQuantization> quantizationCache;
  quantizationCache.reserve(possible.size());
  const OnsetQuantization experiment = run_quantization_experiment(
      odf, peaks, peakValues, possibleTatums, quantizationCache);
  if (!(experiment.num_divisions > 0) || !std::isfinite(experiment.error)) {
    result.reason = "ambiguous_tatum_fit";
    return evaluation;
  }

  const auto divIt = possible.find(experiment.num_divisions);
  if (divIt == possible.end() || divIt->second.empty()) {
    result.reason = "missing_bar_division";
    return evaluation;
  }
  const std::vector<float> autocorr = normalized_circular_autocorr(odf);
  std::unordered_map<int, double> beatScoreCache;
  beatScoreCache.reserve(256);
  const MusicalMeter meter = most_likely_meter(
      autocorr, experiment.num_divisions, divIt->second, duration,
      beatScoreCache);
  const double score = std::clamp(1.0 - experiment.error, 0.0, 1.0);
  result.candidate_bpm = meter.bpm;
  result.loop_fit_score = score;
  result.loop_fit_confidence = score;
  result.tatum_count = static_cast<std::size_t>(experiment.num_divisions);
  result.consensus_support = 1;
  result.meter = time_signature_label(meter.time_signature);
  result.candidate = score >= kLenientLoopThreshold;
  result.reason =
      result.candidate ? "audacity_mir_full" : "audacity_mir_full_below_threshold";
  const PossibleDivHierarchies partialPossible =
      includePartialBarCandidates ? partial_bar_div_hierarchies(duration)
                                  : PossibleDivHierarchies{};
  quantizationCache.reserve(possible.size() + partialPossible.size());
  populate_top_k_segment_candidates(evaluation, possible, odf, autocorr, peaks,
                                    peakValues, quantizationCache,
                                    beatScoreCache);
  if (includePartialBarCandidates) {
    populate_partial_bar_segment_candidates(
        evaluation, partialPossible, odf, autocorr, peaks, peakValues,
        quantizationCache, beatScoreCache);
  }
  return evaluation;
}

[[nodiscard]] double normalize_family_bpm(double candidateBpm,
                                          double referenceBpm) noexcept {
  if (!is_finite_positive(candidateBpm) || !is_finite_positive(referenceBpm)) {
    return candidateBpm;
  }
  double normalized = candidateBpm;
  while (normalized < referenceBpm * 0.7071067811865475) {
    normalized *= 2.0;
  }
  while (normalized > referenceBpm * 1.4142135623730951) {
    normalized *= 0.5;
  }
  return normalized;
}

[[nodiscard]] const char* ratio_class(double ratio) noexcept {
  if (!is_finite_positive(ratio)) {
    return "invalid";
  }
  struct Target {
    double value;
    const char* exact;
    const char* near_label;
  };
  constexpr std::array<Target, 9> targets = {{
      {1.0, "direct", "near_direct"},
      {0.5, "half", "near_half"},
      {2.0, "double", "near_double"},
      {0.75, "three_quarters", "near_three_quarters"},
      {4.0 / 3.0, "four_thirds", "near_four_thirds"},
      {1.5, "three_halves", "near_three_halves"},
      {2.0 / 3.0, "two_thirds", "near_two_thirds"},
      {0.25, "quarter", "near_quarter"},
      {4.0, "quadruple", "near_quadruple"},
  }};
  const Target* best = nullptr;
  double bestError = std::numeric_limits<double>::infinity();
  for (const auto& target : targets) {
    const double error = std::abs(ratio - target.value) / target.value;
    if (error < bestError) {
      bestError = error;
      best = &target;
    }
  }
  if (best == nullptr) {
    return "other";
  }
  if (bestError <= 0.0075) {
    return best->exact;
  }
  if (bestError <= 0.04) {
    return best->near_label;
  }
  return "other";
}

struct ResultFamily {
  double bpm = 0.0;
  double score_sum = 0.0;
  double best_score = 0.0;
  std::size_t support = 0;
  std::size_t candidate_support = 0;
  HodgkinsonTatumProbeResult representative;
};

void add_candidate_family(std::vector<ResultFamily>& families,
                          const HodgkinsonTatumProbeResult& result,
                          double familyBpm) {
  if (!is_finite_positive(result.candidate_bpm) ||
      !(result.loop_fit_score > 0.0) || !is_finite_positive(familyBpm)) {
    return;
  }
  auto bestIt = families.end();
  double bestDistance = std::numeric_limits<double>::infinity();
  for (auto it = families.begin(); it != families.end(); ++it) {
    const double distance = std::abs(it->bpm - familyBpm);
    if (distance <= 2.0 && distance < bestDistance) {
      bestIt = it;
      bestDistance = distance;
    }
  }
  if (bestIt == families.end()) {
    ResultFamily family;
    family.bpm = familyBpm;
    family.score_sum = result.loop_fit_score;
    family.best_score = result.loop_fit_score;
    family.support = 1;
    family.candidate_support = result.candidate ? 1 : 0;
    family.representative = result;
    family.representative.family_bpm = familyBpm;
    families.push_back(family);
    return;
  }

  const double previousWeight = bestIt->score_sum;
  bestIt->score_sum += result.loop_fit_score;
  ++bestIt->support;
  if (result.candidate) {
    ++bestIt->candidate_support;
  }
  if (bestIt->score_sum > 0.0) {
    bestIt->bpm =
        ((bestIt->bpm * previousWeight) +
         (familyBpm * result.loop_fit_score)) /
        bestIt->score_sum;
  }
  if (result.loop_fit_score > bestIt->best_score) {
    bestIt->best_score = result.loop_fit_score;
    bestIt->representative = result;
    bestIt->representative.family_bpm = bestIt->bpm;
  }
}

void add_family(std::vector<ResultFamily>& families,
                const HodgkinsonTatumProbeResult& result,
                double referenceBpm) {
  add_candidate_family(families, result,
                       normalize_family_bpm(result.candidate_bpm, referenceBpm));
}

void add_raw_family(std::vector<ResultFamily>& families,
                    const HodgkinsonTatumProbeResult& result) {
  add_candidate_family(families, result, result.candidate_bpm);
}

[[nodiscard]] const char* family_class(double familyBpm,
                                       double referenceBpm,
                                       std::size_t support,
                                       std::size_t segmentCount,
                                       double runnerUpScore,
                                       double familyScore) noexcept {
  if (!is_finite_positive(familyBpm) || !is_finite_positive(referenceBpm) ||
      segmentCount == 0 || support == 0) {
    return "ambiguous";
  }
  const double supportRatio =
      static_cast<double>(support) / static_cast<double>(segmentCount);
  if (supportRatio < 0.35) {
    return "ambiguous";
  }
  if (runnerUpScore > 0.0 && familyScore < runnerUpScore * 1.25) {
    return "ambiguous";
  }
  if (std::abs(familyBpm - referenceBpm) <= 2.0) {
    return "supports_current";
  }
  if (std::abs((familyBpm * 2.0) - referenceBpm) <= 2.0) {
    return "supports_half";
  }
  if (std::abs((familyBpm * 0.5) - referenceBpm) <= 2.0) {
    return "supports_double";
  }
  return "supports_alternative";
}

}  // namespace

HodgkinsonTatumProbeResult evaluate_hodgkinson_full_mir_segment(
    const HodgkinsonFullMirSegment& segment) noexcept {
  try {
    return evaluate_full_mir_meter(segment.mono_samples, segment.sample_rate,
                                   segment.segment_start_sec,
                                   segment.segment_index,
                                   segment.segment_count)
        .result;
  } catch (...) {
    HodgkinsonTatumProbeResult result;
    result.enabled = true;
    result.frontend_source = "audacity_mir_full";
    result.segment_start_sec = segment.segment_start_sec;
    result.segment_index = segment.segment_index;
    result.segment_count = segment.segment_count;
    result.reason = "exception";
    return result;
  }
}

HodgkinsonFullMirSegmentEvaluation
evaluate_hodgkinson_full_mir_segment_with_candidates(
    const HodgkinsonFullMirSegment& segment) noexcept {
  try {
    return evaluate_full_mir_meter(segment.mono_samples, segment.sample_rate,
                                   segment.segment_start_sec,
                                   segment.segment_index,
                                   segment.segment_count);
  } catch (...) {
    HodgkinsonFullMirSegmentEvaluation evaluation;
    evaluation.result.enabled = true;
    evaluation.result.frontend_source = "audacity_mir_full";
    evaluation.result.segment_start_sec = segment.segment_start_sec;
    evaluation.result.segment_index = segment.segment_index;
    evaluation.result.segment_count = segment.segment_count;
    evaluation.result.reason = "exception";
    return evaluation;
  }
}

HodgkinsonFullMirSegmentEvaluation
evaluate_hodgkinson_full_mir_segment_with_partial_bar_candidates(
    const HodgkinsonFullMirSegment& segment) noexcept {
  try {
    return evaluate_full_mir_meter(
        segment.mono_samples, segment.sample_rate, segment.segment_start_sec,
        segment.segment_index, segment.segment_count, true);
  } catch (...) {
    HodgkinsonFullMirSegmentEvaluation evaluation;
    evaluation.result.enabled = true;
    evaluation.result.frontend_source = "audacity_mir_full";
    evaluation.result.segment_start_sec = segment.segment_start_sec;
    evaluation.result.segment_index = segment.segment_index;
    evaluation.result.segment_count = segment.segment_count;
    evaluation.result.reason = "exception";
    return evaluation;
  }
}

HodgkinsonTatumProbeResult aggregate_hodgkinson_full_mir_results(
    const char* frontendSource,
    double referenceBpm,
    std::span<const HodgkinsonTatumProbeResult> segmentResults) noexcept {
  HodgkinsonTatumProbeResult empty;
  empty.enabled = true;
  empty.frontend_source =
      (frontendSource != nullptr && frontendSource[0] != '\0')
          ? frontendSource
          : "audacity_mir_full";
  empty.input_bpm = referenceBpm;
  empty.candidate_bpm = referenceBpm;
  empty.segment_count = segmentResults.size();
  empty.total_segment_count = segmentResults.size();
  if (segmentResults.empty()) {
    empty.reason = "insufficient_segment_evidence";
    return empty;
  }
  if (!is_finite_positive(referenceBpm)) {
    empty.reason = "invalid_input";
    return empty;
  }

  std::vector<ResultFamily> families;
  families.reserve(segmentResults.size());
  std::size_t peakCount = 0;
  std::size_t fitSegmentCount = 0;
  std::size_t candidateSegmentCount = 0;
  for (const HodgkinsonTatumProbeResult& result : segmentResults) {
    peakCount += result.odf_peak_count;
    if (result.candidate) {
      ++candidateSegmentCount;
    }
    if (result.loop_fit_score > 0.0 &&
        std::isfinite(result.loop_fit_score) &&
        result.candidate_bpm > 0.0 &&
        std::isfinite(result.candidate_bpm)) {
      add_family(families, result, referenceBpm);
      ++fitSegmentCount;
    }
  }
  empty.fit_segment_count = fitSegmentCount;
  empty.candidate_segment_count = candidateSegmentCount;
  empty.odf_peak_count = peakCount;
  empty.onset_count = peakCount;
  if (families.empty()) {
    empty.reason = "ambiguous_tatum_fit";
    return empty;
  }

  std::sort(families.begin(), families.end(),
            [](const ResultFamily& a, const ResultFamily& b) {
              if (a.score_sum != b.score_sum) {
                return a.score_sum > b.score_sum;
              }
              if (a.support != b.support) {
                return a.support > b.support;
              }
              return a.best_score > b.best_score;
            });

  const ResultFamily& dominant = families.front();
  const ResultFamily* runnerUp = families.size() > 1 ? &families[1] : nullptr;
  HodgkinsonTatumProbeResult best = dominant.representative;
  best.enabled = true;
  best.frontend_source = empty.frontend_source;
  best.input_bpm = referenceBpm;
  best.family_bpm = dominant.bpm;
  best.family_score = dominant.score_sum;
  best.segment_count = fitSegmentCount;
  best.total_segment_count = segmentResults.size();
  best.fit_segment_count = fitSegmentCount;
  best.candidate_segment_count = candidateSegmentCount;
  best.consensus_support = dominant.support;
  best.candidate_consensus_support = dominant.candidate_support;
  best.odf_peak_count = peakCount;
  best.onset_count = peakCount;
  best.loop_fit_confidence = best.loop_fit_score;
  if (runnerUp != nullptr) {
    best.runner_up_family_bpm = runnerUp->bpm;
    best.runner_up_family_score = runnerUp->score_sum;
    best.runner_up_family_support = runnerUp->support;
    best.runner_up_family_candidate_support = runnerUp->candidate_support;
  }
  best.family_rank_count = (std::min)(best.family_rank_bpm.size(), families.size());
  for (std::size_t i = 0; i < best.family_rank_count; ++i) {
    best.family_rank_bpm[i] = families[i].bpm;
    best.family_rank_score[i] = families[i].score_sum;
    best.family_rank_support[i] = families[i].support;
    best.family_rank_candidate_support[i] = families[i].candidate_support;
  }
  best.family_class =
      family_class(best.family_bpm, referenceBpm, dominant.support,
                   fitSegmentCount, best.runner_up_family_score,
                   dominant.score_sum);
  if (is_finite_positive(best.candidate_bpm)) {
    best.candidate_ratio_to_input = best.candidate_bpm / referenceBpm;
    best.candidate_ratio_class = ratio_class(best.candidate_ratio_to_input);
  }
  if (is_finite_positive(best.family_bpm)) {
    best.family_ratio_to_input = best.family_bpm / referenceBpm;
    best.family_ratio_class = ratio_class(best.family_ratio_to_input);
  }
  best.reason = "audacity_mir_full_segment_family_probe";
  return best;
}

HodgkinsonTatumProbeResult aggregate_hodgkinson_full_mir_only_results(
    const char* frontendSource,
    std::span<const HodgkinsonTatumProbeResult> segmentResults) noexcept {
  HodgkinsonTatumProbeResult empty;
  empty.enabled = true;
  empty.frontend_source =
      (frontendSource != nullptr && frontendSource[0] != '\0')
          ? frontendSource
          : "audacity_mir_full_only";
  empty.segment_count = segmentResults.size();
  if (segmentResults.empty()) {
    empty.reason = "insufficient_segment_evidence";
    return empty;
  }

  std::vector<ResultFamily> families;
  families.reserve(segmentResults.size());
  std::size_t peakCount = 0;
  std::size_t fitSegmentCount = 0;
  std::size_t candidateSegmentCount = 0;
  for (const HodgkinsonTatumProbeResult& result : segmentResults) {
    peakCount += result.odf_peak_count;
    if (result.candidate) {
      ++candidateSegmentCount;
    }
    if (result.loop_fit_score > 0.0 && std::isfinite(result.loop_fit_score) &&
        result.candidate_bpm > 0.0 && std::isfinite(result.candidate_bpm)) {
      add_raw_family(families, result);
      ++fitSegmentCount;
    }
  }
  empty.fit_segment_count = fitSegmentCount;
  empty.candidate_segment_count = candidateSegmentCount;
  empty.odf_peak_count = peakCount;
  empty.onset_count = peakCount;
  if (families.empty()) {
    empty.reason = "ambiguous_tatum_fit";
    return empty;
  }

  std::sort(families.begin(), families.end(),
            [](const ResultFamily& a, const ResultFamily& b) {
              if (a.score_sum != b.score_sum) {
                return a.score_sum > b.score_sum;
              }
              if (a.support != b.support) {
                return a.support > b.support;
              }
              return a.best_score > b.best_score;
            });

  const ResultFamily& dominant = families.front();
  const ResultFamily* runnerUp = families.size() > 1 ? &families[1] : nullptr;
  HodgkinsonTatumProbeResult best = dominant.representative;
  best.enabled = true;
  best.frontend_source = empty.frontend_source;
  best.input_bpm = 0.0;
  best.candidate_bpm = dominant.bpm;
  best.family_bpm = dominant.bpm;
  best.family_score = dominant.score_sum;
  best.segment_count = fitSegmentCount;
  best.total_segment_count = segmentResults.size();
  best.fit_segment_count = fitSegmentCount;
  best.candidate_segment_count = candidateSegmentCount;
  best.consensus_support = dominant.support;
  best.candidate_consensus_support = dominant.candidate_support;
  best.odf_peak_count = peakCount;
  best.onset_count = peakCount;
  best.loop_fit_confidence = best.loop_fit_score;
  if (runnerUp != nullptr) {
    best.runner_up_family_bpm = runnerUp->bpm;
    best.runner_up_family_score = runnerUp->score_sum;
    best.runner_up_family_support = runnerUp->support;
    best.runner_up_family_candidate_support = runnerUp->candidate_support;
  }
  best.family_rank_count = (std::min)(best.family_rank_bpm.size(), families.size());
  for (std::size_t i = 0; i < best.family_rank_count; ++i) {
    best.family_rank_bpm[i] = families[i].bpm;
    best.family_rank_score[i] = families[i].score_sum;
    best.family_rank_support[i] = families[i].support;
    best.family_rank_candidate_support[i] = families[i].candidate_support;
  }
  best.family_class = "hodgkinson_only_raw";
  best.candidate_ratio_class = "independent";
  best.family_ratio_class = "independent";
  best.reason = "audacity_mir_full_only_segment_family_probe";
  return best;
}

}  // namespace smart_tempo
