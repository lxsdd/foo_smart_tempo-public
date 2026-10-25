#pragma once
#ifndef SMART_TEMPO_CI_ORACLE_PROBE
#error "CI-only Smart Tempo pipeline observer must not be included in production"
#endif

// Public, disposable, synthetic-host tracing ONLY. No file names, audio,
// genres, tags, aliases as free text, or owner data are serialized.
// This is an observational pool-lineage trace, NOT a claim that every
// collapsed candidate passed every selector's internal eligibility gate.
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace smart_tempo::ci_pipeline_trace {
inline std::wstring env(const wchar_t* name) {
  wchar_t buffer[32768] = {};
  const DWORD n = GetEnvironmentVariableW(name, buffer, 32768);
  return n > 0 && n < 32768 ? std::wstring(buffer, n) : L"";
}

inline std::wstring permitted_fixture() {
  if (env(L"GITHUB_ACTIONS") != L"true") return {};
  std::wstring root = env(L"RUNNER_TEMP");
  const std::wstring fixture = env(L"SMART_TEMPO_CI_FIXTURE");
  if (root.empty() || root.back() == L'/' || root.back() == L'\\' ||
      fixture.empty()) return {};
  root += L"\\";
  if (fixture.size() <= root.size() ||
      _wcsnicmp(root.c_str(), fixture.c_str(), root.size()) != 0) return {};
  constexpr wchar_t suffix[] = L"\\synthetic-oracle\\pulse_128.wav";
  constexpr std::size_t count = (sizeof(suffix)/sizeof(suffix[0])) - 1;
  if (fixture.size() < count ||
      _wcsicmp(fixture.c_str() + fixture.size() - count, suffix) != 0)
    return {};
  return fixture;
}

// Alias expansion can legitimately produce BPM outside the production rail.
// Only final accepted BPM is constrained by the 40..220 production decision.
inline bool finite_bpm(double bpm) { return std::isfinite(bpm) && bpm >= 0.0 && bpm <= 2000.0; }
inline std::string safe_num(double value) {
  // Callers must validate all numeric evidence, never silently map NaN to 0.
  return std::to_string(value);
}
inline std::string ci_run_id() {
  const auto run = env(L"GITHUB_RUN_ID");
  if (run.empty() || run.size() > 32) return {};
  uint64_t hash = UINT64_C(14695981039346656037);
  for (wchar_t c : run) {
    if (c < L'0' || c > L'9') return {};
    hash = (hash ^ static_cast<uint64_t>(c)) * UINT64_C(1099511628211);
  }
  hash = (hash ^ GetCurrentProcessId()) * UINT64_C(1099511628211);
  hash = (hash ^ GetCurrentThreadId()) * UINT64_C(1099511628211);
  char digits[17] = {};
  std::snprintf(digits, sizeof(digits), "%016llx",
                static_cast<unsigned long long>(hash));
  return std::string(digits);
}

struct EventWriter {
  std::string run_id;
  explicit EventWriter(std::string id) : run_id(std::move(id)) {}
  std::string lines;
  std::size_t sequence = 0;
  std::size_t next_id = 0;
  std::string create_id() { return "c" + std::to_string(next_id++); }
  void push(const char* stage, const char* outcome, const char* reason,
            const std::string& id, const std::vector<std::string>& parents,
            double bpm, double score, double support) {
    lines += "{\"schema\":\"smart_tempo_source_decision_trace_v1\",";
    lines += "\"analysis_id\":\"" + run_id + "\",\"sequence\":" + std::to_string(sequence++);
    lines += ",\"stage\":\"" + std::string(stage) + "\",\"outcome\":\"" +
             std::string(outcome) + "\",\"reason_code\":\"" + std::string(reason) + "\"";
    lines += ",\"candidate_id\":" + (id.empty() ? "null" : "\"" + id + "\"");
    lines += ",\"parent_candidate_ids\":[";
    for (std::size_t i = 0; i < parents.size(); ++i) {
      if (i) lines += ",";
      lines += "\"" + parents[i] + "\"";
    }
    lines += "],\"metrics\":{\"bpm\":" + safe_num(bpm);
    lines += ",\"score\":" + safe_num(score);
    lines += ",\"support\":" + safe_num(support) + "}}\n";
  }
};

inline bool record(
    const mir_pipeline::MirCandidateBoard& board,
    const std::vector<mir_pipeline::MirCandidate>& collapsed,
    const std::vector<mir_pipeline::MirPolicyMeasuredCandidate>& fullboard,
    std::span<const mir_pipeline::MirCiSelectorEvent> selectorEvents,
    const MirPrimarySelection& primary,
    const MirPolicyDecision& policy,
    double final_bpm, bool withheld, bool same_run_measured_witness) {
  const std::wstring fixture = permitted_fixture();
  const auto run_id = ci_run_id();
  if (fixture.empty() || run_id.empty() ||
      !finite_bpm(final_bpm) || final_bpm > 220.0 ||
      (!withheld && final_bpm > 0.0 && !same_run_measured_witness))
    return false;
  if (board.raw_candidates.size() > 4096 || collapsed.size() > 4096 ||
      selectorEvents.size() > 4096 || fullboard.size() > 4096 ||
      collapsed.size() != primary.candidate_count)
    return false;
  EventWriter out(run_id);
  std::vector<std::string> raw_ids, cluster_ids, policy_ids;
  raw_ids.reserve(board.raw_candidates.size());
  cluster_ids.reserve(collapsed.size());
  policy_ids.reserve(fullboard.size());
  for (const auto& raw : board.raw_candidates) {
    if (!finite_bpm(raw.bpm) || raw.bpm == 0.0 ||
        !std::isfinite(raw.score_sum) || !std::isfinite(raw.synthetic_support))
      return false;
    const auto id = out.create_id();
    raw_ids.push_back(id);
    out.push("candidate_create", "observed", "raw_alias", id, {},
             raw.bpm, raw.score_sum, raw.synthetic_support);
  }
  for (const auto& cluster : collapsed) {
    if (!finite_bpm(cluster.bpm) || cluster.bpm == 0.0 ||
        !std::isfinite(cluster.score_sum) || !std::isfinite(cluster.support) ||
        !std::isfinite(cluster.best_combined_score))
      return false;
    std::vector<std::string> parents;
    for (std::size_t i = 0; i < board.raw_candidates.size(); ++i) {
      if (std::abs(board.raw_candidates[i].bpm - cluster.bpm) < 0.001)
        parents.push_back(raw_ids[i]);
    }
    if (parents.empty() || parents.size() > 128) return false;
    const auto id = out.create_id();
    cluster_ids.push_back(id);
    out.push("candidate_transform", "observed", "cluster_from_raw", id,
             parents, cluster.bpm, cluster.score_sum, cluster.support);
    out.push("primary_eligibility", "observed", "pool_only_gate_unresolved",
             id, {}, cluster.bpm, cluster.best_combined_score, cluster.support);
  }
  for (const auto& candidate : fullboard) {
    if (!finite_bpm(candidate.bpm) || candidate.bpm == 0.0 ||
        !std::isfinite(candidate.local_exact_score) ||
        !std::isfinite(candidate.candidate.support))
      return false;
    std::vector<std::string> parents;
    for (std::size_t i = 0; i < collapsed.size(); ++i) {
      if (std::abs(collapsed[i].bpm - candidate.cluster_bpm) < 0.001)
        parents.push_back(cluster_ids[i]);
    }
    const auto id = out.create_id();
    policy_ids.push_back(id);
    out.push("policy_eligibility", "included",
             parents.empty() ? "full_board_origin_unknown" : "full_board_member",
             id, parents, candidate.bpm,
             candidate.local_exact_score, candidate.candidate.support);
  }
  // Executed selector checks, not a fabricated ranking over the merged
  // manual-review pool. Provisional exclusions may be rescued by fallback.
  for (const auto& item : selectorEvents) {
    if (!finite_bpm(item.bpm) || item.bpm <= 0.0 ||
        !std::isfinite(item.score)) return false;
    const std::size_t count = item.raw ? board.raw_candidates.size() : collapsed.size();
    const auto& ids = item.raw ? raw_ids : cluster_ids;
    if (item.source_index >= count) return false;
    const double actualBpm = item.raw
        ? board.raw_candidates[item.source_index].bpm
        : collapsed[item.source_index].bpm;
    if (std::abs(actualBpm - item.bpm) >= 0.001) return false;
    // Source index is preserved from the ACTUAL executed selector vector:
    // same-BPM alias rows no longer collapse onto the first matching raw ID.
    const bool ranking = std::strcmp(item.reason, "raw_rank_scored") == 0 ||
                         std::strcmp(item.reason, "harmonic_rank_scored") == 0 ||
                         std::strcmp(item.reason, "general_baseline_rank_scored") == 0 ||
                         std::strcmp(item.reason, "general_alt_rank_scored") == 0;
    const bool terminal = std::strncmp(item.reason, "terminal_", 9) == 0;
    out.push(terminal ? "primary_terminal" :
             ranking ? "primary_rank" : "candidate_filter",
             terminal ? "selected" :
                 item.included ? "included" : "excluded",
             item.reason, ids[item.source_index], {}, item.bpm, item.score, 0.0);
  }
  if (!std::isfinite(primary.selected_local_exact_score) ||
      !std::isfinite(primary.support) ||
      !std::isfinite(policy.candidate_local_exact_score) ||
      !std::isfinite(policy.candidate_support))
    return false;
  std::vector<std::string> primary_parent;
  for (std::size_t i = 0; i < collapsed.size(); ++i) {
    if (primary.selected_bpm > 0.0 &&
        std::abs(collapsed[i].bpm - primary.selected_bpm) < 0.001)
      primary_parent.push_back(cluster_ids[i]);
  }
  out.push("primary_select",
           primary.review_hold ? "review_hold" :
           primary.auto_candidate ? "selected" : "no_candidate",
           primary.review_hold ? "primary_review_hold" :
           primary.auto_candidate ? "primary_auto" : "primary_no_candidate",
           "", primary_parent, primary.selected_bpm,
           primary.selected_local_exact_score, primary.support);
  std::vector<std::string> policy_parent;
  for (std::size_t i = 0; i < fullboard.size(); ++i) {
    if (policy.output_bpm > 0.0 &&
        std::abs(fullboard[i].bpm - policy.output_bpm) <= 0.20)
      policy_parent.push_back(policy_ids[i]);
  }
  out.push("policy_change",
           policy.output_would_write ? "selected" :
           policy.output_would_review_hold ? "review_hold" : "rejected",
           policy.writer_override_candidate ? "writer_override" :
           policy.review_hold_promotion_candidate ? "hold_release" :
           policy.output_would_write ? "keep_primary" : "no_policy_write",
           "", policy_parent, policy.output_bpm,
           policy.candidate_local_exact_score, policy.candidate_support);
  out.push("final_output",
           withheld || final_bpm <= 0.0 ? "review_hold" : "selected",
           withheld || final_bpm <= 0.0 ? "final_abstained" : "final_numeric",
           "", {}, final_bpm, 0.0, 0.0);
  // Existing MIR writer invariant is the executable same-run witness.
  // The identity of supplemental TopK/partial-bar candidates is NOT yet
  // serialized; explicitly distinguish this from an indexed witness.
  std::vector<std::string> witness_ids;
  if (!withheld && final_bpm > 0.0 && same_run_measured_witness) {
    for (std::size_t i = 0; i < fullboard.size(); ++i) {
      if (std::abs(fullboard[i].bpm - final_bpm) <= 0.001)
        witness_ids.push_back(policy_ids[i]);
    }
    for (std::size_t i = 0; i < collapsed.size(); ++i) {
      if (std::abs(collapsed[i].bpm - final_bpm) <= 0.001)
        witness_ids.push_back(cluster_ids[i]);
    }
    if (witness_ids.size() > 128) return false;
  }
  out.push("measured_witness",
           same_run_measured_witness && !withheld && final_bpm > 0.0
               ? "included" : "rejected",
           withheld || final_bpm <= 0.0 ? "not_accepted" :
               !witness_ids.empty() ? "indexed_native_witness" :
                                      "supplemental_witness_unindexed",
           "", witness_ids, final_bpm, 0.0, 0.0);
  out.push("write_guard", "write_blocked", "ci_dry_run",
           "", {}, 0.0, 0.0, 0.0);
  if (out.sequence != board.raw_candidates.size() + 2 * collapsed.size() +
                      fullboard.size() + selectorEvents.size() + 5 ||
      out.lines.size() > 2 * 1024 * 1024) return false;
  const std::wstring output = fixture + L".pipeline.jsonl";
  HANDLE h = CreateFileW(output.c_str(), GENERIC_WRITE, 0, nullptr,
                         CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  DWORD wrote = 0;
  const BOOL ok = WriteFile(h, out.lines.data(),
                            static_cast<DWORD>(out.lines.size()), &wrote, nullptr);
  CloseHandle(h);
  if (!ok || wrote != out.lines.size()) {
    DeleteFileW(output.c_str());
    return false;
  }
  return true;
}
}  // namespace smart_tempo::ci_pipeline_trace
