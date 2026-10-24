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

inline bool record(const std::vector<double>& bpm,
                   const std::vector<uint8_t>& withheld) {
  if (read_env(L"GITHUB_ACTIONS") != L"true" ||
      bpm.size() != 1 || withheld.size() != 1) return false;

  std::wstring root = read_env(L"RUNNER_TEMP");
  const std::wstring fixture = read_env(L"SMART_TEMPO_CI_FIXTURE");
  if (root.empty() || fixture.empty()) return false;
  if (root.back() != L'\\') root.push_back(L'\\');
  if (fixture.size() <= root.size() ||
      _wcsnicmp(root.c_str(), fixture.c_str(), root.size()) != 0) return false;

  // Explicit synthetic-only allowlist. Never accept arbitrary fixture names,
  // parent traversal, private media, or production component configurations.
  constexpr const wchar_t* kCases[] = {
      L"pulse_90", L"pulse_128", L"pulse_175",
      L"pulse_128_syncopated", L"pulse_128_half_pulse", L"silence"};
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
  return true;
}
}  // namespace smart_tempo::ci_oracle
