#pragma once

// clang-format off
#include "foobar2000/SDK/foobar2000.h"
#include "foobar2000/helpers/helpers.h"
// clang-format on

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

/// Pure interface for BPM analysis engines (Strategy Pattern).
/// The active implementation is ModernBpmAnalyzer.
/// Instances are created per-track in worker threads via BpmAnalyzerFactory.
class IBpmAnalyzer {
public:
    struct MeasuredBpmCandidate {
        double       bpm = 0.0;
        double       evidence_score = 0.0;
        double       support = 0.0;
        double       winner_support = 0.0;
        double       local_exact_score = 0.0;
        size_t       evidence_rank = 0;
        pfc::string8 alias_classes;
        pfc::string8 sources;
    };

    struct RoutingLogContext {
        const char* source_genres = nullptr;
        const char* normalized_genres = nullptr;
        const char* matched_rule = nullptr;
        const char* matched_token = nullptr;
        size_t      rule_index = 0;
        size_t      specificity_words = 0;
        size_t      specificity_norm_len = 0;
        bool        route_matched = false;
        bool        is_generic_match = false;
        size_t      candidates_considered = 0;
        double      primary_min_bpm = 0.0;
        double      primary_max_bpm = 0.0;
        double      primary_center_bpm = 0.0;
        double      primary_spread_bpm = 0.0;
    };

    // Owns the routing strings needed to reproduce a later no-write candidate
    // inspection with the exact context used by the original analysis.
    struct RoutingContextSnapshot {
        pfc::string8 source_genres;
        pfc::string8 normalized_genres;
        pfc::string8 matched_rule;
        pfc::string8 matched_token;
        size_t       rule_index = 0;
        size_t       specificity_words = 0;
        size_t       specificity_norm_len = 0;
        bool         route_matched = false;
        bool         is_generic_match = false;
        size_t       candidates_considered = 0;
        double       primary_min_bpm = 0.0;
        double       primary_max_bpm = 0.0;
        double       primary_center_bpm = 0.0;
        double       primary_spread_bpm = 0.0;

        void assign(const RoutingLogContext& context) {
            source_genres = context.source_genres != nullptr
                                ? context.source_genres
                                : "";
            normalized_genres = context.normalized_genres != nullptr
                                    ? context.normalized_genres
                                    : "";
            matched_rule = context.matched_rule != nullptr
                               ? context.matched_rule
                               : "";
            matched_token = context.matched_token != nullptr
                                ? context.matched_token
                                : "";
            rule_index = context.rule_index;
            specificity_words = context.specificity_words;
            specificity_norm_len = context.specificity_norm_len;
            route_matched = context.route_matched;
            is_generic_match = context.is_generic_match;
            candidates_considered = context.candidates_considered;
            primary_min_bpm = context.primary_min_bpm;
            primary_max_bpm = context.primary_max_bpm;
            primary_center_bpm = context.primary_center_bpm;
            primary_spread_bpm = context.primary_spread_bpm;
        }

        [[nodiscard]] RoutingLogContext view() const noexcept {
            RoutingLogContext context;
            context.source_genres = source_genres.get_ptr();
            context.normalized_genres = normalized_genres.get_ptr();
            context.matched_rule = matched_rule.get_ptr();
            context.matched_token = matched_token.get_ptr();
            context.rule_index = rule_index;
            context.specificity_words = specificity_words;
            context.specificity_norm_len = specificity_norm_len;
            context.route_matched = route_matched;
            context.is_generic_match = is_generic_match;
            context.candidates_considered = candidates_considered;
            context.primary_min_bpm = primary_min_bpm;
            context.primary_max_bpm = primary_max_bpm;
            context.primary_center_bpm = primary_center_bpm;
            context.primary_spread_bpm = primary_spread_bpm;
            return context;
        }
    };

    struct Diagnostics {
        double       confidence   = 0.0;  ///< 0..100
        pfc::string8 method;
        pfc::string8 error_reason;
        pfc::string8 policy_reason;
        pfc::string8 decision_class;
        pfc::string8 decision_note;
        std::vector<MeasuredBpmCandidate> measured_bpm_candidates;
        bool         is_uncertain = false;
        bool         suppress_write = false;
    };

    virtual ~IBpmAnalyzer() = default;

    // Non-copyable (implementations hold SDK handles)
    IBpmAnalyzer(const IBpmAnalyzer&)            = delete;
    IBpmAnalyzer& operator=(const IBpmAnalyzer&) = delete;

    /// Run BPM analysis on the given track.
    /// Called from a worker thread — must be thread-safe per-instance.
    /// Returns detected BPM (0.0 on failure).
    virtual double analyze(metadb_handle_ptr              track,
                           threaded_process_status&       status,
                           abort_callback&                p_abort,
                           double                         priorMinBpm = 0.0,
                           double                         priorMaxBpm = 0.0,
                           const char*                    trackIdentifier = nullptr,
                           const RoutingLogContext*       routingContext = nullptr,
                           bool captureMeasuredCandidates = false) = 0;

    /// Returns diagnostics from the most recent analyze() call.
    [[nodiscard]] virtual const Diagnostics& get_diagnostics() const noexcept = 0;

protected:
    IBpmAnalyzer() = default;
};
