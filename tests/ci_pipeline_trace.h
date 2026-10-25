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
#include <string>
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

inline bool finite_bpm(double bpm) { return std::isfinite(bpm) && bpm >= 0.0 && bpm <= 220.0; }
inline std::string safe_num(double value) {
  return std::to_string(std::isfinite(value) ? value : 0.0);
}

struct EventWriter {
  std::string lines;
  std::size_t sequence = 0;
  std::size_t next_id = 0;
  std::string create_id() { return "c" + std::to_string(next_id++); }
  void push(const char* stage, const char* outcome, const char* reason,
            const std::string& id, const std::vector<std::string>& parents,
            double bpm, double score, double support) {
    lines += "{\"schema\":\"smart_tempo_source_decision_trace_v1\",";
    lines += "\"analysis_id\":\"cafefeed01234567\",\"sequence\":" + std::to_string(sequence++);
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
    const MirPrimarySelection& primary,
    const MirPolicyDecision& policy,
    double final_bpm, bool withheld) {
  const std::wstring fixture = permitted_fixture();
  if (fixture.empty() || !finite_bpm(final_bpm)) return false;
  if (board.raw_candidates.size() > 4096 || collapsed.size() > 4096 ||
      fullboard.size() > 4096 || collapsed.size() != primary.candidate_count)
    return false;
  EventWriter out;
  std::vector<std::string> raw_ids, cluster_ids, policy_ids;
  raw_ids.reserve(board.raw_candidates.size());
  cluster_ids.reserve(collapsed.size());
  policy_ids.reserve(fullboard.size());
  for (const auto& raw : board.raw_candidates) {
    if (!finite_bpm(raw.bpm) || raw.bpm == 0.0) return false;
    const auto id = out.create_id();
    raw_ids.push_back(id);
    out.push("candidate_create", "observed", "raw_alias", id, {},
             raw.bpm, raw.score_sum, raw.synthetic_support);
  }
  for (const auto& cluster : collapsed) {
    if (!finite_bpm(cluster.bpm) || cluster.bpm == 0.0) return false;
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
    if (!finite_bpm(candidate.bpm) || candidate.bpm == 0.0) return false;
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
  out.push("write_guard", "write_blocked", "ci_dry_run",
           "", {}, 0.0, 0.0, 0.0);
  if (out.sequence != board.raw_candidates.size() + 2 * collapsed.size() +
                      fullboard.size() + 4 ||
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
