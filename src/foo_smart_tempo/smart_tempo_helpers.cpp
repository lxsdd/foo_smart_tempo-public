#include "stdafx.h"

#include "smart_tempo_helpers.h"

#include "pfc/pfc.h"
#include "pfc/string-compare.h"
#include "pfc/string_base.h"
#include "pfc/unicode-normalize.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace {
using smart_tempo::helpers::canonicalize_import_token;
using smart_tempo::helpers::trim_copy;

enum class resolver_stage { stage_a = 0, stage_b = 1, stage_c = 2 };

struct resolver_candidate {
  size_t rule_index = 0;
  std::string canonical_matched_token;
  resolver_stage stage = resolver_stage::stage_a;
  size_t word_count = 1;
  size_t codepoint_length = 0;
  bool is_non_generic = false;
};

struct resolver_rule_state {
  size_t rule_index = 0;
  std::string original_pattern;
  bool tokenizable = false;
  std::vector<std::string> ordered_raw_tokens;
  std::vector<std::string> ordered_canonical_tokens;
  std::unordered_set<std::string> canonical_token_set;
  size_t generic_token_count = 0;
  size_t total_valid_token_count = 0;
  double generic_ratio = 0.0;
  bool catch_all_eligible = false;
};

struct resolver_token_state {
  std::string canonical_token;
  std::string representative_raw;
  size_t occurrence_count = 0;
  int source_rule = -1;
  bool source_rule_is_generic = false;
  bool source_rule_is_selected_catch_all = false;
  bool strict_stable_owner = false;
  std::vector<resolver_candidate> candidates;

  enum class outcome_kind {
    stable_deduplicated,
    stage_match_a,
    stage_match_b,
    stage_match_c,
    generic_fallback,
    unresolved_discarded
  };
  outcome_kind outcome = outcome_kind::stable_deduplicated;
  int target_rule = -1;
};

bool is_rule_placeholder_token(const std::string& token) { return token == "?"; }

size_t utf8_codepoint_length(std::string_view text) {
  size_t out = 0;
  const char* ptr = text.data();
  size_t left = text.size();
  while (left > 0 && *ptr != '\0') {
    unsigned c = 0;
    const size_t delta = pfc::utf8_decode_char(ptr, c, left);
    if (delta == 0 || delta > left) break;
    ptr += delta;
    left -= delta;
    ++out;
  }
  return out;
}

int casefold_compare(std::string_view lhs, std::string_view rhs) {
  const char* lhsPtr = lhs.empty() ? "" : lhs.data();
  const char* rhsPtr = rhs.empty() ? "" : rhs.data();
  const int folded = pfc::stringCompareCaseInsensitiveEx(
      pfc::string_part_ref(lhsPtr, lhs.size()),
      pfc::string_part_ref(rhsPtr, rhs.size()));
  if (folded != 0) return folded;

  // Tie-break with exact byte order to keep deterministic raw/codepoint ordering.
  const size_t lhsSize = lhs.size();
  const size_t rhsSize = rhs.size();
  const size_t shared = (std::min)(lhsSize, rhsSize);
  for (size_t i = 0; i < shared; ++i) {
    const unsigned char lc = static_cast<unsigned char>(lhsPtr[i]);
    const unsigned char rc = static_cast<unsigned char>(rhsPtr[i]);
    if (lc < rc) return -1;
    if (lc > rc) return 1;
  }
  if (lhsSize < rhsSize) return -1;
  if (lhsSize > rhsSize) return 1;
  return 0;
}

bool has_regex_meta(std::string_view text) {
  for (const char ch : text) {
    switch (ch) {
      case '^':
      case '$':
      case '(':
      case ')':
      case '[':
      case ']':
      case '{':
      case '}':
      case '+':
      case '?':
      case '*':
      case '|':
      case '\\':
        return true;
      default:
        break;
    }
  }
  return false;
}

std::string unescape_regex_literal(std::string_view input) {
  std::string out;
  out.reserve(input.size());
  bool escape = false;
  for (const char ch : input) {
    if (escape) {
      out.push_back(ch);
      escape = false;
      continue;
    }
    if (ch == '\\') {
      escape = true;
      continue;
    }
    out.push_back(ch);
  }
  if (escape) out.push_back('\\');
  return out;
}

std::string sanitize_rule_term(std::string_view term) {
  std::string out = trim_copy(term);
  while (!out.empty() && out.front() == '*') out.erase(out.begin());
  while (!out.empty() && out.back() == '*') out.pop_back();
  return trim_copy(out);
}

std::vector<std::string> split_multi_delim(std::string_view input,
                                           const std::unordered_set<char>& delims) {
  std::vector<std::string> out;
  std::string current;
  current.reserve(input.size());
  for (const char ch : input) {
    if (delims.find(ch) != delims.end()) {
      const std::string token = trim_copy(current);
      if (!token.empty()) out.push_back(token);
      current.clear();
    } else {
      current.push_back(ch);
    }
  }
  const std::string token = trim_copy(current);
  if (!token.empty()) out.push_back(token);
  return out;
}

struct parsed_rule_pattern {
  bool tokenizable = false;
  std::vector<std::string> terms;
};

parsed_rule_pattern parse_rule_pattern_terms(std::string_view pattern_text) {
  parsed_rule_pattern out;
  const std::string trimmed = trim_copy(pattern_text);
  if (trimmed.empty()) {
    out.tokenizable = true;
    return out;
  }

  constexpr const char* kPrefix = ".*(";
  constexpr const char* kSuffix = ").*";
  if (trimmed.rfind(kPrefix, 0) == 0 && trimmed.size() > 6 &&
      trimmed.compare(trimmed.size() - 3, 3, kSuffix) == 0) {
    const std::string body = trimmed.substr(3, trimmed.size() - 6);
    std::string token;
    std::istringstream parser(body);
    out.tokenizable = true;
    while (std::getline(parser, token, '|')) {
      const std::string unescaped = unescape_regex_literal(token);
      const std::string cleaned = sanitize_rule_term(unescaped);
      if (!cleaned.empty()) out.terms.push_back(cleaned);
    }
    return out;
  }

  if (trimmed.find(';') != std::string::npos || trimmed.find(',') != std::string::npos) {
    out.tokenizable = true;
    const auto terms = split_multi_delim(trimmed, std::unordered_set<char>{';', ','});
    for (const auto& t : terms) {
      const std::string cleaned = sanitize_rule_term(t);
      if (!cleaned.empty()) out.terms.push_back(cleaned);
    }
    return out;
  }

  if (has_regex_meta(trimmed)) {
    out.tokenizable = false;
    return out;
  }

  out.tokenizable = true;
  const std::string cleaned = sanitize_rule_term(trimmed);
  if (!cleaned.empty()) out.terms.push_back(cleaned);
  return out;
}

std::string build_pattern_from_terms(const std::vector<std::string>& terms) {
  std::string out;
  for (size_t i = 0; i < terms.size(); ++i) {
    if (i != 0) out += "; ";
    out += terms[i];
  }
  return out;
}

std::vector<std::string> split_stage_words_normalized(std::string_view raw) {
  std::vector<std::string> out;
  std::string current;
  current.reserve(raw.size());
  for (const char ch : raw) {
    if (ch == ' ' || ch == '-' || ch == '_') {
      const std::string token = canonicalize_import_token(current);
      if (!token.empty()) out.push_back(token);
      current.clear();
    } else {
      current.push_back(ch);
    }
  }
  const std::string token = canonicalize_import_token(current);
  if (!token.empty()) out.push_back(token);
  return out;
}

int stage_priority(resolver_stage stage) {
  switch (stage) {
    case resolver_stage::stage_a:
      return 3;
    case resolver_stage::stage_b:
      return 2;
    case resolver_stage::stage_c:
      return 1;
  }
  return 0;
}

bool candidate_better(const resolver_candidate& lhs, const resolver_candidate& rhs) {
  if (lhs.is_non_generic != rhs.is_non_generic) return lhs.is_non_generic && !rhs.is_non_generic;
  const int lp = stage_priority(lhs.stage);
  const int rp = stage_priority(rhs.stage);
  if (lp != rp) return lp > rp;
  if (lhs.word_count != rhs.word_count) return lhs.word_count > rhs.word_count;
  if (lhs.codepoint_length != rhs.codepoint_length) {
    return lhs.codepoint_length > rhs.codepoint_length;
  }
  return lhs.rule_index < rhs.rule_index;
}

void add_stage_a_candidates(const resolver_token_state& token,
                            const std::vector<resolver_rule_state>& rules,
                            const std::unordered_set<std::string>& anchorSet,
                            int selectedCatchAll,
                            std::vector<resolver_candidate>& out) {
  const size_t stage_word_count = [&]() {
    const auto parts = split_stage_words_normalized(token.representative_raw);
    return parts.empty() ? size_t{1} : parts.size();
  }();
  const size_t len = utf8_codepoint_length(token.canonical_token);
  for (const auto& rule : rules) {
    if (rule.canonical_token_set.find(token.canonical_token) == rule.canonical_token_set.end()) {
      continue;
    }
    resolver_candidate c;
    c.rule_index = rule.rule_index;
    c.canonical_matched_token = token.canonical_token;
    c.stage = resolver_stage::stage_a;
    c.word_count = stage_word_count;
    c.codepoint_length = len;
    c.is_non_generic =
        (int)rule.rule_index != selectedCatchAll &&
        anchorSet.find(token.canonical_token) == anchorSet.end();
    out.push_back(std::move(c));
  }
}

void add_stage_b_candidates(const resolver_token_state& token,
                            const std::vector<resolver_rule_state>& rules,
                            const std::unordered_set<std::string>& anchorSet,
                            int selectedCatchAll,
                            std::vector<resolver_candidate>& out) {
  auto words = split_stage_words_normalized(token.representative_raw);
  if (words.size() < 2) return;
  if (words.size() > 4) words.resize(4);

  for (size_t n = 2; n <= words.size(); ++n) {
    for (size_t start = 0; start + n <= words.size(); ++start) {
      std::string ngram;
      for (size_t i = 0; i < n; ++i) {
        ngram += words[start + i];
      }
      if (ngram.empty()) continue;
      const size_t len = utf8_codepoint_length(ngram);
      for (const auto& rule : rules) {
        if (rule.canonical_token_set.find(ngram) == rule.canonical_token_set.end()) continue;
        resolver_candidate c;
        c.rule_index = rule.rule_index;
        c.canonical_matched_token = ngram;
        c.stage = resolver_stage::stage_b;
        c.word_count = n;
        c.codepoint_length = len;
        c.is_non_generic =
            (int)rule.rule_index != selectedCatchAll &&
            anchorSet.find(ngram) == anchorSet.end();
        out.push_back(std::move(c));
      }
    }
  }
}

void add_stage_c_candidates(const resolver_token_state& token,
                            const std::vector<resolver_rule_state>& rules,
                            const std::unordered_set<std::string>& anchorSet,
                            int selectedCatchAll,
                            std::vector<resolver_candidate>& out) {
  const auto words = split_stage_words_normalized(token.representative_raw);
  for (const auto& word : words) {
    const size_t len = utf8_codepoint_length(word);
    for (const auto& rule : rules) {
      if (rule.canonical_token_set.find(word) == rule.canonical_token_set.end()) continue;
      resolver_candidate c;
      c.rule_index = rule.rule_index;
      c.canonical_matched_token = word;
      c.stage = resolver_stage::stage_c;
      c.word_count = 1;
      c.codepoint_length = len;
      c.is_non_generic =
          (int)rule.rule_index != selectedCatchAll &&
          anchorSet.find(word) == anchorSet.end();
      out.push_back(std::move(c));
    }
  }
}

void dedupe_candidates(std::vector<resolver_candidate>& candidates) {
  std::unordered_set<std::string> seen;
  std::vector<resolver_candidate> deduped;
  deduped.reserve(candidates.size());
  for (auto& c : candidates) {
    std::string key;
    key.reserve(c.canonical_matched_token.size() + 32);
    key += std::to_string(c.rule_index);
    key.push_back('|');
    key += c.canonical_matched_token;
    key.push_back('|');
    key += std::to_string((int)c.stage);
    if (!seen.emplace(key).second) continue;
    deduped.push_back(std::move(c));
  }
  candidates.swap(deduped);
}

std::string resolve_added_term(const resolver_token_state& token) {
  const std::string trimmed = trim_copy(token.representative_raw);
  return trimmed.empty() ? token.canonical_token : trimmed;
}

}  // namespace

namespace smart_tempo::helpers {

std::string normalize_genre_token(std::string_view token) {
  const std::string trimmed = trim_copy(token);
  if (trimmed.empty()) return {};

  const pfc::string8 nfc = pfc::unicodeNormalizeC(trimmed.c_str());
  pfc::string8 lowered;
  pfc::stringToLowerHere(lowered, nfc.c_str());

  std::string out;
  out.reserve(lowered.length());
  for (const char ch : std::string_view(lowered.c_str(), lowered.length())) {
    switch (ch) {
      case ' ':
      case '\t':
      case '\r':
      case '\n':
      case '-':
      case '_':
      case '/':
      case '|':
      case '\\':
      case '.':
      case ',':
        break;
      default:
        out.push_back(ch);
        break;
    }
  }
  return out;
}

std::string canonicalize_import_token(std::string_view input) {
  return normalize_genre_token(input);
}

std::vector<std::string> split_import_tokens_library(std::string_view input) {
  // Library STYLE/GENRE fields often contain comma-separated multi-value lists.
  // Importing those combinations as one alias can accidentally attach broad
  // compound tags (e.g. "Hardcore, Jungle, Drum n Bass") to the wrong rule.
  return split_multi_delim(input, std::unordered_set<char>{';', ',', '/', '|'});
}

std::vector<std::string> normalize_anchor_tokens_csv(std::string_view input) {
  std::unordered_set<std::string> seen;
  std::vector<std::string> out;
  std::string current;
  current.reserve(input.size());

  auto flush = [&]() {
    const std::string token = canonicalize_import_token(current);
    current.clear();
    if (token.empty()) return;
    if (!seen.emplace(token).second) return;
    out.push_back(token);
  };

  for (const char ch : input) {
    if (ch == ',' || ch == ';') {
      flush();
    } else {
      current.push_back(ch);
    }
  }
  flush();

  std::sort(out.begin(), out.end(),
            [](const std::string& a, const std::string& b) { return casefold_compare(a, b) < 0; });
  return out;
}

std::string join_tokens_csv(const std::vector<std::string>& tokens) {
  std::string out;
  for (size_t i = 0; i < tokens.size(); ++i) {
    if (i != 0) out += ", ";
    out += tokens[i];
  }
  return out;
}

bool casefold_codepoint_less(std::string_view lhs, std::string_view rhs) {
  return casefold_compare(lhs, rhs) < 0;
}

bool is_historical_generic_catch_all(
    const std::vector<std::string>& normalized_tokens) noexcept {
  // v2.0.0 and earlier appended unresolved imported values to the factory
  // Dance/Electro/Club/Electronic/EDM row. Recognize only that stable factory
  // signature; merely containing two broad tokens must never reclassify a
  // legitimate custom rule such as Club/Dance/House.
  static constexpr std::string_view kFactorySignature[] = {
      "dance", "electro", "club", "electronic", "edm"};
  return std::all_of(
      std::begin(kFactorySignature), std::end(kFactorySignature),
      [&normalized_tokens](std::string_view required) {
        return std::find(normalized_tokens.begin(), normalized_tokens.end(),
                         required) != normalized_tokens.end();
      });
}

import_resolver_result resolve_import_genres(
    const std::vector<import_resolver_input_token>& input_tokens,
    const std::vector<std::string>& rule_patterns,
    std::string_view anchor_tokens_csv, const std::atomic<bool>* abort_flag) {
  import_resolver_result result;
  result.updated_rule_patterns = rule_patterns;
  const auto is_abort_requested = [abort_flag]() noexcept -> bool {
    return abort_flag != nullptr && abort_flag->load(std::memory_order_relaxed);
  };
  if (is_abort_requested()) return result;

  std::vector<resolver_rule_state> rules;
  rules.reserve(rule_patterns.size());
  for (size_t i = 0; i < rule_patterns.size(); ++i) {
    if (is_abort_requested()) return result;
    resolver_rule_state rule;
    rule.rule_index = i;
    rule.original_pattern = rule_patterns[i];

    const auto parsed = parse_rule_pattern_terms(rule_patterns[i]);
    rule.tokenizable = parsed.tokenizable;
    std::unordered_set<std::string> seenCanonical;
    for (const auto& term : parsed.terms) {
      const std::string cleanedRaw = trim_copy(term);
      const std::string canonical = canonicalize_import_token(cleanedRaw);
      if (canonical.empty()) continue;
      if (!seenCanonical.emplace(canonical).second) continue;
      rule.ordered_raw_tokens.push_back(cleanedRaw);
      rule.ordered_canonical_tokens.push_back(canonical);
      rule.canonical_token_set.emplace(canonical);
    }
    rules.push_back(std::move(rule));
  }

  const auto normalizedAnchors = normalize_anchor_tokens_csv(anchor_tokens_csv);
  result.anchor_token_count = normalizedAnchors.size();
  const std::unordered_set<std::string> anchorSet(normalizedAnchors.begin(), normalizedAnchors.end());

  for (auto& rule : rules) {
    size_t genericCount = 0;
    size_t totalValid = 0;
    for (const auto& token : rule.ordered_canonical_tokens) {
      if (token.empty() || is_rule_placeholder_token(token)) continue;
      ++totalValid;
      if (anchorSet.find(token) != anchorSet.end()) ++genericCount;
    }
    rule.generic_token_count = genericCount;
    rule.total_valid_token_count = totalValid;
    rule.generic_ratio = (totalValid == 0) ? 0.0 : (double)genericCount / (double)totalValid;
    const bool explicitlyGenericOnly =
        genericCount >= 2 && genericCount == totalValid;
    rule.catch_all_eligible =
        explicitlyGenericOnly ||
        is_historical_generic_catch_all(rule.ordered_canonical_tokens);
  }

  int selectedCatchAll = -1;
  for (const auto& rule : rules) {
    if (!rule.catch_all_eligible) continue;
    if (selectedCatchAll < 0) {
      selectedCatchAll = (int)rule.rule_index;
      continue;
    }
    const auto& cur = rules[(size_t)selectedCatchAll];
    // Imported fallback terms dilute the ratio over time. Prefer the rule that
    // still contains the largest number of explicit Generic routing tokens so
    // the catch-all cannot jump from Dance/Club/... to Beat/Beats on a later
    // import.
    if (rule.generic_token_count > cur.generic_token_count ||
        (rule.generic_token_count == cur.generic_token_count &&
         (rule.generic_ratio > cur.generic_ratio ||
          (rule.generic_ratio == cur.generic_ratio &&
           rule.rule_index < cur.rule_index)))) {
      selectedCatchAll = (int)rule.rule_index;
    }
  }
  result.selected_catch_all_rule_index = selectedCatchAll;

  std::map<std::string, resolver_token_state> tokenByCanonical;
  for (const auto& in : input_tokens) {
    if (is_abort_requested()) return result;
    if (in.occurrence_count == 0) continue;
    const std::string canonical = canonicalize_import_token(in.representative_raw);
    if (canonical.empty()) continue;
    const std::string rep = trim_copy(in.representative_raw);
    auto& item = tokenByCanonical[canonical];
    item.canonical_token = canonical;
    item.occurrence_count += in.occurrence_count;
    if (item.representative_raw.empty() || casefold_compare(rep, item.representative_raw) < 0) {
      item.representative_raw = rep;
    }
  }

  std::vector<resolver_token_state*> orderedTokens;
  orderedTokens.reserve(tokenByCanonical.size());
  for (auto& kv : tokenByCanonical) orderedTokens.push_back(&kv.second);
  std::sort(orderedTokens.begin(), orderedTokens.end(),
            [](const resolver_token_state* a, const resolver_token_state* b) {
              return casefold_compare(a->canonical_token, b->canonical_token) < 0;
            });

  result.telemetry.genres_analyzed = 0;
  for (const auto* token : orderedTokens) {
    if (is_abort_requested()) return result;
    result.telemetry.genres_analyzed += token->occurrence_count;
  }

  for (auto* token : orderedTokens) {
    if (is_abort_requested()) return result;
    std::vector<int> specificOwners;
    std::vector<int> genericOwners;
    for (const auto& rule : rules) {
      if (rule.canonical_token_set.find(token->canonical_token) == rule.canonical_token_set.end()) {
        continue;
      }
      const bool genericOwner =
          (int)rule.rule_index == selectedCatchAll ||
          anchorSet.find(token->canonical_token) != anchorSet.end();
      if (genericOwner) {
        genericOwners.push_back((int)rule.rule_index);
      } else {
        specificOwners.push_back((int)rule.rule_index);
      }
    }

    if (!specificOwners.empty()) {
      token->source_rule = *std::min_element(specificOwners.begin(), specificOwners.end());
      token->source_rule_is_generic = false;
    } else if (!genericOwners.empty()) {
      int owner = -1;
      if (selectedCatchAll >= 0 &&
          std::find(genericOwners.begin(), genericOwners.end(), selectedCatchAll) != genericOwners.end()) {
        owner = selectedCatchAll;
      } else {
        owner = *std::min_element(genericOwners.begin(), genericOwners.end());
      }
      token->source_rule = owner;
      token->source_rule_is_generic = true;
    }

    token->source_rule_is_selected_catch_all =
        (token->source_rule >= 0 && token->source_rule == selectedCatchAll);
    token->strict_stable_owner =
        (token->source_rule >= 0 && !token->source_rule_is_selected_catch_all);
  }

  for (auto* token : orderedTokens) {
    if (is_abort_requested()) return result;
    if (token->strict_stable_owner) {
      token->outcome = resolver_token_state::outcome_kind::stable_deduplicated;
      token->target_rule = token->source_rule;
      continue;
    }

    std::vector<resolver_candidate> candidates;
    add_stage_a_candidates(*token, rules, anchorSet, selectedCatchAll,
                           candidates);
    add_stage_b_candidates(*token, rules, anchorSet, selectedCatchAll,
                           candidates);
    add_stage_c_candidates(*token, rules, anchorSet, selectedCatchAll,
                           candidates);
    dedupe_candidates(candidates);
    token->candidates = candidates;

    auto pick_best = [&](bool requireNonGeneric, resolver_candidate& outBest) -> bool {
      bool found = false;
      for (const auto& c : token->candidates) {
        if (requireNonGeneric && !c.is_non_generic) continue;
        if (!found || candidate_better(c, outBest)) {
          outBest = c;
          found = true;
        }
      }
      return found;
    };

    resolver_candidate best{};
    const bool hasAny = pick_best(false, best);
    resolver_candidate bestNonGeneric{};
    const bool hasNonGeneric = pick_best(true, bestNonGeneric);

    if (token->source_rule_is_selected_catch_all) {
      if (hasNonGeneric) {
        token->target_rule = (int)bestNonGeneric.rule_index;
        switch (bestNonGeneric.stage) {
          case resolver_stage::stage_a:
            token->outcome = resolver_token_state::outcome_kind::stage_match_a;
            break;
          case resolver_stage::stage_b:
            token->outcome = resolver_token_state::outcome_kind::stage_match_b;
            break;
          case resolver_stage::stage_c:
            token->outcome = resolver_token_state::outcome_kind::stage_match_c;
            break;
        }
      } else {
        token->target_rule = token->source_rule;
        token->outcome = resolver_token_state::outcome_kind::stable_deduplicated;
      }
      continue;
    }

    if (hasNonGeneric) {
      token->target_rule = (int)bestNonGeneric.rule_index;
      switch (bestNonGeneric.stage) {
        case resolver_stage::stage_a:
          token->outcome = resolver_token_state::outcome_kind::stage_match_a;
          break;
        case resolver_stage::stage_b:
          token->outcome = resolver_token_state::outcome_kind::stage_match_b;
          break;
        case resolver_stage::stage_c:
          token->outcome = resolver_token_state::outcome_kind::stage_match_c;
          break;
      }
      continue;
    }

    const bool onlyGenericCandidates = hasAny && !hasNonGeneric;
    const bool fallbackAllowed = (selectedCatchAll >= 0);
    if ((token->source_rule < 0) && (token->candidates.empty() || onlyGenericCandidates) && fallbackAllowed) {
      token->target_rule = selectedCatchAll;
      token->outcome = resolver_token_state::outcome_kind::generic_fallback;
    } else {
      token->target_rule = -1;
      token->outcome = resolver_token_state::outcome_kind::unresolved_discarded;
    }
  }

  std::vector<resolver_rule_state> mutatedRules = rules;
  for (const auto* token : orderedTokens) {
    if (is_abort_requested()) return result;
    const auto outcome = token->outcome;
    const bool isStage = outcome == resolver_token_state::outcome_kind::stage_match_a ||
                         outcome == resolver_token_state::outcome_kind::stage_match_b ||
                         outcome == resolver_token_state::outcome_kind::stage_match_c;
    const bool isFallback = outcome == resolver_token_state::outcome_kind::generic_fallback;

    if (!isStage && !isFallback) continue;
    if (token->target_rule < 0 || (size_t)token->target_rule >= mutatedRules.size()) {
      continue;
    }

    auto& target = mutatedRules[(size_t)token->target_rule];
    const std::string canonical = token->canonical_token;
    if (target.canonical_token_set.find(canonical) == target.canonical_token_set.end()) {
      target.canonical_token_set.emplace(canonical);
      target.ordered_canonical_tokens.push_back(canonical);
      target.ordered_raw_tokens.push_back(resolve_added_term(*token));
    }

    if (token->source_rule_is_selected_catch_all && isStage && token->source_rule >= 0 &&
        (size_t)token->source_rule < mutatedRules.size() && token->source_rule != token->target_rule) {
      auto& source = mutatedRules[(size_t)token->source_rule];
      source.canonical_token_set.erase(canonical);

      std::vector<std::string> newRaw;
      std::vector<std::string> newCanonical;
      for (size_t i = 0; i < source.ordered_raw_tokens.size(); ++i) {
        const std::string thisCanonical = (i < source.ordered_canonical_tokens.size())
                                              ? source.ordered_canonical_tokens[i]
                                              : canonicalize_import_token(source.ordered_raw_tokens[i]);
        if (thisCanonical == canonical) continue;
        newRaw.push_back(source.ordered_raw_tokens[i]);
        newCanonical.push_back(thisCanonical);
      }
      source.ordered_raw_tokens.swap(newRaw);
      source.ordered_canonical_tokens.swap(newCanonical);
    }
  }

  result.telemetry.deduplicated = 0;
  result.telemetry.stage_a_matches = 0;
  result.telemetry.stage_b_matches = 0;
  result.telemetry.stage_c_matches = 0;
  result.telemetry.initially_unresolved = 0;
  result.telemetry.generic_fallbacks = 0;
  result.telemetry.generic_tokens_added = 0;
  result.telemetry.unresolved_discarded.clear();

  for (const auto* token : orderedTokens) {
    if (is_abort_requested()) return result;
    const size_t n = token->occurrence_count;
    const bool isStage = token->outcome == resolver_token_state::outcome_kind::stage_match_a ||
                         token->outcome == resolver_token_state::outcome_kind::stage_match_b ||
                         token->outcome == resolver_token_state::outcome_kind::stage_match_c;
    const bool isStable = token->outcome == resolver_token_state::outcome_kind::stable_deduplicated;
    const bool isFallback = token->outcome == resolver_token_state::outcome_kind::generic_fallback;
    const bool isUnresolved = token->outcome == resolver_token_state::outcome_kind::unresolved_discarded;

    if (isStable) {
      result.telemetry.deduplicated += n;
    } else {
      result.telemetry.deduplicated += (n > 0) ? (n - 1) : 0;
    }

    if (isStage) {
      switch (token->outcome) {
        case resolver_token_state::outcome_kind::stage_match_a:
          ++result.telemetry.stage_a_matches;
          break;
        case resolver_token_state::outcome_kind::stage_match_b:
          ++result.telemetry.stage_b_matches;
          break;
        case resolver_token_state::outcome_kind::stage_match_c:
          ++result.telemetry.stage_c_matches;
          break;
        default:
          break;
      }
    }

    if (isFallback || isUnresolved) {
      ++result.telemetry.initially_unresolved;
    }
    if (isFallback) {
      ++result.telemetry.generic_fallbacks;
    }
    if (isUnresolved) {
      result.telemetry.unresolved_discarded.push_back(token->canonical_token);
    }
  }

  std::sort(result.telemetry.unresolved_discarded.begin(), result.telemetry.unresolved_discarded.end(),
            [](const std::string& a, const std::string& b) { return casefold_compare(a, b) < 0; });

  result.updated_rule_patterns.resize(mutatedRules.size());
  for (size_t i = 0; i < mutatedRules.size(); ++i) {
    if (is_abort_requested()) return result;
    if (!mutatedRules[i].tokenizable) {
      result.updated_rule_patterns[i] = mutatedRules[i].original_pattern;
      continue;
    }
    result.updated_rule_patterns[i] = build_pattern_from_terms(mutatedRules[i].ordered_raw_tokens);
  }

  // Normalize the selected catch-all into the same token-level contract used
  // by the runtime. A term that exists only in this rule is a routed no-prior
  // fallback, not a calibrated 125 BPM genre. Returning it to Preferences
  // makes that status explicit and persistent. Terms that also have a
  // specific owner remain specific and are deliberately not added.
  if (selectedCatchAll >= 0 &&
      (size_t)selectedCatchAll < mutatedRules.size()) {
    const auto& catchAll = mutatedRules[(size_t)selectedCatchAll];
    for (size_t i = 0; i < catchAll.ordered_canonical_tokens.size(); ++i) {
      const std::string& canonical = catchAll.ordered_canonical_tokens[i];
      if (canonical.empty() || is_rule_placeholder_token(canonical) ||
          anchorSet.find(canonical) != anchorSet.end()) {
        continue;
      }

      bool hasSpecificOwner = false;
      for (const auto& rule : mutatedRules) {
        if ((int)rule.rule_index == selectedCatchAll) continue;
        if (rule.canonical_token_set.find(canonical) !=
            rule.canonical_token_set.end()) {
          hasSpecificOwner = true;
          break;
        }
      }
      if (hasSpecificOwner) continue;

      const std::string display =
          i < catchAll.ordered_raw_tokens.size()
              ? trim_copy(catchAll.ordered_raw_tokens[i])
              : canonical;
      if (!display.empty()) result.generic_tokens_to_add.push_back(display);
    }
  }
  result.telemetry.generic_tokens_added =
      result.generic_tokens_to_add.size();

  return result;
}

}  // namespace smart_tempo::helpers
