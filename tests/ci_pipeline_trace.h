#pragma once
#ifndef SMART_TEMPO_CI_ORACLE_PROBE
#error "CI-only Smart Tempo pipeline observer must not be included in production"
#endif

// Public, disposable, synthetic-host tracing ONLY. No file names, audio,
// genres, tags, aliases as free text, or owner data are serialized.
// This is an observational pool-lineage trace, NOT a claim that every
// collapsed candidate passed every selector's internal eligibility gate.
#include <Windows.h>
#include <Psapi.h>
#pragma comment(lib, "Psapi.lib")
#include <algorithm>
#include <chrono>
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

// Only actual decoded source/analysis frames, never audio samples or paths.
// Captured in the CI-only analyzer loop after resampler flush.
struct WindowObservation {
  std::size_t pass_index = 0;
  std::size_t pass_total = 0;
  double start_sec = 0.0;
  std::size_t requested_frames = 0;
  std::size_t decoded_frames = 0;
  std::size_t analysis_frames = 0;
  unsigned source_rate_hz = 0;
  unsigned analysis_rate_hz = 0;
};

// Process-scoped measurements, not attributed per-thread CPU/RSS.
struct ProcessResourceObservation {
  bool valid = false;
  double wall_seconds = 0.0;
  double process_cpu_seconds = 0.0;
  std::size_t process_peak_rss_before = 0;
  std::size_t process_peak_rss_after = 0;
  std::size_t process_working_set_after = 0;
};
struct ProcessResourceStart {
  std::chrono::steady_clock::time_point started{};
  uint64_t process_cpu_ticks = 0;
  std::size_t peak_rss_bytes = 0;
  bool valid = false;
};
inline uint64_t process_cpu_filetime(const FILETIME& a, const FILETIME& b) noexcept {
  ULARGE_INTEGER x{}, y{};
  x.LowPart = a.dwLowDateTime; x.HighPart = a.dwHighDateTime;
  y.LowPart = b.dwLowDateTime; y.HighPart = b.dwHighDateTime;
  return x.QuadPart + y.QuadPart;
}
inline ProcessResourceStart begin_process_resource_observation() noexcept {
  ProcessResourceStart out;
  out.started = std::chrono::steady_clock::now();
  FILETIME created{}, exited{}, kernel{}, user{};
  PROCESS_MEMORY_COUNTERS counters{};
  counters.cb = sizeof(counters);
  if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user) ||
      !GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)))
    return out;
  out.process_cpu_ticks = process_cpu_filetime(kernel, user);
  out.peak_rss_bytes = counters.PeakWorkingSetSize;
  out.valid = true;
  return out;
}
inline ProcessResourceObservation end_process_resource_observation(
    const ProcessResourceStart& start) noexcept {
  ProcessResourceObservation out;
  if (!start.valid) return out;
  const auto ended = std::chrono::steady_clock::now();
  FILETIME created{}, exited{}, kernel{}, user{};
  PROCESS_MEMORY_COUNTERS counters{};
  counters.cb = sizeof(counters);
  if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user) ||
      !GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)))
    return out;
  const uint64_t ticks = process_cpu_filetime(kernel, user);
  if (ticks < start.process_cpu_ticks ||
      counters.PeakWorkingSetSize < start.peak_rss_bytes ||
      counters.PeakWorkingSetSize < counters.WorkingSetSize)
    return out;
  out.wall_seconds = std::chrono::duration<double>(ended-start.started).count();
  out.process_cpu_seconds =
      static_cast<double>(ticks-start.process_cpu_ticks) / 10000000.0;
  out.process_peak_rss_before = start.peak_rss_bytes;
  out.process_peak_rss_after = counters.PeakWorkingSetSize;
  out.process_working_set_after = counters.WorkingSetSize;
  out.valid = std::isfinite(out.wall_seconds) && out.wall_seconds > 0.0 &&
              std::isfinite(out.process_cpu_seconds) &&
              out.process_cpu_seconds >= 0.0;
  return out;
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
  void push_resource(const ProcessResourceObservation& r) {
    push("resource_usage", "observed", "process_resource_snapshot",
         "", {}, 0.0, 0.0, 0.0);
    lines.insert(lines.size() - 3,
        ",\"wall_seconds\":" + safe_num(r.wall_seconds) +
        ",\"process_cpu_seconds\":" + safe_num(r.process_cpu_seconds) +
        ",\"process_peak_rss_before_bytes\":" + std::to_string(r.process_peak_rss_before) +
        ",\"process_peak_rss_after_bytes\":" + std::to_string(r.process_peak_rss_after) +
        ",\"process_working_set_after_bytes\":" + std::to_string(r.process_working_set_after));
  }
  void push_window(const WindowObservation& w) {
    push("decode_window", "observed", "actual_decoded_extent",
         "", {}, 0.0, 0.0, 0.0);
    // Extend the just-emitted numeric-only metrics object, before its two
    // closing braces and newline. The writer never serializes identifiers.
    lines.insert(lines.size() - 3,
        ",\"pass_index\":" + std::to_string(w.pass_index) +
        ",\"pass_total\":" + std::to_string(w.pass_total) +
        ",\"start_sec\":" + safe_num(w.start_sec) +
        ",\"requested_frames\":" + std::to_string(w.requested_frames) +
        ",\"decoded_frames\":" + std::to_string(w.decoded_frames) +
        ",\"analysis_frames\":" + std::to_string(w.analysis_frames) +
        ",\"source_rate_hz\":" + std::to_string(w.source_rate_hz) +
        ",\"analysis_rate_hz\":" + std::to_string(w.analysis_rate_hz));
  }
};

inline bool record(
    const mir_pipeline::MirCandidateBoard& board,
    const std::vector<WindowObservation>& windows,
    const std::vector<mir_pipeline::MirCandidate>& collapsed,
    const std::vector<mir_pipeline::MirPolicyMeasuredCandidate>& fullboard,
    std::span<const mir_pipeline::MirCiSelectorEvent> selectorEvents,
    mir_pipeline::MirCiPolicyPointerSource policyPointer,
    const MirPrimarySelection& primary,
    const MirPolicyDecision& policy,
    double final_bpm, bool withheld, bool same_run_measured_witness,
    const ProcessResourceObservation& resource,
    const smart_tempo::HodgkinsonMaterialRiskLocalExactProbe* risk,
    const smart_tempo::HodgkinsonPartialBarAggregateShadow* partial,
    std::span<const smart_tempo::HodgkinsonPartialBarTopKExactCandidate> topK) {
  const std::wstring fixture = permitted_fixture();
  const auto run_id = ci_run_id();
  if (fixture.empty() || run_id.empty() || !resource.valid ||
      !std::isfinite(resource.wall_seconds) || resource.wall_seconds <= 0.0 ||
      resource.wall_seconds > 7200.0 ||
      !std::isfinite(resource.process_cpu_seconds) || resource.process_cpu_seconds < 0.0 ||
      resource.process_cpu_seconds > resource.wall_seconds * 256.0 + 1.0 ||
      resource.process_peak_rss_before == 0 ||
      resource.process_peak_rss_after < resource.process_peak_rss_before ||
      resource.process_peak_rss_after < resource.process_working_set_after ||
      !finite_bpm(final_bpm) || final_bpm > 220.0 ||
      (!withheld && final_bpm > 0.0 && !same_run_measured_witness))
    return false;
  if (windows.empty() || windows.size() > 128 ||
      board.raw_candidates.size() > 4096 || collapsed.size() > 4096 ||
      selectorEvents.size() > 4096 || fullboard.size() > 4096 ||
      collapsed.size() != primary.candidate_count)
    return false;
  EventWriter out(run_id);
  std::vector<std::string> raw_ids, cluster_ids, policy_ids;
  raw_ids.reserve(board.raw_candidates.size());
  cluster_ids.reserve(collapsed.size());
  policy_ids.reserve(fullboard.size());
  // Fail closed on missing, reordered or physically impossible decoder extents.
  // Actual decoded frames may be shorter than requested at end-of-file.
  const std::size_t declaredPasses = windows.front().pass_total;
  for (std::size_t i = 0; i < windows.size(); ++i) {
    const auto& w = windows[i];
    if (w.pass_index != i || w.pass_total != declaredPasses ||
        declaredPasses == 0 || declaredPasses > 128 ||
        !std::isfinite(w.start_sec) || w.start_sec < 0.0 ||
        w.start_sec > 86400.0 ||
        w.source_rate_hz < 8000 || w.source_rate_hz > 384000 ||
        w.analysis_rate_hz < 8000 || w.analysis_rate_hz > 48000 ||
        w.analysis_rate_hz > w.source_rate_hz ||
        w.requested_frames == 0 ||
        w.requested_frames > static_cast<std::size_t>(w.source_rate_hz) * 600 ||
        w.decoded_frames == 0 || w.decoded_frames > w.requested_frames ||
        w.analysis_frames == 0 ||
        static_cast<double>(w.analysis_frames) / w.analysis_rate_hz >
            static_cast<double>(w.decoded_frames) / w.source_rate_hz + 0.10 ||
        (i > 0 && w.start_sec < windows[i - 1].start_sec))
      return false;
    out.push_window(w);
  }
  for (const auto& raw : board.raw_candidates) {
    if (!finite_bpm(raw.bpm) || raw.bpm == 0.0 ||
        !std::isfinite(raw.score_sum) || !std::isfinite(raw.synthetic_support))
      return false;
    const auto id = out.create_id();
    raw_ids.push_back(id);
    out.push("candidate_create", "observed", "raw_alias", id, {},
             raw.bpm, raw.score_sum, raw.synthetic_support);
  }
  // Reproduce the ACTUAL first-match collapse partition, not a many-to-many
  // near-BPM join. A raw BPM can lie within 0.001 of TWO cluster centers,
  // but collapse_mir_candidates assigns it only to the first matching one.
  std::vector<std::vector<std::size_t>> raw_cluster_sources(collapsed.size());
  for (std::size_t raw_index = 0; raw_index < board.raw_candidates.size();
       ++raw_index) {
    std::size_t assigned = collapsed.size();
    for (std::size_t cluster_index = 0; cluster_index < collapsed.size();
         ++cluster_index) {
      if (std::abs(board.raw_candidates[raw_index].bpm -
                   collapsed[cluster_index].bpm) < 0.001) {
        assigned = cluster_index;
        break;
      }
    }
    if (assigned == collapsed.size()) return false;
    raw_cluster_sources[assigned].push_back(raw_index);
  }
  for (std::size_t cluster_index = 0; cluster_index < collapsed.size();
       ++cluster_index) {
    const auto& cluster = collapsed[cluster_index];
    if (!finite_bpm(cluster.bpm) || cluster.bpm == 0.0 ||
        !std::isfinite(cluster.score_sum) || !std::isfinite(cluster.support) ||
        !std::isfinite(cluster.best_combined_score))
      return false;
    const auto& sources = raw_cluster_sources[cluster_index];
    if (sources.empty() || sources.size() > 128 ||
        board.raw_candidates[sources.front()].bpm != cluster.bpm)
      return false;
    std::vector<std::string> parents;
    parents.reserve(sources.size());
    for (const auto raw_index : sources)
      parents.push_back(raw_ids[raw_index]);
    const auto id = out.create_id();
    cluster_ids.push_back(id);
    out.push("candidate_transform", "observed", "cluster_from_raw", id,
             parents, cluster.bpm, cluster.score_sum, cluster.support);
    out.push("primary_eligibility", "observed", "pool_only_gate_unresolved",
             id, {}, cluster.bpm, cluster.best_combined_score, cluster.support);
  }
  // FullBoard is a filtered/reordered exact-BPM transform of collapsed
  // candidates. Each surviving source must occur at most once, even when
  // multiple transformed BPMs are numerically equal.
  std::vector<uint8_t> fullboard_source_seen(collapsed.size(), 0);
  for (const auto& candidate : fullboard) {
    if (!finite_bpm(candidate.bpm) || candidate.bpm == 0.0 ||
        !std::isfinite(candidate.local_exact_score) ||
        !std::isfinite(candidate.candidate.support))
      return false;
    std::vector<std::string> parents;
    for (std::size_t i = 0; i < collapsed.size(); ++i) {
      if (std::abs(collapsed[i].bpm - candidate.cluster_bpm) < 0.001) {
        if (fullboard_source_seen[i] != 0) return false;
        fullboard_source_seen[i] = 1;
        parents.push_back(cluster_ids[i]);
      }
    }
    if (parents.size() != 1) return false;
    const auto id = out.create_id();
    policy_ids.push_back(id);
    out.push("policy_eligibility", "included",
             parents.empty() ? "full_board_origin_unknown" : "full_board_member",
             id, parents, candidate.bpm,
             candidate.local_exact_score, candidate.candidate.support);
  }
  // Exact selected supplemental object from the analysis-owned input objects.
  std::vector<std::string> supplemental_id;
  double supplemental_bpm = 0.0;
  if (policyPointer.kind != mir_pipeline::MirCiSupplementalKind::none) {
    if (!policyPointer.observed || policyPointer.in_fullboard ||
        policyPointer.object == nullptr) return false;
    const char* source = nullptr;
    double score = 0.0, support = 0.0;
    if (policyPointer.kind == mir_pipeline::MirCiSupplementalKind::material_risk &&
        risk != nullptr && policyPointer.object == risk) {
      source = "material_risk_source";
      supplemental_bpm = risk->best_bpm;
      score = risk->best_score;
    } else if (policyPointer.kind ==
                   mir_pipeline::MirCiSupplementalKind::partial_bar_aggregate &&
               partial != nullptr && policyPointer.object == partial) {
      source = "partial_bar_aggregate_source";
      supplemental_bpm = partial->local_exact_bpm;
      score = partial->score_sum;
    } else if (policyPointer.kind ==
                   mir_pipeline::MirCiSupplementalKind::partial_bar_topk) {
      for (const auto& candidate : topK) {
        if (policyPointer.object != &candidate) continue;
        source = "partial_bar_topk_source";
        supplemental_bpm = candidate.local_exact_bpm;
        score = candidate.local_exact_score;
        support = candidate.score_ratio_to_top;
        break;
      }
    }
    if (!source || !finite_bpm(supplemental_bpm) ||
        supplemental_bpm <= 0.0 || !std::isfinite(score) ||
        !std::isfinite(support) ||
        std::abs(supplemental_bpm - policyPointer.bpm) > 0.001)
      return false;
    supplemental_id.push_back(out.create_id());
    out.push("supplemental_candidate", "observed", source,
             supplemental_id.front(), {}, supplemental_bpm, score, support);
  } else if (policyPointer.object != nullptr) return false;
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
                         std::strcmp(item.reason, "low_rank_scored") == 0 ||
                         std::strcmp(item.reason, "general_baseline_rank_scored") == 0 ||
                         std::strcmp(item.reason, "general_alt_rank_scored") == 0;
    const bool terminal = std::strncmp(item.reason, "terminal_", 9) == 0;
    const bool comparison = std::strstr(item.reason, "_compare_") != nullptr;
    out.push(terminal ? "primary_terminal" :
             ranking ? "primary_rank" :
             comparison ? "primary_compare" : "candidate_filter",
             terminal ? "selected" :
                 item.included ? "included" : "excluded",
             item.reason, ids[item.source_index], {}, item.bpm, item.score, 0.0);
    // Only the two native tie-based comparator families carry four ordered
    // tie fields. All fields are numeric and generated from executed C++.
    const bool tieExpected = comparison &&
        (std::strncmp(item.reason, "raw_compare_", 12) == 0 ||
         std::strncmp(item.reason, "general_baseline_compare_", 25) == 0);
    if (item.has_tie_metrics != tieExpected) return false;
    if (tieExpected) {
      for (const double value : item.tie_metrics)
        if (!std::isfinite(value)) return false;
      out.lines.insert(out.lines.size() - 3,
          ",\"tie_0\":" + safe_num(item.tie_metrics[0]) +
          ",\"tie_1\":" + safe_num(item.tie_metrics[1]) +
          ",\"tie_2\":" + safe_num(item.tie_metrics[2]) +
          ",\"tie_3\":" + safe_num(item.tie_metrics[3]));
    }
  }
  // Every collapsed candidate receives a purely observational disposition.
  // A temporary filter veto may be reversed later: never label it terminal.
  std::vector<uint8_t> primary_observation(collapsed.size(), 0);
  std::size_t final_source_index = collapsed.size();
  for (const auto& item : selectorEvents) {
    if (item.raw || item.source_index >= collapsed.size()) continue;
    if (std::strcmp(item.reason, "terminal_final_choice") == 0) {
      if (final_source_index != collapsed.size()) return false;
      final_source_index = item.source_index;
    } else if (std::strstr(item.reason, "_rank_scored") != nullptr) {
      primary_observation[item.source_index] = 2;
    } else if (primary_observation[item.source_index] < 1) {
      primary_observation[item.source_index] = 1;
    }
  }
  for (std::size_t i = 0; i < collapsed.size(); ++i) {
    const char* reason = i == final_source_index
        ? "selected_final_object"
        : primary_observation[i] == 2 ? "ranked_not_final"
        : primary_observation[i] == 1 ? "observed_not_ranked"
        : "no_cluster_observation";
    out.push("primary_disposition", "observed", reason, cluster_ids[i],
             {}, collapsed[i].bpm, 0.0, 0.0);
  }
  if (!std::isfinite(primary.selected_local_exact_score) ||
      !std::isfinite(primary.support) ||
      !std::isfinite(policy.candidate_local_exact_score) ||
      !std::isfinite(policy.candidate_support))
    return false;
  // Exact executed final-selector event, not a set of numerically equal BPMs.
  std::vector<std::string> primary_parent;
  for (const auto& item : selectorEvents) {
    if (std::strcmp(item.reason, "terminal_final_choice") == 0) {
      if (item.raw || item.source_index >= cluster_ids.size() ||
          !std::isfinite(item.bpm) || item.bpm <= 0.0)
        return false;
      if (!primary_parent.empty()) return false;
      if (std::abs(collapsed[item.source_index].bpm - item.bpm) >= 0.001 ||
          std::abs(primary.selected_bpm - item.bpm) >= 0.001)
        return false;
      primary_parent.push_back(cluster_ids[item.source_index]);
    }
  }
  if (primary.selected_bpm > 0.0 && primary_parent.size() != 1)
    return false;
  out.push("primary_select",
           primary.review_hold ? "review_hold" :
           primary.auto_candidate ? "selected" : "no_candidate",
           primary.review_hold ? "primary_review_hold" :
           primary.auto_candidate ? "primary_auto" : "primary_no_candidate",
           "", primary_parent, primary.selected_bpm,
           primary.selected_local_exact_score, primary.support);
  // BPM proximity to a FullBoard row is not native chosen-object identity.
  // Report matching BPM multiplicity as a falsifiable *comparison*, not an
  // assertion that Policy selected that candidate pointer.
  std::vector<std::string> exact_policy_matches;
  if (policy.output_bpm > 0.0) {
    for (std::size_t i = 0; i < fullboard.size(); ++i) {
      if (std::abs(fullboard[i].bpm - policy.output_bpm) <= 0.001)
        exact_policy_matches.push_back(policy_ids[i]);
    }
  }
  const char* policySource = policy.output_bpm <= 0.0 ? "policy_no_output" :
      exact_policy_matches.size() == 1 ? "policy_one_same_bpm" :
      exact_policy_matches.empty() ? "policy_no_fullboard_match" :
                                     "policy_multiple_same_bpm";
  if (exact_policy_matches.size() > 128) return false;
  out.push("policy_resolution", "observed", policySource, "",
           exact_policy_matches.size() == 1 ? exact_policy_matches :
                                             std::vector<std::string>{},
           policy.output_bpm,
           static_cast<double>(policy.writer_override_candidate),
           static_cast<double>(policy.review_hold_promotion_candidate));
  // Capture ACTUAL selected candidate reference when the native Policy
  // selector passed a FullBoard object, retaining explicit uncertainty for
  // supplemental TopK/partial-bar/micro-upgrade output changes.
  std::vector<std::string> selected_policy_pointer;
  const bool actualFullBoardObject =
      policyPointer.observed && policyPointer.in_fullboard &&
      policyPointer.index < fullboard.size() &&
      std::isfinite(policyPointer.bpm) && policy.output_bpm > 0.0 &&
      std::abs(policyPointer.bpm - policy.output_bpm) <= 0.001 &&
      std::abs(fullboard[policyPointer.index].bpm - policyPointer.bpm) <= 0.001;
  const bool actualSupplementalObject =
      !supplemental_id.empty() && !actualFullBoardObject &&
      policy.output_bpm > 0.0 &&
      std::abs(policy.output_bpm - supplemental_bpm) <= 0.001;
  if (actualFullBoardObject)
    selected_policy_pointer.push_back(policy_ids[policyPointer.index]);
  else if (actualSupplementalObject)
    selected_policy_pointer = supplemental_id;
  const char* pointerReason = policy.output_bpm <= 0.0
                                 ? "pointer_no_output"
                                 : actualFullBoardObject
                                       ? "pointer_native_fullboard_consistent"
                                       : actualSupplementalObject
                                             ? "pointer_native_supplemental_consistent"
                                       : policyPointer.observed
                                             ? "pointer_supplemental_or_changed"
                                             : "pointer_not_observed";
  out.push("policy_pointer", "observed", pointerReason, "",
           selected_policy_pointer, policy.output_bpm,
           static_cast<double>(policyPointer.observed),
           static_cast<double>(policyPointer.in_fullboard));
  out.push("policy_change",
           policy.output_would_write ? "selected" :
           policy.output_would_review_hold ? "review_hold" : "rejected",
           policy.writer_override_candidate ? "writer_override" :
           policy.review_hold_promotion_candidate ? "hold_release" :
           policy.output_would_write ? "keep_primary" : "no_policy_write",
           "", selected_policy_pointer, policy.output_bpm,
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
    // A selected Policy supplemental source is independently observed by
    // exact native object address. An equal-BPM value alone is insufficient.
    if (actualSupplementalObject &&
        std::abs(supplemental_bpm - final_bpm) <= 0.001 &&
        supplemental_id.size() == 1)
      witness_ids.push_back(supplemental_id.front());
    if (witness_ids.size() > 128) return false;
  }
  out.push("measured_witness",
           same_run_measured_witness && !withheld && final_bpm > 0.0
               ? "included" : "rejected",
           withheld || final_bpm <= 0.0 ? "not_accepted" :
               !witness_ids.empty() ? "indexed_native_witness" :
                                      "supplemental_witness_unindexed",
           "", witness_ids, final_bpm, 0.0, 0.0);
  out.push_resource(resource);
  out.push("write_guard", "write_blocked", "ci_dry_run",
           "", {}, 0.0, 0.0, 0.0);
  if (out.sequence != windows.size() + board.raw_candidates.size() + 2 * collapsed.size() +
                      fullboard.size() + selectorEvents.size() +
                      supplemental_id.size() + collapsed.size() + 8 ||
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
