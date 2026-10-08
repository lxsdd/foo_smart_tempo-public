#pragma once

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstddef>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace smart_tempo::helpers {

inline std::string trim_copy(std::string_view input) {
  const auto is_space = [](unsigned char ch) {
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
  };

  size_t begin = 0;
  while (begin < input.size() && is_space((unsigned char)input[begin])) {
    ++begin;
  }

  size_t end = input.size();
  while (end > begin && is_space((unsigned char)input[end - 1])) {
    --end;
  }

  return std::string(input.substr(begin, end - begin));
}

inline std::string to_lower_ascii(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const char ch : text) {
    out.push_back(
        static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  }
  return out;
}

inline bool parse_double_strict(const std::string& text, double& out_value) {
  if (text.empty()) return false;
  char* end = nullptr;
  const double value = std::strtod(text.c_str(), &end);
  if (end == nullptr || *end != '\0') return false;
  if (!std::isfinite(value)) return false;
  out_value = value;
  return true;
}

inline bool cstr_contains(const char* text, const char* needle) noexcept {
  if (text == nullptr || needle == nullptr) return false;
  return std::strstr(text, needle) != nullptr;
}

struct parsed_rule_rhs_fields {
  std::vector<std::string> raw_parts;
  double min_bpm = 0.0;
  double max_bpm = 0.0;
  double center_bpm = 0.0;
  double spread_bpm = 0.0;
  bool center_schema = false;
};

inline bool parse_rule_rhs_fields(std::string_view rhs,
                                  parsed_rule_rhs_fields& out_fields) {
  out_fields = parsed_rule_rhs_fields{};

  std::string token;
  std::istringstream parser{std::string(rhs)};
  while (std::getline(parser, token, ',')) {
    out_fields.raw_parts.emplace_back(trim_copy(token));
  }
  if (out_fields.raw_parts.empty()) return false;

  constexpr double kCenterCompatibilityHalfWidthBpm = 15.0;
  const auto parse_named_value = [](const std::string& raw,
                                    std::string_view name,
                                    double& out) -> bool {
    const std::string lower = to_lower_ascii(raw);
    if (lower.rfind(name, 0) != 0) return false;
    std::string value = trim_copy(std::string_view(raw).substr(name.size()));
    if (!value.empty() && (value.front() == ':' || value.front() == '=')) {
      value = trim_copy(std::string_view(value).substr(1));
    }
    return parse_double_strict(value, out);
  };
  const auto apply_center_schema = [&](double centerValue,
                                       double spreadValue) -> bool {
    if (!(spreadValue > 0.0) || !(centerValue > spreadValue)) {
      return false;
    }
    out_fields.center_bpm = centerValue;
    out_fields.min_bpm = centerValue - spreadValue;
    out_fields.max_bpm = centerValue + spreadValue;
    out_fields.spread_bpm = spreadValue;
    out_fields.center_schema = true;
    return true;
  };

  std::string first = to_lower_ascii(out_fields.raw_parts[0]);
  if (first.rfind("center ", 0) == 0 || first.rfind("center:", 0) == 0) {
    double centerValue = 0.0;
    if (!parse_named_value(out_fields.raw_parts[0], "center", centerValue)) {
      return false;
    }
    double spreadValue = kCenterCompatibilityHalfWidthBpm;
    if (out_fields.raw_parts.size() >= 2 &&
        !parse_named_value(out_fields.raw_parts[1], "spread", spreadValue)) {
      return false;
    }
    if (out_fields.raw_parts.size() > 2) return false;
    return apply_center_schema(centerValue, spreadValue);
  }

  // Preferred center-only rule schema: Genres = 128.00. It keeps the runtime
  // fed with a real route center while synthesizing compatibility bounds only
  // for the unchanged parsed-rule data shape.
  if (out_fields.raw_parts.size() == 1) {
    double centerValue = 0.0;
    return parse_double_strict(out_fields.raw_parts[0], centerValue) &&
           apply_center_schema(centerValue, kCenterCompatibilityHalfWidthBpm);
  }

  if (out_fields.raw_parts.size() < 2) return false;

  double min_value = 0.0;
  double max_value = 0.0;
  if (!parse_double_strict(out_fields.raw_parts[0], min_value) ||
      !parse_double_strict(out_fields.raw_parts[1], max_value)) {
    return false;
  }
  if (!(min_value > 0.0) || !(max_value > min_value)) return false;

  out_fields.min_bpm = min_value;
  out_fields.max_bpm = max_value;
  out_fields.center_bpm = (min_value + max_value) * 0.5;
  out_fields.spread_bpm = (max_value - min_value) * 0.5;
  // Additional columns belong to pre-v2 detector/folding/SR schemas. Accept
  // them for deterministic import, but intentionally do not parse or retain
  // them in the Hodgkinson/MIR routing model.

  return true;
}

inline std::string normalize_routing_script_text(std::string_view input) {
  const std::string cleaned = trim_copy(input);
  if (cleaned.empty()) return std::string("%genre%");

  // Migrate only the exact former STYLE-first factory string; any real custom
  // script stays exactly user-controlled.
  if (cleaned == "$if(%style%,%style%[; %genre%],$if2(%genre%,?))") {
    return "$if(%genre%,%genre%[; %style%],$if2(%style%,?))";
  }
  return cleaned;
}

struct genre_token_info {
  std::string original;
  std::string normalized;
  size_t token_word_count = 0;
  size_t normalized_codepoint_length = 0;
  size_t source_field_index = 0;
  size_t token_index_in_field = 0;
};

inline bool is_genre_token_delimiter(char ch) noexcept {
  return ch == ';' || ch == ',' || ch == '/' || ch == '|';
}

inline std::string canonicalize_rule_pattern_source(std::string_view input) {
  std::string text = trim_copy(input);
  if (text.empty()) return {};

  constexpr std::string_view kPrefix = ".*(";
  constexpr std::string_view kSuffix = ").*";
  if (text.rfind(kPrefix.data(), 0) == 0 &&
      text.size() > (kPrefix.size() + kSuffix.size()) &&
      text.compare(text.size() - kSuffix.size(), kSuffix.size(),
                   kSuffix.data()) == 0) {
    text = text.substr(kPrefix.size(),
                       text.size() - (kPrefix.size() + kSuffix.size()));
    std::replace(text.begin(), text.end(), '|', ';');
  }
  return trim_copy(text);
}

inline size_t compute_token_word_count_before_normalization(
    std::string_view token) noexcept {
  const std::string trimmed = trim_copy(token);
  if (trimmed.empty()) return 0;
  if (trimmed == "?") return 1;

  size_t count = 0;
  bool inWord = false;
  for (const char ch : trimmed) {
    const bool sep = (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' ||
                      ch == '-' || ch == '_');
    if (sep) {
      inWord = false;
      continue;
    }
    if (!inWord) {
      ++count;
      inWord = true;
    }
  }
  return (count == 0) ? 1 : count;
}

inline size_t utf8_codepoint_length(std::string_view text) noexcept {
  size_t out = 0;
  for (const unsigned char ch : text) {
    if ((ch & 0xC0u) != 0x80u) ++out;
  }
  return out;
}

std::string normalize_genre_token(std::string_view token);
bool is_historical_generic_catch_all(
    const std::vector<std::string>& normalized_tokens) noexcept;

inline std::vector<genre_token_info> tokenize_genre_text(
    std::string_view input, bool dedupe_normalized = true,
    bool treat_as_rule_pattern = false) {
  std::vector<genre_token_info> out;
  std::unordered_map<std::string, size_t> seen;

  const std::string source =
      treat_as_rule_pattern ? canonicalize_rule_pattern_source(input)
                            : trim_copy(input);
  if (source.empty()) return out;

  std::string current;
  current.reserve(source.size());
  size_t sourceFieldIndex = 0;
  size_t tokenIndexInField = 0;
  auto flush_token = [&]() {
    std::string token = trim_copy(current);
    current.clear();

    while (!token.empty() && token.front() == '*') token.erase(token.begin());
    while (!token.empty() && token.back() == '*') token.pop_back();
    token = trim_copy(token);
    if (token.empty()) return;

    const std::string normalized = normalize_genre_token(token);
    if (normalized.empty()) return;

    genre_token_info parsed;
    parsed.original = token;
    parsed.normalized = normalized;
    parsed.token_word_count =
        compute_token_word_count_before_normalization(token);
    parsed.normalized_codepoint_length = utf8_codepoint_length(normalized);
    parsed.source_field_index = sourceFieldIndex;
    parsed.token_index_in_field = tokenIndexInField;
    if (dedupe_normalized) {
      const auto seenIt = seen.find(normalized);
      if (seenIt != seen.end()) return;
      seen.emplace(normalized, out.size());
    }
    out.emplace_back(std::move(parsed));
  };

  for (const char ch : source) {
    if (is_genre_token_delimiter(ch)) {
      flush_token();
      if (ch == ';') {
        ++sourceFieldIndex;
        tokenIndexInField = 0;
      } else {
        ++tokenIndexInField;
      }
      continue;
    }
    current.push_back(ch);
  }
  flush_token();
  return out;
}

inline std::vector<std::string> split_genre_tokens(std::string_view input) {
  const auto parsed = tokenize_genre_text(input, true, false);
  std::vector<std::string> out;
  out.reserve(parsed.size());
  for (const auto& token : parsed) out.push_back(token.original);
  return out;
}

struct import_resolver_input_token {
  std::string representative_raw;
  size_t occurrence_count = 1;
};

struct import_resolver_telemetry {
  size_t genres_analyzed = 0;
  size_t deduplicated = 0;
  size_t stage_a_matches = 0;
  size_t stage_b_matches = 0;
  size_t stage_c_matches = 0;
  size_t initially_unresolved = 0;
  size_t generic_fallbacks = 0;
  size_t generic_tokens_added = 0;
  std::vector<std::string> unresolved_discarded;
};

struct import_resolver_result {
  std::vector<std::string> updated_rule_patterns;
  // Tokens kept only in the generic catch-all must also be persisted in the
  // runtime Generic routing list so they cannot inherit the catch-all center.
  std::vector<std::string> generic_tokens_to_add;
  import_resolver_telemetry telemetry;
  int selected_catch_all_rule_index = -1;
  size_t anchor_token_count = 0;
};

std::string canonicalize_import_token(std::string_view input);
std::vector<std::string> split_import_tokens_library(std::string_view input);
std::vector<std::string> normalize_anchor_tokens_csv(std::string_view input);
std::string join_tokens_csv(const std::vector<std::string>& tokens);
bool casefold_codepoint_less(std::string_view lhs, std::string_view rhs);

import_resolver_result resolve_import_genres(
    const std::vector<import_resolver_input_token>& input_tokens,
    const std::vector<std::string>& rule_patterns,
    std::string_view anchor_tokens_csv,
    const std::atomic<bool>* abort_flag = nullptr);

}  // namespace smart_tempo::helpers
