#pragma once
#ifndef SMART_TEMPO_CI_ORACLE_PROBE
#error "Only the ephemeral CI host probe may include this result observer"
#endif

// Diagnostic values and public-fixture-only window overrides; NO media/tag writes.
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


// All CI control surfaces share the exact same disposable, synthetic WAV
// allowlist. An arbitrary CI environment variable is never enough to activate
// an override against non-public media or ordinary component builds.
inline std::wstring approved_fixture() {
  if (read_env(L"GITHUB_ACTIONS") != L"true") return L"";
  std::wstring root = read_env(L"RUNNER_TEMP");
  const std::wstring fixture = read_env(L"SMART_TEMPO_CI_FIXTURE");
  if (root.empty() || fixture.empty()) return L"";
  if (fixture.find(L"..") != std::wstring::npos ||
      fixture.find(L'/') != std::wstring::npos) return L"";
  if (root.back() != L'\\') root.push_back(L'\\');
  if (fixture.size() <= root.size() ||
      _wcsnicmp(root.c_str(), fixture.c_str(), root.size()) != 0) return L"";

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
      L"drum_60_four_floor", L"drum_90_four_floor", L"drum_110_four_floor", L"drum_120_four_floor", L"drum_128_four_floor", L"drum_140_four_floor", L"drum_175_four_floor", L"drum_123_45_four_floor", L"drum_90_breakbeat", L"drum_128_breakbeat", L"drum_175_breakbeat", L"drum_128_8s", L"drum_128_44100_stereo",
      L"drum_75_shuffle", L"drum_110_shuffle", L"drum_160_shuffle", L"drum_95_triplet", L"drum_125_triplet", L"drum_185_triplet", L"drum_90_half_time", L"drum_150_half_time", L"drum_100_odd_five", L"drum_142_odd_five", L"drum_105_dropouts", L"drum_170_dropouts", L"drum_124_dense_hats", L"drum_174_dense_hats", L"drum_112_poly_three", L"drum_176_poly_three", L"drum_88_ghost_notes", L"drum_137_ghost_notes", L"nonperiodic_percussion", L"tempo_90_128_switch",
      L"prospective_four_floor_97_25", L"prospective_four_floor_133_70",
      L"prospective_four_floor_181_30", L"prospective_breakbeat_96_70",
      L"prospective_breakbeat_144_20", L"prospective_breakbeat_189_60",
      L"prospective_shuffle_83_80", L"prospective_shuffle_134_60",
      L"prospective_triplet_107_20", L"prospective_triplet_156_40",
      L"prospective_odd_five_117_30", L"prospective_odd_five_149_20",
      L"prospective_poly_three_143_60", L"prospective_dense_hats_118_40",
      L"prospective_half_pulse_153_60", L"prospective_sparse_117_80",
      L"prospective_tempo_switch_alt", L"prospective_nonperiodic_alt",
      L"prospective_tone_alt", L"prospective_noise_alt",
      // Only fixed, public synthetic meter WAVs in disposable CI sessions.
      L"meter_3_4_120_quarter", L"meter_6_8_80_dotted",
      L"meter_3_4_150_quarter", L"meter_6_8_100_dotted",
      L"meter_ambiguous_120q_80dq",
      L"challenge_73_25_cross32", L"challenge_97_25_cross54",
      L"challenge_129_37_clave", L"challenge_161_80_odd7",
      L"challenge_189_60_halftrap", L"challenge_67_40_doubletrap",
      L"challenge_143_60_ghostswing", L"challenge_118_75_phasehats",
      L"challenge_103_30_sparsekick", L"challenge_151_25_humanized",
      L"challenge_84_60_masked", L"challenge_212_50_fast",
      L"challenge_57_35_slow", L"challenge_108_142_switch",
      L"challenge_nonperiodic", L"challenge_steady_tone",
      L"window_128_four_floor", L"window_175_breakbeat", L"window_110_shuffle",
      L"window_100_odd_five", L"window_90_128_switch",
      L"window_nonperiodic_percussion"};
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
  if (!approved) return L"";
  return fixture;
}

inline bool override_window_geometry(int& seconds, int& passes) {
  if (approved_fixture().empty()) return false;
  const std::wstring window = read_env(L"SMART_TEMPO_CI_WINDOW_SECONDS");
  const std::wstring pass = read_env(L"SMART_TEMPO_CI_SAMPLE_PASSES");
  struct Choice { const wchar_t* text; int value; };
  constexpr Choice windows[] = {
      {L"5", 5}, {L"10", 10}, {L"15", 15}, {L"20", 20},
      {L"30", 30}, {L"45", 45}, {L"60", 60}, {L"90", 90}};
  constexpr Choice passesAllowed[] = {
      {L"1", 1}, {L"3", 3}, {L"5", 5}, {L"10", 10},
      {L"20", 20}, {L"50", 50}};
  int desiredSeconds = 0, desiredPasses = 0;
  for (const Choice& c : windows)
    if (window == c.text) desiredSeconds = c.value;
  for (const Choice& c : passesAllowed)
    if (pass == c.text) desiredPasses = c.value;
  if (desiredSeconds == 0 || desiredPasses == 0) return false;
  seconds = desiredSeconds;
  passes = desiredPasses;
  return true;
}

// Probe-only, deterministic early/late sampling offsets (single pass). The
// test dispatcher, allowlist and no-write guards remain mandatory.
inline bool override_single_pass_offset(int samplePasses,
                                        int& minPct, int& maxPct) {
  if (samplePasses != 1 || approved_fixture().empty()) return false;
  const std::wstring offset = read_env(L"SMART_TEMPO_CI_OFFSET_PCT");
  if (offset == L"20") {
    minPct = 20; maxPct = 20; return true;
  }
  if (offset == L"80") {
    minPct = 80; maxPct = 80; return true;
  }
  return false;
}

// The observer emits actual host geometry, not merely the requested values.
// Only public disposable fixtures get this output, and only in probe builds.
inline bool record_window_plan(int requestedSeconds, int requestedPasses,
                               double effectiveSeconds, int effectivePasses,
                               int offsetMinPct, int offsetMaxPct) {
  if (read_env(L"SMART_TEMPO_CI_WINDOW_SECONDS").empty()) return false;
  const std::wstring fixture = approved_fixture();
  if (fixture.empty() || !std::isfinite(effectiveSeconds) ||
      effectiveSeconds <= 0.0 || effectivePasses <= 0) return false;
  std::string csv =
      "requested_seconds,requested_passes,effective_seconds,effective_passes,offset_min_pct,offset_max_pct\n";
  csv += std::to_string(requestedSeconds) + "," +
         std::to_string(requestedPasses) + "," +
         std::to_string(effectiveSeconds) + "," +
         std::to_string(effectivePasses) + "," +
         std::to_string(offsetMinPct) + "," +
         std::to_string(offsetMaxPct) + "\n";
  const std::wstring output = fixture + L".plan.csv";
  HANDLE h = CreateFileW(output.c_str(), GENERIC_WRITE, 0, nullptr,
                         CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  const BOOL ok = WriteFile(h, csv.data(), static_cast<DWORD>(csv.size()),
                            &written, nullptr);
  CloseHandle(h);
  if (!ok || written != csv.size()) {
    DeleteFileW(output.c_str());
    return false;
  }
  return true;
}

// Only fixed public CI fixtures may disclose bounded decision enums. The
// production diagnostic note may contain arbitrary text, so retain only its
// known measured-policy prefix and an ASCII token; never serialize paths.
inline std::string bounded_policy_token(const char* value) {
  if (value == nullptr || value[0] == '\0') return "none";
  std::string token;
  for (const unsigned char* p =
           reinterpret_cast<const unsigned char*>(value); *p != 0; ++p) {
    if (token.size() >= 96 ||
        !((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
          (*p >= '0' && *p <= '9') || *p == '_' || *p == '-')) {
      return "invalid";
    }
    token.push_back(static_cast<char>(*p));
  }
  return token;
}

inline bool record_policy(const char* policyReason,
                          const char* decisionClass,
                          const char* decisionNote) {
  const std::wstring fixture = approved_fixture();
  if (fixture.empty()) return false;
  constexpr const char* prefix = "Hodgkinson/MIR measured policy: ";
  const char* measuredReason =
      decisionNote != nullptr &&
              std::strncmp(decisionNote, prefix, std::strlen(prefix)) == 0
          ? decisionNote + std::strlen(prefix)
          : nullptr;
  std::string csv =
      "policy_reason,decision_class,measured_policy_reason\n";
  csv += bounded_policy_token(policyReason) + "," +
         bounded_policy_token(decisionClass) + "," +
         bounded_policy_token(measuredReason) + "\n";
  const std::wstring path = fixture + L".policy.csv";
  HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  const BOOL ok = WriteFile(handle, csv.data(), static_cast<DWORD>(csv.size()),
                            &written, nullptr);
  CloseHandle(handle);
  if (!ok || written != csv.size()) {
    DeleteFileW(path.c_str());
    return false;
  }
  return true;
}

// Numeric provenance only in synthetic CI; no source strings leave this file.
inline int bounded_source_bits(const char* source) noexcept {
  if (source == nullptr || *source == '\0') return 0;
  struct Label { const char* token; int mask; };
  constexpr Label labels[] = {
      {"full_board", 1}, {"primary_selector", 2},
      {"material_risk_probe", 4}, {"partial_bar_aggregate", 8},
      {"partial_bar_topk", 16}};
  int bits = 0;
  const char* cursor = source;
  while (*cursor) {
    const char* end = std::strchr(cursor, '|');
    const std::size_t n = end ? static_cast<std::size_t>(end - cursor)
                              : std::strlen(cursor);
    bool known = false;
    for (const auto& label : labels) {
      if (std::strlen(label.token) == n &&
          std::strncmp(cursor, label.token, n) == 0) {
        if (bits & label.mask) return 0;
        bits |= label.mask;
        known = true;
        break;
      }
    }
    if (!known) return 0;
    if (end == nullptr) break;
    cursor = end + 1;
    if (*cursor == '\0') return 0;
  }
  return bits;
}

template <typename CandidateGroups>
inline bool record(const std::vector<double>& bpm,
                   const std::vector<uint8_t>& withheld,
                   const CandidateGroups& candidates) {
  if (read_env(L"GITHUB_ACTIONS") != L"true" ||
      bpm.size() != 1 || withheld.size() != 1 ||
      candidates.size() != 1) return false;

  const std::wstring fixture = approved_fixture();
  if (fixture.empty()) return false;
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
  const bool challenge = fixture.find(L"challenge_") != std::wstring::npos;
  std::string origins = "rank,bpm,source_bits\n";
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
    if (challenge) {
      const int bits = bounded_source_bits(candidate.sources.get_ptr());
      if (bits == 0) return false;
      origins += std::to_string(candidate.evidence_rank) + "," +
                 std::to_string(candidate.bpm) + "," +
                 std::to_string(bits) + "\n";
    }
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
  if (challenge) {
    const std::wstring path = fixture + L".sources.csv";
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD count = 0;
    const BOOL wrote = WriteFile(h, origins.data(),
                                 static_cast<DWORD>(origins.size()),
                                 &count, nullptr);
    CloseHandle(h);
    if (!wrote || count != origins.size()) {
      DeleteFileW(path.c_str());
      return false;
    }
  }
  return true;
}
}  // namespace smart_tempo::ci_oracle
