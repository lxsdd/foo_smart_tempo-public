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
