#pragma once
#ifndef SMART_TEMPO_CI_ORACLE_PROBE
#error "Only the ephemeral CI host probe may include this result observer"
#endif

// Diagnostic values ONLY; no change to MIR selection and NO media/tag writes.
// The observer is compiled out of every personal/distributable component.
#include <Windows.h>
#include <cmath>
#include <cwchar>
#include <cstring>
#include <string>
#include <vector>
#include <cstdint>

namespace smart_tempo::ci_oracle {
inline std::wstring read_env(const wchar_t* key) {
  wchar_t data[32768] = {};
  const DWORD n = GetEnvironmentVariableW(key, data, 32768);
  return n > 0 && n < 32768 ? std::wstring(data, n) : L"";
}

template <typename CandidateGroups>
inline bool record(const std::vector<double>& bpm,
                   const std::vector<uint8_t>& withheld,
                   const CandidateGroups& candidates) {
  if (read_env(L"GITHUB_ACTIONS") != L"true" ||
      bpm.size() != 1 || withheld.size() != 1 ||
      candidates.size() != 1) return false;

  std::wstring root = read_env(L"RUNNER_TEMP");
  const std::wstring fixture = read_env(L"SMART_TEMPO_CI_FIXTURE");
  if (root.empty() || fixture.empty()) return false;
  if (root.back() != L'\\') root.push_back(L'\\');
  if (fixture.size() <= root.size() ||
      _wcsnicmp(root.c_str(), fixture.c_str(), root.size()) != 0) return false;

  // Explicit synthetic-only allowlist. Never accept arbitrary fixture names,
  // parent traversal, private media, or production component configurations.
  constexpr const wchar_t* kCases[] = {
      L"pulse_60", L"pulse_90", L"pulse_120", L"pulse_128",
      L"pulse_140", L"pulse_175", L"pulse_128_syncopated", L"pulse_120_swing",
      L"pulse_128_half_pulse", L"pulse_120_sparse", L"silence", L"pulse_40",
      L"pulse_45", L"pulse_50", L"pulse_70", L"pulse_80",
      L"pulse_100", L"pulse_110", L"pulse_130", L"pulse_150",
      L"pulse_160", L"pulse_180", L"pulse_200", L"pulse_220",
      L"pulse_59_73", L"pulse_93_37", L"pulse_123_45", L"pulse_127_31",
      L"pulse_174_62", L"pulse_199_25", L"pulse_90_syncopated", L"pulse_175_syncopated",
      L"pulse_90_swing", L"pulse_175_swing", L"pulse_90_half_pulse", L"pulse_175_half_pulse",
      L"pulse_128_sparse", L"pulse_175_sparse", L"pulse_128_8s", L"pulse_128_12s",
      L"pulse_128_40s", L"pulse_128_44100_mono", L"pulse_128_48000_stereo", L"pulse_123_45_44100_stereo",
      L"single_click", L"steady_tone", L"noise",
      L"drum_60_four_floor", L"drum_90_four_floor", L"drum_110_four_floor", L"drum_120_four_floor", L"drum_128_four_floor", L"drum_140_four_floor", L"drum_175_four_floor", L"drum_123_45_four_floor", L"drum_90_breakbeat", L"drum_128_breakbeat", L"drum_175_breakbeat", L"drum_128_8s", L"drum_128_44100_stereo"};
  bool approved = false;
  for (const wchar_t* id : kCases) {
    std::wstring suffix = L"\\synthetic-oracle-";
    suffix += id;
    suffix += L"\\";
    suffix += id;
    suffix += L".wav";
    if (fixture.size() >= suffix.size() &&
        _wcsicmp(fixture.c_str() + fixture.size() - suffix.size(),
                 suffix.c_str()) == 0) {
      approved = true;
      break;
    }
  }
  if (!approved) return false;
  const std::wstring report = fixture + L".results.csv";
  const double numeric = std::isfinite(bpm[0]) && bpm[0] > 0.0
                             ? bpm[0] : 0.0;
  std::string csv = "index,bpm,held\n0,";
  csv += std::to_string(numeric);
  csv += ",";
  csv += withheld[0] ? "1\n" : "0\n";
  HANDLE handle = CreateFileW(report.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return false;
  DWORD count = 0;
  const BOOL wrote = WriteFile(handle, csv.data(),
                               static_cast<DWORD>(csv.size()), &count,
                               nullptr);
  CloseHandle(handle);
  if (!wrote || count != csv.size()) {
    DeleteFileW(report.c_str());
    return false;
  }

  // Only numeric, bounded evidence from the already-computed MIR candidate
  // board. No filenames, genre, artist, source strings or reference BPM are
  // serialized. This sidecar never affects a production analysis decision.
  std::string evidence = "rank,bpm,evidence,support,winner,local_exact\n";
  const auto& rows = candidates.front();
  const std::size_t maxRows = rows.size() < 32 ? rows.size() : 32;
  for (std::size_t index = 0; index < maxRows; ++index) {
    const auto& candidate = rows[index];
    if (!std::isfinite(candidate.bpm) || candidate.bpm <= 0.0 ||
        !std::isfinite(candidate.evidence_score) ||
        !std::isfinite(candidate.support) ||
        !std::isfinite(candidate.winner_support) ||
        !std::isfinite(candidate.local_exact_score)) {
      continue;
    }
    evidence += std::to_string(candidate.evidence_rank);
    evidence += "," + std::to_string(candidate.bpm);
    evidence += "," + std::to_string(candidate.evidence_score);
    evidence += "," + std::to_string(candidate.support);
    evidence += "," + std::to_string(candidate.winner_support);
    evidence += "," + std::to_string(candidate.local_exact_score) + "\n";
  }
  const std::wstring evidencePath = fixture + L".candidates.csv";
  HANDLE evidenceHandle = CreateFileW(
      evidencePath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
      FILE_ATTRIBUTE_NORMAL, nullptr);
  if (evidenceHandle == INVALID_HANDLE_VALUE) return false;
  DWORD evidenceCount = 0;
  const BOOL evidenceWritten = WriteFile(
      evidenceHandle, evidence.data(), static_cast<DWORD>(evidence.size()),
      &evidenceCount, nullptr);
  CloseHandle(evidenceHandle);
  if (!evidenceWritten || evidenceCount != evidence.size()) {
    DeleteFileW(evidencePath.c_str());
    return false;
  }
  return true;
}
}  // namespace smart_tempo::ci_oracle
