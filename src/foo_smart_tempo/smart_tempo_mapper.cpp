#include "stdafx.h"

#include "preferences.h"
#include "smart_tempo_mapper.h"
#include "smart_tempo_helpers.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace {
using smart_tempo::helpers::normalize_anchor_tokens_csv;
using smart_tempo::helpers::parse_rule_rhs_fields;
using smart_tempo::helpers::is_historical_generic_catch_all;
using smart_tempo::helpers::tokenize_genre_text;
using smart_tempo::helpers::trim_copy;

struct RoutingRuleToken {
  std::string original;
  std::string normalized;
  size_t token_word_count = 0;
  size_t normalized_codepoint_length = 0;
  bool is_generic = false;
};

struct RoutingRule {
  std::vector<RoutingRuleToken> tokens;
  std::string canonical_rule;
  double min_bpm = 0.0;
  double max_bpm = 0.0;
  double center_bpm = 0.0;
  double spread_bpm = 0.0;
  size_t rule_index = 0;  // 0-based order from config.
  size_t explicit_generic_token_count = 0;
  bool is_absolute_fallback = false;  // Token list contains '?'.
};

using RoutingRules = std::vector<RoutingRule>;
using NormalizedTokenSet = std::unordered_set<std::string>;

bool parse_rule_bounds(const std::string& rhs, double& out_min,
                       double& out_max, double& out_center,
                       double& out_spread) {
  smart_tempo::helpers::parsed_rule_rhs_fields parsed;
  if (!parse_rule_rhs_fields(rhs, parsed)) return false;

  out_min = parsed.min_bpm;
  out_max = parsed.max_bpm;
  out_center = parsed.center_bpm;
  out_spread = parsed.spread_bpm;
  return true;
}

std::string join_rule_tokens(const std::vector<RoutingRuleToken>& tokens) {
  std::string out;
  for (size_t i = 0; i < tokens.size(); ++i) {
    if (i != 0) out += "; ";
    out += tokens[i].original;
  }
  return out;
}

struct MatchCandidate {
  const RoutingRule* rule = nullptr;
  const RoutingRuleToken* rule_token = nullptr;
  const smart_tempo::helpers::genre_token_info* input_token = nullptr;
};

NormalizedTokenSet build_generic_anchor_set(const char* anchor_tokens_text) {
  NormalizedTokenSet out;
  if (anchor_tokens_text != nullptr && anchor_tokens_text[0] != '\0') {
    const auto normalizedAnchors = normalize_anchor_tokens_csv(anchor_tokens_text);
    for (const auto& token : normalizedAnchors) {
      if (!token.empty()) out.emplace(token);
    }
  }
  if (anchor_tokens_text == nullptr && out.empty()) {
    for (const char* token : {"dance", "electro", "club", "edm", "electronic", "beat"}) {
      out.emplace(token);
    }
  }
  return out;
}

bool is_generic_token(std::string_view normalized,
                      const NormalizedTokenSet& genericAnchors) {
  return genericAnchors.find(std::string(normalized)) != genericAnchors.end();
}

NormalizedTokenSet build_composite_whitelist(const RoutingRules& rules) {
  NormalizedTokenSet out;
  for (const auto& rule : rules) {
    for (const auto& token : rule.tokens) {
      if (token.token_word_count >= 2 && !token.normalized.empty()) {
        out.emplace(token.normalized);
      }
    }
  }
  return out;
}

std::vector<smart_tempo::helpers::genre_token_info> build_input_tokens(
    const std::vector<std::string>& tag_values,
    const NormalizedTokenSet* compositeWhitelist = nullptr) {
  std::vector<smart_tempo::helpers::genre_token_info> inputTokens;
  std::unordered_set<std::string> seenInputTokens;
  for (const auto& rawValue : tag_values) {
    const auto parsed = tokenize_genre_text(rawValue, true, false);
    for (const auto& token : parsed) {
      if (!seenInputTokens.emplace(token.normalized).second) continue;
      inputTokens.emplace_back(token);
    }
  }

  // Composite matching is deliberately rule-driven: synthetic tokens only win
  // if an active rule contains the same normalized combined token.
  constexpr size_t kMaxCompositeInputTokens = 12;
  const size_t baseCount = (std::min)(inputTokens.size(), kMaxCompositeInputTokens);
  std::vector<smart_tempo::helpers::genre_token_info> composites;
  for (size_t i = 0; i < baseCount; ++i) {
    for (size_t j = 0; j < baseCount; ++j) {
      if (j == i) continue;

      const std::string pairText =
          inputTokens[i].original + " " + inputTokens[j].original;
      const std::string pairNormalized =
          smart_tempo::helpers::normalize_genre_token(pairText);
      if (!pairNormalized.empty() &&
          (compositeWhitelist == nullptr ||
           compositeWhitelist->find(pairNormalized) != compositeWhitelist->end()) &&
          seenInputTokens.emplace(pairNormalized).second) {
        smart_tempo::helpers::genre_token_info composite;
        composite.original =
            inputTokens[i].original + " + " + inputTokens[j].original;
        composite.normalized = pairNormalized;
        composite.token_word_count =
            inputTokens[i].token_word_count + inputTokens[j].token_word_count;
        composite.normalized_codepoint_length =
            smart_tempo::helpers::utf8_codepoint_length(pairNormalized);
        composite.source_field_index =
            (std::min)(inputTokens[i].source_field_index,
                       inputTokens[j].source_field_index);
        composite.token_index_in_field =
            (std::min)(inputTokens[i].token_index_in_field,
                       inputTokens[j].token_index_in_field);
        composites.emplace_back(std::move(composite));
      }

      for (size_t k = 0; k < baseCount; ++k) {
        if (k == i || k == j) continue;
        const std::string tripleText = inputTokens[i].original + " " +
                                       inputTokens[j].original + " " +
                                       inputTokens[k].original;
        const std::string tripleNormalized =
            smart_tempo::helpers::normalize_genre_token(tripleText);
        if (tripleNormalized.empty() ||
            (compositeWhitelist != nullptr &&
             compositeWhitelist->find(tripleNormalized) == compositeWhitelist->end()) ||
            !seenInputTokens.emplace(tripleNormalized).second) {
          continue;
        }

        smart_tempo::helpers::genre_token_info composite;
        composite.original = inputTokens[i].original + " + " +
                             inputTokens[j].original + " + " +
                             inputTokens[k].original;
        composite.normalized = tripleNormalized;
        composite.token_word_count = inputTokens[i].token_word_count +
                                     inputTokens[j].token_word_count +
                                     inputTokens[k].token_word_count;
        composite.normalized_codepoint_length =
            smart_tempo::helpers::utf8_codepoint_length(tripleNormalized);
        composite.source_field_index =
            (std::min)((std::min)(inputTokens[i].source_field_index,
                                  inputTokens[j].source_field_index),
                       inputTokens[k].source_field_index);
        composite.token_index_in_field =
            (std::min)((std::min)(inputTokens[i].token_index_in_field,
                                  inputTokens[j].token_index_in_field),
                       inputTokens[k].token_index_in_field);
        composites.emplace_back(std::move(composite));
      }
    }
  }

  inputTokens.insert(inputTokens.end(), std::make_move_iterator(composites.begin()),
                     std::make_move_iterator(composites.end()));
  return inputTokens;
}

bool is_better_candidate(const MatchCandidate& lhs,
                         const MatchCandidate& rhs) noexcept {
  if (rhs.rule == nullptr) return true;
  if (lhs.rule == nullptr) return false;

  // A specific token must also beat a generic token inside the same rule.
  // Without this check, "Beat; 60s Beat" could resolve to generic "Beat"
  // merely because it appeared first in the formatted metadata.
  if (lhs.rule_token->is_generic != rhs.rule_token->is_generic) {
    return !lhs.rule_token->is_generic;
  }

  // User routing scripts can contain arbitrary fields, e.g. "%genre%",
  // "%style%" or "%genre%; %style%". After generic precedence, preserve
  // the user-provided order; specificity only breaks ties inside the same
  // field/token position.
  if (lhs.input_token->source_field_index != rhs.input_token->source_field_index) {
    return lhs.input_token->source_field_index < rhs.input_token->source_field_index;
  }
  if (lhs.input_token->token_index_in_field != rhs.input_token->token_index_in_field) {
    return lhs.input_token->token_index_in_field < rhs.input_token->token_index_in_field;
  }
  if (lhs.rule_token->token_word_count != rhs.rule_token->token_word_count) {
    return lhs.rule_token->token_word_count > rhs.rule_token->token_word_count;
  }
  if (lhs.rule_token->normalized_codepoint_length !=
      rhs.rule_token->normalized_codepoint_length) {
    return lhs.rule_token->normalized_codepoint_length >
           rhs.rule_token->normalized_codepoint_length;
  }
  if (lhs.rule->rule_index != rhs.rule->rule_index) {
    return lhs.rule->rule_index < rhs.rule->rule_index;
  }

  if (lhs.rule_token->normalized != rhs.rule_token->normalized) {
    return lhs.rule_token->normalized < rhs.rule_token->normalized;
  }
  return lhs.input_token->normalized < rhs.input_token->normalized;
}

SmartTempoMapper::RouteMatchInfo route_match_from_candidate(
    const MatchCandidate& candidate, bool genericMatch,
    size_t candidatesConsidered) {
  SmartTempoMapper::RouteMatchInfo out;
  out.min_bpm = candidate.rule->min_bpm;
  out.max_bpm = candidate.rule->max_bpm;
  out.center_bpm = genericMatch ? 0.0 : candidate.rule->center_bpm;
  out.spread_bpm = genericMatch ? 0.0 : candidate.rule->spread_bpm;
  out.resolved_value = candidate.input_token->original;
  out.debug.matched_rule = candidate.rule->canonical_rule;
  out.debug.matched_token = candidate.input_token->original;
  out.debug.specificity_words = candidate.rule_token->token_word_count;
  out.debug.specificity_norm_len =
      candidate.rule_token->normalized_codepoint_length;
  out.debug.rule_index = candidate.rule->rule_index;
  out.debug.is_generic_match = genericMatch;
  out.debug.candidates_considered = candidatesConsidered;
  return out;
}

SmartTempoMapper::RouteMatchInfo route_match_from_fallback_rule(
    const RoutingRule& rule, size_t candidatesConsidered) {
  SmartTempoMapper::RouteMatchInfo out;
  out.min_bpm = rule.min_bpm;
  out.max_bpm = rule.max_bpm;
  out.center_bpm = 0.0;
  out.spread_bpm = 0.0;
  out.resolved_value = "?";
  out.debug.matched_rule = rule.canonical_rule;
  out.debug.matched_token = "?";
  out.debug.is_generic_match = true;
  out.debug.rule_index = rule.rule_index;
  out.debug.candidates_considered = candidatesConsidered;
  return out;
}

RoutingRules parse_rules_from_text(const char* rules_text,
                                   const NormalizedTokenSet& genericAnchors) {
  RoutingRules parsedRules;
  if (rules_text == nullptr) return parsedRules;

  std::istringstream stream(rules_text);
  std::string line;
  while (std::getline(stream, line)) {
    const std::string trimmed = trim_copy(line);
    if (trimmed.empty()) continue;
    if (trimmed[0] == '#' || trimmed[0] == ';') continue;
    if (trimmed.size() >= 2 && trimmed[0] == '/' && trimmed[1] == '/') continue;

    const size_t eqPos = trimmed.find('=');
    if (eqPos == std::string::npos) continue;

    const std::string lhsRaw =
        trim_copy(std::string_view(trimmed).substr(0, eqPos));
    const std::string rhs =
        trim_copy(std::string_view(trimmed).substr(eqPos + 1));
    if (lhsRaw.empty() || rhs.empty()) continue;

    double minBpm = 0.0;
    double maxBpm = 0.0;
    double centerBpm = 0.0;
    double spreadBpm = 0.0;
    if (!parse_rule_bounds(rhs, minBpm, maxBpm, centerBpm, spreadBpm)) {
      continue;
    }

    RoutingRule rule;
    rule.min_bpm = minBpm;
    rule.max_bpm = maxBpm;
    rule.center_bpm = centerBpm;
    rule.spread_bpm = spreadBpm;
    rule.rule_index = parsedRules.size();

    const auto parsedTokens = tokenize_genre_text(lhsRaw, true, true);
    for (const auto& token : parsedTokens) {
      if (token.normalized == "?") {
        rule.is_absolute_fallback = true;
        continue;
      }
      RoutingRuleToken rt;
      rt.original = token.original;
      rt.normalized = token.normalized;
      rt.token_word_count = token.token_word_count;
      rt.normalized_codepoint_length = token.normalized_codepoint_length;
      rt.is_generic = is_generic_token(token.normalized, genericAnchors);
      if (rt.is_generic) ++rule.explicit_generic_token_count;
      rule.tokens.emplace_back(std::move(rt));
    }

    if (rule.tokens.empty() && !rule.is_absolute_fallback) continue;

    rule.canonical_rule = join_rule_tokens(rule.tokens);
    if (rule.canonical_rule.empty()) {
      rule.canonical_rule = rule.is_absolute_fallback ? "?" : lhsRaw;
    }
    parsedRules.emplace_back(std::move(rule));
  }

  // Older imports appended unresolved values to the stable factory catch-all.
  // Also accept a rule made exclusively from user-declared Generic tokens.
  // Never infer catch-all status from only two broad tokens in a mixed rule.
  size_t selectedCatchAll = parsedRules.size();
  for (size_t i = 0; i < parsedRules.size(); ++i) {
    const auto& candidate = parsedRules[i];
    std::vector<std::string> normalizedTokens;
    normalizedTokens.reserve(candidate.tokens.size());
    for (const auto& token : candidate.tokens) {
      normalizedTokens.push_back(token.normalized);
    }
    const bool explicitlyGenericOnly =
        candidate.explicit_generic_token_count >= 2 &&
        candidate.explicit_generic_token_count == candidate.tokens.size();
    if (candidate.tokens.empty() ||
        (!explicitlyGenericOnly &&
         !is_historical_generic_catch_all(normalizedTokens))) {
      continue;
    }
    if (selectedCatchAll == parsedRules.size()) {
      selectedCatchAll = i;
      continue;
    }

    const auto& current = parsedRules[selectedCatchAll];
    const double candidateRatio =
        static_cast<double>(candidate.explicit_generic_token_count) /
        static_cast<double>(candidate.tokens.size());
    const double currentRatio =
        static_cast<double>(current.explicit_generic_token_count) /
        static_cast<double>(current.tokens.size());
    if (candidate.explicit_generic_token_count >
            current.explicit_generic_token_count ||
        (candidate.explicit_generic_token_count ==
             current.explicit_generic_token_count &&
         (candidateRatio > currentRatio ||
          (candidateRatio == currentRatio &&
           candidate.rule_index < current.rule_index)))) {
      selectedCatchAll = i;
    }
  }
  if (selectedCatchAll < parsedRules.size()) {
    for (auto& token : parsedRules[selectedCatchAll].tokens) {
      token.is_generic = true;
    }
  }

  return parsedRules;
}

std::vector<SmartTempoMapper::RouteMatchInfo> collect_route_matches(
    const RoutingRules& rules, const std::vector<std::string>& tag_values,
    const NormalizedTokenSet* compositeWhitelist, size_t max_matches) {
  std::vector<SmartTempoMapper::RouteMatchInfo> out;
  if (rules.empty() || max_matches == 0) return out;

  const auto inputTokens = build_input_tokens(tag_values, compositeWhitelist);

  std::vector<MatchCandidate> bestByRule(rules.size());
  size_t candidatesConsidered = 0;
  const RoutingRule* absoluteFallbackRule = nullptr;
  for (const auto& rule : rules) {
    if (absoluteFallbackRule == nullptr && rule.is_absolute_fallback) {
      absoluteFallbackRule = &rule;
    }
    for (const auto& inputToken : inputTokens) {
      for (const auto& ruleToken : rule.tokens) {
        ++candidatesConsidered;
        if (inputToken.normalized != ruleToken.normalized) continue;
        if (rule.rule_index >= bestByRule.size()) continue;
        MatchCandidate candidate{&rule, &ruleToken, &inputToken};
        if (is_better_candidate(candidate, bestByRule[rule.rule_index])) {
          bestByRule[rule.rule_index] = candidate;
        }
      }
    }
  }

  std::vector<MatchCandidate> specificMatches;
  std::vector<MatchCandidate> genericMatches;
  for (const auto& candidate : bestByRule) {
    if (candidate.rule == nullptr || candidate.rule_token == nullptr ||
        candidate.input_token == nullptr) {
      continue;
    }
    if (candidate.rule_token->is_generic) {
      genericMatches.emplace_back(candidate);
    } else {
      specificMatches.emplace_back(candidate);
    }
  }

  const auto sortByUserOrder = [](const MatchCandidate& lhs,
                                  const MatchCandidate& rhs) noexcept {
    return is_better_candidate(lhs, rhs);
  };
  std::sort(specificMatches.begin(), specificMatches.end(), sortByUserOrder);
  std::sort(genericMatches.begin(), genericMatches.end(), sortByUserOrder);

  out.reserve((std::min)(max_matches,
                         specificMatches.size() + genericMatches.size()));
  for (const auto& candidate : specificMatches) {
    if (out.size() >= max_matches) break;
    out.emplace_back(
        route_match_from_candidate(candidate, false, candidatesConsidered));
  }
  for (const auto& candidate : genericMatches) {
    if (out.size() >= max_matches) break;
    out.emplace_back(
        route_match_from_candidate(candidate, true, candidatesConsidered));
  }

  if (out.empty() && absoluteFallbackRule != nullptr) {
    out.emplace_back(
        route_match_from_fallback_rule(*absoluteFallbackRule, candidatesConsidered));
  }
  return out;
}

}  // namespace

namespace SmartTempoMapper {

struct RulesSnapshot {
  std::shared_ptr<const RoutingRules> rules;
  std::shared_ptr<const NormalizedTokenSet> composite_whitelist;
};

RulesSnapshotPtr create_rules_snapshot_from_string(
    const char* rules_text, const char* generic_anchor_tokens_text) {
  const NormalizedTokenSet genericAnchors =
      build_generic_anchor_set(generic_anchor_tokens_text);
  RoutingRules parsedRules = parse_rules_from_text(rules_text, genericAnchors);
  NormalizedTokenSet compositeWhitelist = build_composite_whitelist(parsedRules);
  auto snapshot = std::make_shared<RulesSnapshot>();
  snapshot->rules =
      std::make_shared<const RoutingRules>(std::move(parsedRules));
  snapshot->composite_whitelist =
      std::make_shared<const NormalizedTokenSet>(std::move(compositeWhitelist));
  return snapshot;
}

size_t get_rule_count(const RulesSnapshotPtr& snapshot) noexcept {
  if (!snapshot || !snapshot->rules) return 0;
  return snapshot->rules->size();
}

std::vector<RouteMatchInfo> get_route_matches_for_tag_values_snapshot(
    const RulesSnapshotPtr& snapshot, const std::vector<std::string>& tag_values,
    size_t max_matches) {
  if (!snapshot || !snapshot->rules || snapshot->rules->empty()) return {};
  return collect_route_matches(*snapshot->rules, tag_values,
                               snapshot->composite_whitelist.get(), max_matches);
}

}  // namespace SmartTempoMapper
