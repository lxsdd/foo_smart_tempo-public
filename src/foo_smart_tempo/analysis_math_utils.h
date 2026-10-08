#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace smart_tempo {

struct GaussianLut {
  static constexpr double kMaxNormalizedDistance = 8.0;
  static constexpr std::size_t kTableSize = 4096;
  static constexpr double kScale = kTableSize / kMaxNormalizedDistance;
  std::array<float, kTableSize + 1> table{};

  GaussianLut();

  [[nodiscard]] double eval(double normalizedDistance) const noexcept;
};

const GaussianLut& gaussian_lut() noexcept;

double clamp_confidence(double v) noexcept;

double normalize_bpm_ratio_distance(double ratio, double target) noexcept;

bool ratio_near(double ratio, double target, double tolerance) noexcept;

double harmonic_agreement_score(double bpmA, double bpmB) noexcept;

bool is_route_polyrhythm_relation(double lhsBpm, double rhsBpm) noexcept;

double mode_tolerance_for_bpm(double bpm) noexcept;

double initial_confidence_for_mode() noexcept;

double compute_confidence_score(const std::vector<double>& candidates,
                                double chosenBpm) noexcept;

double median_of_sorted(const std::vector<double>& sorted) noexcept;


double compute_bpm_from_detected_beat_samples(const std::vector<uint64_t>& beatDetectionSamples,
                                              double sampleRate, double bpmMin, double bpmMax,
                                              double* outMedianDeltaSec,
                                              double* outRawBpm);

} // namespace smart_tempo
