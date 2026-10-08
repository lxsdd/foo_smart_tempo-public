#include "stdafx.h"

#include "analysis_math_utils.h"

#include "foo_smart_tempo.h"
#include "preferences.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace smart_tempo {

GaussianLut::GaussianLut() {
  for (std::size_t i = 0; i <= kTableSize; ++i) {
    const double x = static_cast<double>(i) / kScale;
    table[i] = static_cast<float>(std::exp(-0.5 * x * x));
  }
}

double GaussianLut::eval(double normalizedDistance) const noexcept {
  if (normalizedDistance <= 0.0) return 1.0;
  if (normalizedDistance >= kMaxNormalizedDistance) return 0.0;
  const double pos = normalizedDistance * kScale;
  const std::size_t idx = static_cast<std::size_t>(pos);
  const double frac = pos - static_cast<double>(idx);
  const double a = table[idx];
  const double b = table[idx + 1];
  return a + (b - a) * frac;
}

const GaussianLut& gaussian_lut() noexcept {
  static const GaussianLut lut;
  return lut;
}

double clamp_confidence(double v) noexcept {
  if (v < 0.0) return 0.0;
  if (v > 100.0) return 100.0;
  return v;
}

double normalize_bpm_ratio_distance(double ratio, double target) noexcept {
  return std::abs(ratio - target) / (target > 0.0 ? target : 1.0);
}

bool ratio_near(double ratio, double target, double tolerance) noexcept {
  return std::isfinite(ratio) && std::abs(ratio - target) <= tolerance;
}

double harmonic_agreement_score(double bpmA, double bpmB) noexcept {
  if (!(bpmA > 0.0) || !(bpmB > 0.0)) return 0.0;
  const double ratio = bpmA / bpmB;
  constexpr std::array<double, 7> kHarmonicRatios = {
      1.0, 2.0, 0.5, 1.5, 0.75, 4.0 / 3.0, 2.0 / 3.0};
  double bestRelError = std::numeric_limits<double>::infinity();
  for (double target : kHarmonicRatios) {
    bestRelError = (std::min)(bestRelError,
                              normalize_bpm_ratio_distance(ratio, target));
  }
  if (bestRelError <= 0.005) return 1.0;
  if (bestRelError >= 0.03) return 0.0;
  return 1.0 - ((bestRelError - 0.005) / 0.025);
}

bool is_route_polyrhythm_relation(double lhsBpm, double rhsBpm) noexcept {
  if (!(lhsBpm > 0.0) || !(rhsBpm > 0.0) ||
      !std::isfinite(lhsBpm) || !std::isfinite(rhsBpm)) {
    return false;
  }

  const double ratio = lhsBpm / rhsBpm;
  constexpr double kRatioTolerance = 0.025;
  return ratio_near(ratio, 0.75, kRatioTolerance) ||
         ratio_near(ratio, 4.0 / 3.0, kRatioTolerance) ||
         ratio_near(ratio, 1.5, kRatioTolerance) ||
         ratio_near(ratio, 2.0 / 3.0, kRatioTolerance);
}

double mode_tolerance_for_bpm(double bpm) noexcept {
  return (bpm * 0.008 > 0.6) ? (bpm * 0.008) : 0.6;
}

double initial_confidence_for_mode() noexcept {
  return 0.0;
}

double compute_confidence_score(const std::vector<double>& candidates,
                                double chosenBpm) noexcept {
  if (candidates.empty() || chosenBpm <= 0.0) return 0.0;
  const size_t n = candidates.size();
  if (n == 1) return 55.0;

  const double tightWindow = mode_tolerance_for_bpm(chosenBpm);
  const double wideWindow = (chosenBpm * 0.020 > 1.2) ? (chosenBpm * 0.020) : 1.2;

  size_t tightHits = 0;
  size_t wideHits = 0;
  double mad = 0.0;
  for (double value : candidates) {
    const double diff = std::abs(value - chosenBpm);
    if (diff <= tightWindow) ++tightHits;
    if (diff <= wideWindow) ++wideHits;
    mad += diff;
  }

  const double nf = (double)n;
  mad /= nf;
  const double tightConsensus = (double)tightHits / nf;
  const double wideConsensus = (double)wideHits / nf;

  const double spreadRef = (chosenBpm * 0.06 > 1.6) ? (chosenBpm * 0.06) : 1.6;
  double spreadNorm = (spreadRef > 0.0) ? (mad / spreadRef) : 1.0;
  if (spreadNorm < 0.0) spreadNorm = 0.0;
  if (spreadNorm > 1.0) spreadNorm = 1.0;
  const double spreadQuality = 1.0 - spreadNorm;

  double raw = 0.50 * wideConsensus + 0.35 * tightConsensus + 0.15 * spreadQuality;
  const double fullTrustAt =
      (std::max)(4.0, static_cast<double>(get_default_analysis_sample_passes()));
  double countTrust = 0.75 + 0.25 * (nf / fullTrustAt);
  if (countTrust > 1.0) countTrust = 1.0;
  raw *= countTrust;

  if (raw < 0.0) raw = 0.0;
  if (raw > 1.0) raw = 1.0;

  double score = 100.0 * std::pow(raw, 0.45);
  if (score < 5.0 && wideHits > 0) score = 5.0;
  return clamp_confidence(score);
}

double median_of_sorted(const std::vector<double>& sorted) noexcept {
  if (sorted.empty()) return 0.0;
  const size_t mid = sorted.size() / 2;
  if ((sorted.size() % 2) == 0) return (sorted[mid - 1] + sorted[mid]) * 0.5;
  return sorted[mid];
}

double compute_bpm_from_detected_beat_samples(const std::vector<uint64_t>& beatDetectionSamples,
                                              double sampleRate, double bpmMin, double bpmMax,
                                              double* outMedianDeltaSec,
                                              double* outRawBpm) {
  if (outMedianDeltaSec != nullptr) *outMedianDeltaSec = 0.0;
  if (outRawBpm != nullptr) *outRawBpm = 0.0;
  if (beatDetectionSamples.size() < 2 || sampleRate <= 0.0) return 0.0;

  const double minDeltaSec = 60.0 / bpmMax;
  const double maxDeltaSec = 60.0 / bpmMin;
  std::vector<double> deltaSeconds;
  deltaSeconds.reserve(beatDetectionSamples.size() - 1);

  for (size_t i = 1; i < beatDetectionSamples.size(); ++i) {
    const uint64_t prev = beatDetectionSamples[i - 1];
    const uint64_t cur = beatDetectionSamples[i];
    if (cur <= prev) continue;
    const uint64_t deltaSamples = cur - prev;
    const double deltaSec = (double)deltaSamples / sampleRate;
    if (deltaSec < minDeltaSec || deltaSec > maxDeltaSec) continue;
    deltaSeconds.push_back(deltaSec);
  }

  if (deltaSeconds.empty()) return 0.0;
  std::sort(deltaSeconds.begin(), deltaSeconds.end());
  const double medianDeltaSec = median_of_sorted(deltaSeconds);
  if (outMedianDeltaSec != nullptr) *outMedianDeltaSec = medianDeltaSec;
  if (medianDeltaSec <= 0.0) return 0.0;

  const double rawBpm = 60.0 / medianDeltaSec;
  if (outRawBpm != nullptr) *outRawBpm = rawBpm;
  return std::clamp(rawBpm, bpmMin, bpmMax);
}

} // namespace smart_tempo
