// Test-only in-process host driver. This file is compiled ONLY when the
// ephemeral GitHub Actions source workflow explicitly enables
// SmartTempoCIRuntimeProbe=true. Never link it into personal components.
// The driver calls the real registered context command by GUID; no guessing
// of unsupported foobar command-line options.
#ifndef SMART_TEMPO_CI_ORACLE_PROBE
#error "CI runner must build with the isolated numeric observer enabled"
#endif
#include "../src/foo_smart_tempo/stdafx.h"
#include "../src/foo_smart_tempo/guid.h"
#include "../src/foo_smart_tempo/mir_candidate_pipeline.h"
#include "foobar2000/SDK/foobar2000.h"
#include "foobar2000/SDK/menu_helpers.h"
#include "foobar2000/SDK/playlist_loader.h"

#include <array>
#include <span>
#include <cwchar>
#include <cstring>
#include <string>
#include <string_view>

namespace {
UINT_PTR g_ci_timer = 0;
std::wstring g_ci_fixture;

bool read_env(const wchar_t* key, std::wstring& out) {
    wchar_t value[32768] = {};
    DWORD len = ::GetEnvironmentVariableW(key, value, 32768);
    if (len == 0 || len >= 32768) return false;
    out.assign(value, len);
    return true;
}

// Run the genuine production selector directly on a fabricated public,
// transient MirCandidateBoard. This is NOT decoded-audio qualification, a BPM
// oracle, or a productive selector/writer change. It proves the positive
// native five-field continuous-center comparator on both host architectures.
bool native_continuous_selector_contract(double center,
                                          double expectedBpm,
                                          std::size_t expectedWins,
                                          std::size_t expectedLosses) {
    using namespace smart_tempo;
    using namespace smart_tempo::mir_pipeline;
    MirCandidateBoard board{};
    board.segment_count = 10;
    for (const double bpm : {60.0, 96.0, 99.0}) {
        MirRawCandidate candidate{};
        candidate.bpm = bpm;
        candidate.alias_bit = static_cast<uint16_t>(MirAlias::direct);
        candidate.alias_class = "direct";
        candidate.synthetic_support = bpm == 60.0 ? .65 : .40;
        candidate.synthetic_winner_support = .20;
        candidate.score_sum = .8;
        candidate.rows = 1;
        candidate.pulse_score = bpm == 60.0 ? .70 : .05;
        candidate.pulse_section_support = bpm == 60.0 ? .65 : .10;
        candidate.continuous_score_ratio = bpm == 60.0 ? 0.0 : 2.0;
        candidate.continuous_section_support = bpm == 60.0 ? 0.0 : .90;
        candidate.continuous_section_stability = bpm == 60.0 ? 0.0 : .80;
        candidate.continuous_score = bpm == 60.0 ? 0.0 : .50;
        board.raw_candidates.push_back(candidate);
    }
    MirRoutingContext routing{};
    routing.source_genres = "Chill";
    routing.matched_token = "Chill";
    routing.route_matched = true;
    const HodgkinsonTatumProbeResult mir{};
    const HodgkinsonMaterialRiskEvidence risk{};
    const auto selection = compute_mir_primary_selection(
        board, mir, risk, &routing, center - 6.0, center + 6.0, true);
    if (std::string_view(selection.lane) != "low_pulse_context" ||
        std::string_view(selection.lane_variant) !=
            "low_pulse_continuous_center_rescue_v1" ||
        !mir_ci_primary_selector_trace_complete())
        return false;
    std::size_t wins = 0, losses = 0, laneTerminals = 0;
    for (const auto& row : mir_ci_primary_selector_events()) {
        if (std::strcmp(row.reason, "low_continuous_compare_won") == 0 ||
            std::strcmp(row.reason, "low_continuous_compare_lost") == 0) {
            if (row.raw || !row.has_tie_metrics ||
                !row.has_tie_metric_4 ||
                row.tie_metrics[0] != 2.0 ||
                row.tie_metrics[1] != .90 ||
                row.tie_metrics[2] != .80 ||
                row.tie_metrics[3] != .50 ||
                row.tie_metric_4 != -std::abs(row.bpm - center))
                return false;
            if (std::strcmp(row.reason, "low_continuous_compare_won") == 0)
                ++wins;
            else
                ++losses;
        } else if (std::strcmp(row.reason, "terminal_lane_choice") == 0) {
            if (row.raw || row.bpm != expectedBpm)
                return false;
            ++laneTerminals;
        }
    }
    return wins == expectedWins && losses == expectedLosses &&
           laneTerminals == 1;
}


/*
 * Phase-B native supplemental source-object + measured-writer invariant
 * control. All candidates are fabricated public values in disposable CI.
 * This executes the ORIGINAL policy release selector and writer invariant,
 * but does not claim that decoded WAV audio selected this path.
 */
bool native_supplemental_policy_contract() {
    using namespace smart_tempo;
    using namespace smart_tempo::mir_pipeline;
    std::array<HodgkinsonPartialBarTopKExactCandidate, 2> rows{};
    for (auto& row : rows) {
        row.family_rank = 2;
        row.coarse_bpm = 144.0;
        row.local_exact_bpm = 144.0;
        row.local_exact_score = .45;
        row.score_sum = 2.0;
        row.score_ratio_to_top = .80;
        row.segment_count = 5;
        row.total_segment_count = 5;
        row.top5_hits = 5;
        row.fullboard_bpm = 96.0;
    }
    // Same BPM, distinct objects: the higher-evidence SECOND source must
    // win; numeric BPM coincidence cannot stand in for pointer identity.
    rows[1].family_rank = 1;
    rows[1].local_exact_score = .95;
    rows[1].score_ratio_to_top = .98;
    const auto candidates = std::span<const HodgkinsonPartialBarTopKExactCandidate>(
        rows.data(), rows.size());
    MirPolicyDecision decision{};
    decision.enabled = true;
    decision.family_conflict_review_hold_candidate = true;
    decision.current_bpm = 96.0;
    if (!try_apply_mir_policy_partial_bar_topk_exact_release(
            decision, candidates, 144.0, 8.0, true, false, true))
        return false;
    const auto selected = mir_ci_policy_pointer_source();
    if (!selected.observed || selected.in_fullboard ||
        selected.kind != MirCiSupplementalKind::partial_bar_topk ||
        selected.object != &rows[1] || selected.bpm != 144.0 ||
        !decision.output_would_write || decision.output_would_review_hold ||
        !decision.writer_override_candidate || decision.output_bpm != 144.0)
        return false;

    // Exercise the actual same-run measured-candidate writer guard with no
    // FullBoard, Primary, material-risk or aggregate fallback witnesses.
    const MirPrimarySelection primary{};
    const std::span<const MirPolicyMeasuredCandidate> noFullboard{};
    if (!mir_output_has_measured_candidate_witness(
            144.0, noFullboard, primary, nullptr, nullptr, candidates) ||
        !enforce_mir_measured_candidate_invariant(
            decision, noFullboard, primary, nullptr, nullptr, candidates))
        return false;

    // Adversarial output substitution must fail closed to ReviewHold, not
    // be accepted by the selected source object's score or nearby BPM.
    auto forged = decision;
    forged.output_bpm = 145.0;
    if (enforce_mir_measured_candidate_invariant(
            forged, noFullboard, primary, nullptr, nullptr, candidates) ||
        forged.output_would_write || !forged.output_would_review_hold ||
        forged.writer_override_candidate || forged.output_bpm != 0.0)
        return false;
    if (mir_output_has_measured_candidate_witness(
            144.002, noFullboard, primary, nullptr, nullptr, candidates))
        return false;

    // Negative Policy eligibility and source quality, on the same values.
    auto reject = [&](bool route, bool generic, bool enabled,
                      bool conflict, std::span<const HodgkinsonPartialBarTopKExactCandidate> input) {
        MirPolicyDecision denied{};
        denied.enabled = true;
        denied.family_conflict_review_hold_candidate = conflict;
        denied.current_bpm = 96.0;
        return !try_apply_mir_policy_partial_bar_topk_exact_release(
            denied, input, 144.0, 8.0, route, generic, enabled) &&
            !denied.output_would_write;
    };
    auto weak = rows[0];
    weak.local_exact_score = .01;
    const std::span<const HodgkinsonPartialBarTopKExactCandidate> badScore(&weak, 1);
    if (!reject(false, false, true, true, candidates) ||
        !reject(true, true, true, true, candidates) ||
        !reject(true, false, false, true, candidates) ||
        !reject(true, false, true, false, candidates) ||
        !reject(true, false, true, true, badScore))
        return false;
    return true;
}

bool write_native_supplemental_marker() {
    if (!native_supplemental_policy_contract()) return false;
    const std::wstring output = g_ci_fixture + L".native-supplemental";
    const HANDLE handle = ::CreateFileW(
        output.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    constexpr char kPass[] = "SMART_TEMPO_CI_NATIVE_SUPPLEMENTAL=PASS\n";
    DWORD written = 0;
    const bool ok = ::WriteFile(handle, kPass,
        static_cast<DWORD>(sizeof(kPass) - 1), &written, nullptr) &&
        written == sizeof(kPass) - 1;
    ::CloseHandle(handle);
    return ok;
}


// Phase-B actual native low-pulse NESTED rescue, executed inside original
// Win32/x64 foobar with a disposable fabricated three-source board.
// Both runs differ in ONE alias bit only. This is NOT decoded WAV accuracy,
// a metadata selector override, or a modified productive comparator.
bool native_low_nested_rescue_contract(bool twoThirdsAlias,
                                       double expectedBpm,
                                       std::size_t expectedEpochs) {
    using namespace smart_tempo;
    using namespace smart_tempo::mir_pipeline;
    MirCandidateBoard board{};
    board.segment_count = 10;
    for (double bpm : {60.0, 80.0, 71.0}) {
        MirRawCandidate candidate{};
        candidate.bpm = bpm;
        candidate.alias_bit = bpm == 60.0
            ? static_cast<uint16_t>(MirAlias::direct)
            : bpm == 80.0
                ? static_cast<uint16_t>(twoThirdsAlias
                    ? MirAlias::two_thirds : MirAlias::direct)
                : static_cast<uint16_t>(MirAlias::half) |
                    static_cast<uint16_t>(MirAlias::three_quarters);
        candidate.alias_class = "direct";
        candidate.synthetic_support = bpm == 60.0 ? .65 : bpm == 80.0 ? .95 : .70;
        candidate.synthetic_winner_support = .20;
        candidate.best_combined_score = .20;
        candidate.score_sum = 1.0;
        candidate.rows = 1;
        candidate.pulse_score = bpm == 60.0 ? 4.0 : .04;
        candidate.pulse_section_support = bpm == 60.0 ? 1.0 : .05;
        board.raw_candidates.push_back(candidate);
    }
    MirRoutingContext route{};
    route.source_genres = "Chill";
    route.matched_token = "Chill";
    route.route_matched = true;
    const HodgkinsonTatumProbeResult mir{};
    const HodgkinsonMaterialRiskEvidence risk{};
    const auto result = compute_mir_primary_selection(
        board, mir, risk, &route, 50.0, 88.0, true);
    if (std::string_view(result.lane) != "low_pulse_context" ||
        std::string_view(result.lane_variant) != "low_pulse_targeted_rescue_v4" ||
        result.selected_bpm != expectedBpm ||
        !mir_ci_primary_selector_trace_complete()) return false;

    // Adjacent source-indexed rank/compare witnesses for every *actual*
    // invocation of bestBy. Each new pool must reset its incumbent.
    const auto events = mir_ci_primary_selector_events();
    std::array<std::size_t, 4> ranks{}, comparisons{}, wins{}, losses{};
    const double expectedWinners[4] = {0.0, 60.0, 80.0, 71.0};
    bool pendingRank = false;
    std::size_t rankIndex = 0, maxEpoch = 0, finalTerminals = 0;
    for (const auto& event : events) {
        if (std::strcmp(event.reason, "low_rank_scored") == 0) {
            if (pendingRank || !event.has_low_pool_epoch ||
                event.low_pool_epoch < 1 || event.low_pool_epoch > expectedEpochs ||
                event.raw || event.source_index >= 3) return false;
            pendingRank = true;
            rankIndex = event.source_index;
            ++ranks[event.low_pool_epoch];
            if (event.low_pool_epoch > maxEpoch + 1) return false;
            maxEpoch = (std::max)(maxEpoch, event.low_pool_epoch);
        } else if (std::strcmp(event.reason, "low_compare_won") == 0 ||
                   std::strcmp(event.reason, "low_compare_lost") == 0) {
            if (!pendingRank || event.raw || !event.has_low_pool_epoch ||
                event.low_pool_epoch < 1 || event.low_pool_epoch > maxEpoch ||
                event.source_index != rankIndex || event.source_index >= 3)
                return false;
            pendingRank = false;
            const std::size_t ep = event.low_pool_epoch;
            ++comparisons[ep];
            if (std::strcmp(event.reason, "low_compare_won") == 0) {
                ++wins[ep];
                if (event.bpm != expectedWinners[ep]) return false;
            } else {
                ++losses[ep];
            }
        } else if (std::strcmp(event.reason, "terminal_lane_choice") == 0) {
            if (event.raw || event.bpm != expectedBpm ||
                event.source_index != (twoThirdsAlias ? 2u : 1u))
                return false;
            ++finalTerminals;
        }
    }
    if (pendingRank || maxEpoch != expectedEpochs || finalTerminals != 1 ||
        ranks[1] != 3 || ranks[2] != 2 ||
        wins[1] != 1 || wins[2] != 1 ||
        losses[1] != 2 || losses[2] != 1)
        return false;
    if (twoThirdsAlias)
        return expectedEpochs == 3 && ranks[3] == 1 &&
               comparisons[3] == 1 && wins[3] == 1 && losses[3] == 0;
    return expectedEpochs == 2 && ranks[3] == 0 &&
           comparisons[1] == 3 && comparisons[2] == 2;
}

bool write_native_low_nested_marker() {
    // Positive 3-episode rescue: 60 -> 80 -> 71.
    // Negative one-bit control: same evidence except missing 80 two-thirds
    // alias; the *third* rescue must never execute, leaving final 80.
    if (!native_low_nested_rescue_contract(true, 71.0, 3) ||
        !native_low_nested_rescue_contract(false, 80.0, 2))
        return false;
    const std::wstring output = g_ci_fixture + L".native-low-nested";
    const HANDLE handle = ::CreateFileW(
        output.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    constexpr char kPass[] = "SMART_TEMPO_CI_NATIVE_LOW_NESTED=PASS\n";
    DWORD written = 0;
    const bool ok = ::WriteFile(handle, kPass,
        static_cast<DWORD>(sizeof(kPass) - 1), &written, nullptr) &&
        written == sizeof(kPass) - 1;
    ::CloseHandle(handle);
    return ok;
}


/*
 * Actual original selector raw alias / support fallback. All three cases
 * execute native C++ on CI-FABRICATED same input objects; the ONLY changes:
 * raw alias on second candidate and its support.
 * 0: all aliases rejected, all support rejected -> both FALLBACKs
 * 1: second is direct/high support -> NO fallback; first never ranked
 * 2: all aliases rejected, only second supported -> alias fallback ONLY
 * This never modifies metadata, DSP, the production selector or writer.
 */
bool native_raw_fallback_contract(int variant) {
    using namespace smart_tempo;
    using namespace smart_tempo::mir_pipeline;
    if (variant < 0 || variant > 5) return false;
    MirCandidateBoard board{};
    board.segment_count = 10;
    for (int index = 0; index < 2; ++index) {
        MirRawCandidate raw{};
        raw.bpm = index == 0 ? 103.0 : 129.0;
        raw.alias_bit = index == 0
            ? static_cast<uint16_t>(
                variant == 5 ? MirAlias::direct : MirAlias::one_third)
            : static_cast<uint16_t>(
                variant == 1 || variant >= 3 ? MirAlias::direct : MirAlias::quarter);
        raw.alias_class = index == 0
            ? (variant == 5 ? "direct" : "one_third")
            : (variant == 1 || variant >= 3 ? "direct" : "quarter");
        raw.synthetic_support = index == 0 ? .01 :
                                variant == 0 ? .02 :
                                variant == 3 ? .08 :
                                variant == 4 ? .079999 : .80;
        raw.synthetic_winner_support = 0.01;
        raw.score_sum = index == 0 ? .10 : 3.0;
        raw.best_combined_score = index == 0 ? .10 : .90;
        raw.rows = 1;
        board.raw_candidates.push_back(raw);
    }
    MirRoutingContext route{};
    route.source_genres = "Acapella";
    route.matched_token = "Acapella";
    route.route_matched = true;
    const HodgkinsonTatumProbeResult mir{};
    const HodgkinsonMaterialRiskEvidence risk{};
    const auto result = compute_mir_primary_selection(
        board, mir, risk, &route, 111.0, 157.0, true);
    if (std::string_view(result.lane) != "sparse_or_acapella" ||
        std::string_view(result.lane_variant) != "conservative" ||
        result.selected_bpm != 129.0 ||
        !mir_ci_primary_selector_trace_complete())
        return false;

    struct Expected {
        const char* reason;
        std::size_t source;
        bool raw;
        bool included;
    };
    using E = Expected;
    const std::array<E, 15> fallbackBoth{{
        E{"raw_alias_provisional_exclusion", 0, true, false},
        E{"raw_alias_provisional_exclusion", 1, true, false},
        E{"raw_alias_fallback_readmitted", 0, true, true},
        E{"raw_alias_fallback_readmitted", 1, true, true},
        E{"raw_support_provisional_exclusion", 0, true, false},
        E{"raw_support_provisional_exclusion", 1, true, false},
        E{"raw_support_fallback_readmitted", 0, true, true},
        E{"raw_support_fallback_readmitted", 1, true, true},
        E{"raw_rank_scored", 0, true, true},
        E{"raw_compare_won", 0, true, true},
        E{"raw_rank_scored", 1, true, true},
        E{"raw_compare_won", 1, true, true},
        E{"terminal_lane_choice", 1, false, true},
        E{"terminal_residual_choice", 1, false, true},
        E{"terminal_final_choice", 1, false, true},
    }};
    const std::array<E, 9> directOnly{{
        E{"raw_alias_provisional_exclusion", 0, true, false},
        E{"raw_alias_allowed", 1, true, true},
        E{"raw_alias_decisive_veto", 0, true, false},
        E{"raw_support_allowed", 1, true, true},
        E{"raw_rank_scored", 1, true, true},
        E{"raw_compare_won", 1, true, true},
        E{"terminal_lane_choice", 1, false, true},
        E{"terminal_residual_choice", 1, false, true},
        E{"terminal_final_choice", 1, false, true},
    }};
    const std::array<E, 12> aliasOnly{{
        E{"raw_alias_provisional_exclusion", 0, true, false},
        E{"raw_alias_provisional_exclusion", 1, true, false},
        E{"raw_alias_fallback_readmitted", 0, true, true},
        E{"raw_alias_fallback_readmitted", 1, true, true},
        E{"raw_support_provisional_exclusion", 0, true, false},
        E{"raw_support_allowed", 1, true, true},
        E{"raw_support_decisive_veto", 0, true, false},
        E{"raw_rank_scored", 1, true, true},
        E{"raw_compare_won", 1, true, true},
        E{"terminal_lane_choice", 1, false, true},
        E{"terminal_residual_choice", 1, false, true},
        E{"terminal_final_choice", 1, false, true},
    }};
    // Three independent sparse floor/source-ID controls, all exercising
    // the unchanged native productive path within isolated foobar CI.
    const std::array<E, 9> supportAtFloor{{
        E{"raw_alias_provisional_exclusion", 0, true, false},
        E{"raw_alias_allowed", 1, true, true},
        E{"raw_alias_decisive_veto", 0, true, false},
        E{"raw_support_allowed", 1, true, true},
        E{"raw_rank_scored", 1, true, true},
        E{"raw_compare_won", 1, true, true},
        E{"terminal_lane_choice", 1, false, true},
        E{"terminal_residual_choice", 1, false, true},
        E{"terminal_final_choice", 1, false, true},
    }};
    const std::array<E, 10> supportBelowFloor{{
        E{"raw_alias_provisional_exclusion", 0, true, false},
        E{"raw_alias_allowed", 1, true, true},
        E{"raw_alias_decisive_veto", 0, true, false},
        E{"raw_support_provisional_exclusion", 1, true, false},
        E{"raw_support_fallback_readmitted", 1, true, true},
        E{"raw_rank_scored", 1, true, true},
        E{"raw_compare_won", 1, true, true},
        E{"terminal_lane_choice", 1, false, true},
        E{"terminal_residual_choice", 1, false, true},
        E{"terminal_final_choice", 1, false, true},
    }};
    const std::array<E, 10> supportVetoOnly{{
        E{"raw_alias_allowed", 0, true, true},
        E{"raw_alias_allowed", 1, true, true},
        E{"raw_support_provisional_exclusion", 0, true, false},
        E{"raw_support_allowed", 1, true, true},
        E{"raw_support_decisive_veto", 0, true, false},
        E{"raw_rank_scored", 1, true, true},
        E{"raw_compare_won", 1, true, true},
        E{"terminal_lane_choice", 1, false, true},
        E{"terminal_residual_choice", 1, false, true},
        E{"terminal_final_choice", 1, false, true},
    }};
    const auto eventMatches = [](const MirCiSelectorEvent& event,
                                 const E& expected) {
        return std::strcmp(event.reason, expected.reason) == 0 &&
               event.source_index == expected.source &&
               event.raw == expected.raw &&
               event.included == expected.included &&
               event.bpm == (expected.source == 0 ? 103.0 : 129.0);
    };
    const auto events = mir_ci_primary_selector_events();
    const auto check = [&](const auto& expected) {
        if (events.size() != expected.size()) return false;
        for (std::size_t i = 0; i < expected.size(); ++i) {
            if (!eventMatches(events[i], expected[i]))
                return false;
        }
        return true;
    };
    if (variant == 0 && !check(fallbackBoth)) return false;
    if (variant == 1 && !check(directOnly)) return false;
    if (variant == 2 && !check(aliasOnly)) return false;
    if (variant == 3 && !check(supportAtFloor)) return false;
    if (variant == 4 && !check(supportBelowFloor)) return false;
    if (variant == 5 && !check(supportVetoOnly)) return false;
    // Exact native source-index terminal and at least one genuine raw
    // comparator; first alias exclusion must NOT be called a final veto.
    return true;
}


/* CI-only original productive general/harmonic rail boundaries.
 * 0 = General with two decisive out-of-rail excludes;
 * 1 = Harmonic with two decisive excludes and NO fallback;
 * 2 = General with zero in-rail sources, mandatory source-indexed readmission.
 * These are disposable FABRICATED MIR boards, not naturally decoded audio. */
bool native_rail_contract(int variant) {
    using namespace smart_tempo;
    using namespace smart_tempo::mir_pipeline;
    if (variant < 0 || variant > 2) return false;
    MirCandidateBoard board{};
    board.segment_count = 10;
    const int count = variant == 2 ? 2 : 3;
    for (int i=0; i<count; ++i) {
        MirRawCandidate candidate{};
        candidate.bpm = i==0 ? 30.0 : i==1 && count==3 ? 128.0 : 230.0;
        candidate.alias_bit = static_cast<uint16_t>(MirAlias::direct);
        candidate.alias_class = "direct";
        candidate.synthetic_support =
            variant == 1 ? (i < 2 ? .90 : .20) :
            (i == 0 ? .25 : i == 1 ? .35 : .20);
        candidate.synthetic_winner_support=.05;
        candidate.score_sum=.25;
        candidate.best_combined_score=.10;
        candidate.rows=1;
        board.raw_candidates.push_back(candidate);
    }
    const HodgkinsonTatumProbeResult mir{};
    const HodgkinsonMaterialRiskEvidence risk{};
    const auto result = compute_mir_primary_selection(
        board,mir,risk,nullptr,100.,160.,true);
    if (std::string_view(result.lane) !=
            (variant==1 ? "harmonic_conflict" : "general") ||
        !mir_ci_primary_selector_trace_complete())
        return false;
    const char* expected = variant==1 ? "harmonic_rail_decisive_veto"
                                    : "general_rail_decisive_veto";
    const char* fallback = "general_rail_fallback_readmitted";
    std::size_t excluded=0,readmitted=0,firstGate=0;
    std::size_t rank=0,compare=0,terminal=0;
    for (const auto& row : mir_ci_primary_selector_events()) {
        if (row.raw) return false;
        if (std::strcmp(row.reason,expected)==0) {
            if (row.included || row.source_index==1 && variant!=2)
                return false;
            ++excluded;
        }
        if (std::strcmp(row.reason,fallback)==0) {
            if (!row.included || variant!=2) return false;
            ++readmitted;
        }
        if (std::strcmp(row.reason,variant==1 ? "harmonic_in_rail" :
                        "general_in_rail")==0 ||
            std::strcmp(row.reason,variant==1 ? "harmonic_out_of_rail" :
                        "general_out_of_rail_provisional")==0)
            ++firstGate;
        if (std::strcmp(row.reason,variant==1 ? "harmonic_rank_scored" :
                        "general_baseline_rank_scored")==0) ++rank;
        if (std::strcmp(row.reason,variant==1 ? "harmonic_compare_won" :
                        "general_baseline_compare_won")==0 ||
            std::strcmp(row.reason,variant==1 ? "harmonic_compare_lost" :
                        "general_baseline_compare_lost")==0) ++compare;
        if (std::strcmp(row.reason,"terminal_lane_choice")==0) ++terminal;
    }
    if (firstGate!=static_cast<std::size_t>(count)) return false;
    if (variant == 2)
        return excluded==0 && readmitted==2 && rank==2 &&
               compare==2 && terminal==1;
    return excluded==2 && readmitted==0 && rank==1 &&
           compare==1 && terminal==1 && result.selected_bpm==128.0;
}
bool write_native_rail_marker() {
    if (!native_rail_contract(0) || !native_rail_contract(1) ||
        !native_rail_contract(2)) return false;
    const std::wstring path=g_ci_fixture+L".native-cluster-rail";
    HANDLE handle=::CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,
                                FILE_ATTRIBUTE_NORMAL,nullptr);
    if (handle==INVALID_HANDLE_VALUE) return false;
    constexpr char proof[]="SMART_TEMPO_CI_NATIVE_RAIL=PASS\n";
    DWORD written=0;
    const bool ok=::WriteFile(handle,proof,
        static_cast<DWORD>(sizeof(proof)-1),&written,nullptr) &&
        written==sizeof(proof)-1;
    ::CloseHandle(handle);
    return ok;
}

bool write_native_raw_fallback_marker() {
    if (!native_raw_fallback_contract(0) ||
        !native_raw_fallback_contract(1) ||
        !native_raw_fallback_contract(2) ||
        !native_raw_fallback_contract(3) ||
        !native_raw_fallback_contract(4) ||
        !native_raw_fallback_contract(5))
        return false;
    const std::wstring output = g_ci_fixture + L".native-raw-fallback";
    const HANDLE handle = ::CreateFileW(
        output.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    constexpr char kPass[] = "SMART_TEMPO_CI_NATIVE_RAW_FALLBACK=PASS\n";
    DWORD written = 0;
    const bool ok = ::WriteFile(handle, kPass,
        static_cast<DWORD>(sizeof(kPass) - 1), &written, nullptr) &&
        written == sizeof(kPass) - 1;
    ::CloseHandle(handle);
    return ok;
}

bool write_native_continuous_marker() {
    // Run both centers on exactly the SAME fabricated sources. Reversing
    // only the route-center distance must reverse the measured incumbent
    // verdict. Fail closed if native C++ silently diverges from Python replay.
    if (!native_continuous_selector_contract(99., 99., 2, 0) ||
        !native_continuous_selector_contract(96., 96., 1, 1))
        return false;
    const std::wstring output = g_ci_fixture + L".native-continuous";
    const HANDLE handle = ::CreateFileW(
        output.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    constexpr char kPass[] = "SMART_TEMPO_CI_NATIVE_CONTINUOUS=PASS\n";
    DWORD written = 0;
    const bool ok = ::WriteFile(handle, kPass,
        static_cast<DWORD>(sizeof(kPass) - 1), &written, nullptr) &&
        written == sizeof(kPass) - 1;
    ::CloseHandle(handle);
    return ok;
}

void CALLBACK run_ci_probe(HWND, UINT, UINT_PTR timer, DWORD) {
    // Complete foobar's one-time appearance wizard before invoking any menu.
    if (::FindWindowW(nullptr, L"Quick Appearance Setup") != nullptr) return;
    const std::wstring readyMarker = g_ci_fixture + L".ready";
    if (::GetFileAttributesW(readyMarker.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    ::KillTimer(nullptr, timer);
    g_ci_timer = 0;

    try {
        if (!write_native_supplemental_marker()) {
            FB2K_console_formatter() << "SMART_TEMPO_CI_NATIVE_SUPPLEMENTAL=FAIL";
            return;
        }
        FB2K_console_formatter() << "SMART_TEMPO_CI_NATIVE_SUPPLEMENTAL=PASS";
        if (!write_native_low_nested_marker()) {
            FB2K_console_formatter() << "SMART_TEMPO_CI_NATIVE_LOW_NESTED=FAIL";
            return;
        }
        FB2K_console_formatter() << "SMART_TEMPO_CI_NATIVE_LOW_NESTED=PASS";
        if (!write_native_rail_marker()) {
            FB2K_console_formatter() << "SMART_TEMPO_CI_NATIVE_RAIL=FAIL";
            return;
        }
        FB2K_console_formatter() << "SMART_TEMPO_CI_NATIVE_RAIL=PASS";
        if (!write_native_raw_fallback_marker()) {
            FB2K_console_formatter() << "SMART_TEMPO_CI_NATIVE_RAW_FALLBACK=FAIL";
            return;
        }
        FB2K_console_formatter() << "SMART_TEMPO_CI_NATIVE_RAW_FALLBACK=PASS";
        if (!write_native_continuous_marker()) {
            FB2K_console_formatter() << "SMART_TEMPO_CI_NATIVE_CONTINUOUS=FAIL";
            return;
        }
        FB2K_console_formatter() << "SMART_TEMPO_CI_NATIVE_CONTINUOUS=PASS";
        const pfc::stringcvt::string_utf8_from_wide path(g_ci_fixture.c_str());
        metadb_handle_list tracks;
        playlist_loader::g_path_to_handles_simple(path.get_ptr(), tracks, fb2k::noAbort);
        FB2K_console_formatter() << "SMART_TEMPO_CI_PROBE_HANDLES=" << static_cast<uint64_t>(tracks.get_count());
        if (tracks.get_count() == 0) {
            FB2K_console_formatter() << "SMART_TEMPO_CI_PROBE=FAIL_NO_HANDLES";
            return;
        }
        const auto state = metadb_io::get()->load_info_multi(
            tracks, metadb_io::load_info_force, core_api::get_main_window(), false);
        if (state != metadb_io::load_info_success) {
            FB2K_console_formatter() << "SMART_TEMPO_CI_PROBE=FAIL_METADATA_READ";
            return;
        }
        const bool dispatched = menu_helpers::run_command_context(
            guid_batch_dry_run_bpm, pfc::guid_null, tracks);
        FB2K_console_formatter()
            << (dispatched ? "SMART_TEMPO_CI_PROBE=DISPATCHED"
                           : "SMART_TEMPO_CI_PROBE=FAIL_MENU_DISPATCH");
    } catch (const std::exception& e) {
        FB2K_console_formatter() << "SMART_TEMPO_CI_PROBE=FAIL_EXCEPTION " << e.what();
    } catch (...) {
        FB2K_console_formatter() << "SMART_TEMPO_CI_PROBE=FAIL_UNKNOWN";
    }
}

class smart_tempo_ci_probe : public initquit {
public:
    void on_init() override {
        std::wstring githubActions, tempRoot, fixture;
        if (!read_env(L"GITHUB_ACTIONS", githubActions) ||
            githubActions != L"true" ||
            !read_env(L"RUNNER_TEMP", tempRoot) ||
            !read_env(L"SMART_TEMPO_CI_FIXTURE", fixture)) return;

        // Never touch a path outside the temporary GitHub runner tree.
        if (tempRoot.empty() || tempRoot.back() != L'\\') tempRoot += L'\\';
        if (_wcsnicmp(fixture.c_str(), tempRoot.c_str(), tempRoot.size()) != 0)
            return;

        g_ci_fixture = std::move(fixture);
        g_ci_timer = ::SetTimer(nullptr, 0, 750, &run_ci_probe);
        if (g_ci_timer == 0) {
            FB2K_console_formatter() << "SMART_TEMPO_CI_PROBE=FAIL_TIMER";
        } else {
            FB2K_console_formatter() << "SMART_TEMPO_CI_PROBE=ARMED";
        }
    }

    void on_quit() override {
        if (g_ci_timer != 0) {
            ::KillTimer(nullptr, g_ci_timer);
            g_ci_timer = 0;
        }
        g_ci_fixture.clear();
    }
};
static initquit_factory_t<smart_tempo_ci_probe> g_smart_tempo_ci_probe;
} // namespace
