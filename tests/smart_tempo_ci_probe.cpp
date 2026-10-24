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
#include "foobar2000/SDK/foobar2000.h"
#include "foobar2000/SDK/menu_helpers.h"
#include "foobar2000/SDK/playlist_loader.h"

#include <cwchar>
#include <string>

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

void CALLBACK run_ci_probe(HWND, UINT, UINT_PTR timer, DWORD) {
    // Complete foobar's one-time appearance wizard before invoking any menu.
    if (::FindWindowW(nullptr, L"Quick Appearance Setup") != nullptr) return;
    const std::wstring readyMarker = g_ci_fixture + L".ready";
    if (::GetFileAttributesW(readyMarker.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    ::KillTimer(nullptr, timer);
    g_ci_timer = 0;

    try {
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
