#pragma once
#ifndef SMART_TEMPO_CI_ORACLE_PROBE
#error "Manual provenance is restricted to the disposable CI observer"
#endif

// Included inside smart_tempo::ci_pipeline_trace after EventWriter.
// Actual native manual-export inputs and output are read-only. The exported
// source labels are checked against a closed enum and NEVER serialized.
inline bool record_manual_export(
    EventWriter& out,
    std::span<const mir_pipeline::MirPolicyMeasuredCandidate> fullboard,
    const std::vector<std::string>& policy_ids,
    const MirPrimarySelection& primary,
    const std::vector<std::string>& primary_parent,
    const smart_tempo::HodgkinsonMaterialRiskLocalExactProbe* risk,
    const smart_tempo::HodgkinsonPartialBarAggregateShadow* partial,
    std::span<const smart_tempo::HodgkinsonPartialBarTopKExactCandidate> topK,
    std::span<const IBpmAnalyzer::MeasuredBpmCandidate> manual,
    bool manual_requested,
    std::size_t& source_count) {
  struct Input {
    double bpm, score, support, winner, exact;
    std::size_t rank;
    unsigned mask;
    const char* reason;
    std::vector<std::string> parents;
    std::string id;
  };
  std::vector<Input> inputs;
  const auto add = [&](double bpm, double score, double support,
                       double winner, double exact, std::size_t rank,
                       unsigned mask, const char* reason,
                       std::vector<std::string> parents) {
    // Exact production export rail; out-of-range sources are not exported.
    if (!(bpm >= 40.0 && bpm <= 220.0) || !std::isfinite(bpm))
      return true;
    if (!std::isfinite(score) || !std::isfinite(support) ||
        !std::isfinite(winner) || !std::isfinite(exact) ||
        inputs.size() >= 8192 || rank > 8192) return false;
    inputs.push_back({bpm, score, support, winner, exact,
                      rank, mask, reason, std::move(parents), {}});
    return true;
  };
  if (!manual_requested && !manual.empty()) return false;
  if (manual_requested) {
    for (std::size_t i = 0; i < fullboard.size(); ++i) {
      const auto& m = fullboard[i];
      if (!add(m.bpm, m.base_score, m.candidate.support,
               m.candidate.winner_support, m.local_exact_score,
               i + 1, 1, "manual_full_board", {policy_ids[i]}))
        return false;
    }
    if (primary.candidate) {
      const double bpm = primary.selected_local_exact_bpm > 0.0
                             ? primary.selected_local_exact_bpm
                             : primary.selected_bpm;
      if (primary_parent.size() != 1 ||
          !add(bpm, primary.measured_evidence_score, primary.support,
               primary.winner_support, primary.selected_local_exact_score,
               0, 2, "manual_primary_selector", primary_parent))
        return false;
    }
    if (risk != nullptr && risk->candidate &&
        !add(risk->best_bpm, risk->best_score, 0.0, 0.0,
             risk->best_score, 0, 4, "manual_material_risk", {}))
      return false;
    if (partial != nullptr && partial->candidate) {
      const double total = static_cast<double>(partial->total_segment_count);
      if (!add(partial->local_exact_bpm, partial->score_sum,
               total > 0.0 ? partial->segment_count / total : 0.0,
               total > 0.0 ? partial->top5_hits / total : 0.0,
               partial->local_exact_score, 0, 8,
               "manual_partial_aggregate", {}))
        return false;
    }
    for (const auto& row : topK) {
      const double total = static_cast<double>(row.total_segment_count);
      if (!add(row.local_exact_bpm, row.score_sum,
               total > 0.0 ? row.segment_count / total : 0.0,
               total > 0.0 ? row.top5_hits / total : 0.0,
               row.local_exact_score, 0, 16,
               "manual_partial_topk", {}))
        return false;
    }
  }
  if (manual.size() > 8192) return false;
  source_count = inputs.size();
  out.push("manual_state", "observed",
           manual_requested ? "manual_requested" : "manual_not_requested",
           "", {}, 0.0, static_cast<double>(inputs.size()),
           static_cast<double>(manual.size()));
  const auto append_metrics = [&](double winner, double exact,
                                  std::size_t rank, unsigned mask) {
    out.lines.insert(out.lines.size() - 3,
        ",\"winner_support\":" + safe_num(winner) +
        ",\"local_exact_score\":" + safe_num(exact) +
        ",\"evidence_rank\":" + std::to_string(rank) +
        ",\"origin_mask\":" + std::to_string(mask));
  };
  struct Group {
    double bpm;
    std::vector<std::string> ids;
    unsigned mask;
    bool used = false;
  };
  std::vector<Group> groups;
  for (auto& source : inputs) {
    source.id = out.create_id();
    out.push("manual_source", "observed", source.reason, source.id,
             source.parents, source.bpm, source.score, source.support);
    append_metrics(source.winner, source.exact, source.rank, source.mask);
    auto group = std::find_if(groups.begin(), groups.end(),
                              [&](const auto& g) {
                                return std::abs(g.bpm - source.bpm) <= 0.001;
                              });
    if (group == groups.end())
      groups.push_back({source.bpm, {source.id}, source.mask, false});
    else {
      if (group->ids.size() >= 128) return false;
      group->ids.push_back(source.id);
      group->mask |= source.mask;
    }
  }
  if (groups.size() != manual.size()) return false;
  for (const auto& candidate : manual) {
    if (!finite_bpm(candidate.bpm) || candidate.bpm < 40.0 ||
        candidate.bpm > 220.0 ||
        !std::isfinite(candidate.evidence_score) ||
        !std::isfinite(candidate.support) ||
        !std::isfinite(candidate.winner_support) ||
        !std::isfinite(candidate.local_exact_score) ||
        candidate.evidence_rank == 0 || candidate.evidence_rank > 8192)
      return false;
    auto group = std::find_if(groups.begin(), groups.end(),
                              [&](const auto& g) {
                                return !g.used && g.bpm == candidate.bpm;
                              });
    if (group == groups.end()) return false;
    // The native source-label set must contain only five fixed internal
    // tokens, never a track, path, genre or free-text identifier.
    const std::string labels(candidate.sources.get_ptr());
    unsigned mask = 0;
    std::size_t start = 0;
    while (start < labels.size()) {
      const auto end = labels.find('|', start);
      const std::string token = labels.substr(start, end - start);
      unsigned bit = 0;
      if (token == "full_board") bit = 1;
      else if (token == "primary_selector") bit = 2;
      else if (token == "material_risk_probe") bit = 4;
      else if (token == "partial_bar_aggregate") bit = 8;
      else if (token == "partial_bar_topk") bit = 16;
      if (bit == 0 || (mask & bit)) return false;
      mask |= bit;
      if (end == std::string::npos) break;
      start = end + 1;
    }
    if (mask != group->mask) return false;
    group->used = true;
    const auto id = out.create_id();
    out.push("manual_export", "observed", "manual_merged_export", id,
             group->ids, candidate.bpm, candidate.evidence_score,
             candidate.support);
    append_metrics(candidate.winner_support, candidate.local_exact_score,
                   candidate.evidence_rank, mask);
  }
  return std::all_of(groups.begin(), groups.end(),
                     [](const auto& g) { return g.used; });
}
