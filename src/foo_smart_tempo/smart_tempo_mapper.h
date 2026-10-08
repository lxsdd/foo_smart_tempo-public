#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace SmartTempoMapper {

struct MatchDebugInfo {
  std::string matched_rule;
  std::string matched_token;
  size_t specificity_words = 0;
  size_t specificity_norm_len = 0;
  size_t rule_index = 0;
  bool is_generic_match = false;
  size_t candidates_considered = 0;
};

struct RouteMatchInfo {
  double min_bpm = 0.0;
  double max_bpm = 0.0;
  double center_bpm = 0.0;
  double spread_bpm = 0.0;
  std::string resolved_value;
  MatchDebugInfo debug;
};

struct RulesSnapshot;
using RulesSnapshotPtr = std::shared_ptr<const RulesSnapshot>;

RulesSnapshotPtr create_rules_snapshot_from_string(
    const char* rules_text, const char* generic_anchor_tokens_text = nullptr);
size_t get_rule_count(const RulesSnapshotPtr& snapshot) noexcept;

std::vector<RouteMatchInfo> get_route_matches_for_tag_values_snapshot(
    const RulesSnapshotPtr& snapshot, const std::vector<std::string>& tag_values,
    size_t max_matches = 8);

} // namespace SmartTempoMapper

