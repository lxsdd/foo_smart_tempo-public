#pragma once
// CI-only source, included exclusively from bpm_auto_analysis_thread.cpp
// when SmartTempoCIRuntimeProbe=true. NEVER compile into owner/public packages.
// Reports only numeric synthetic results in the isolated Actions runner,
// never source paths, track names, tags, or external audio.
#include <windows.h>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include <cstdint>

namespace smart_tempo_ci {
inline bool write_report(const std::vector<double>& bpms,
                         const std::vector<double>& confidences,
                         const std::vector<uint8_t>& suppress,
                         double elapsed_seconds) noexcept {
    wchar_t action[16]{}, temp[32768]{}, fixture[32768]{};
    const auto na = ::GetEnvironmentVariableW(L"GITHUB_ACTIONS", action, 16);
    const auto nt = ::GetEnvironmentVariableW(L"RUNNER_TEMP", temp, 32768);
    const auto nf = ::GetEnvironmentVariableW(L"SMART_TEMPO_CI_FIXTURE", fixture, 32768);
    if (na != 4 || std::wstring(action) != L"true" ||
        nt == 0 || nt >= 32768 || nf == 0 || nf >= 32768) return false;
    std::wstring root(temp, nt);
    if (!root.empty() && root.back() != L'\\') root += L'\\';
    const std::wstring file(fixture, nf);
    // Whitelist only files directly under runner temp descendants.
    if (file.size() <= root.size() ||
        _wcsnicmp(file.c_str(), root.c_str(), root.size()) != 0 ||
        file.find(L"..\\") != std::wstring::npos ||
        file.find(L"../") != std::wstring::npos) return false;
    if (bpms.empty() || bpms.size() != suppress.size() ||
        bpms.size() != confidences.size() || bpms.size() > 64) return false;
    const std::wstring reportPath = file + L".tempo.tsv";
    HANDLE handle = ::CreateFileW(reportPath.c_str(), GENERIC_WRITE, 0, nullptr,
                                   CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    bool ok = true;
    auto append = [&](const char* s, size_t bytes) {
        DWORD written = 0;
        if (!::WriteFile(handle, s, static_cast<DWORD>(bytes), &written, nullptr) ||
            written != bytes) ok = false;
    };
    const char header[] = "smart_tempo_ci_result_v1\nindex\tbpm\tblocked\tconfidence\n";
    append(header, sizeof(header) - 1);
    char line[256]{};
    for (size_t i = 0; i < bpms.size(); ++i) {
        const double bpm = std::isfinite(bpms[i]) ? bpms[i] : 0.0;
        const double conf = std::isfinite(confidences[i]) ? confidences[i] : 0.0;
        const int n = std::snprintf(line, sizeof(line), "%zu\t%.6f\t%u\t%.4f\n",
                                    i, bpm, static_cast<unsigned>(suppress[i]), conf);
        if (n <= 0 || static_cast<size_t>(n) >= sizeof(line)) { ok = false; break; }
        append(line, static_cast<size_t>(n));
    }
    const int n = std::snprintf(line, sizeof(line), "elapsed_seconds\t%.6f\n",
                                 std::isfinite(elapsed_seconds) ? elapsed_seconds : 0.0);
    if (n > 0 && static_cast<size_t>(n) < sizeof(line)) {
        append(line, static_cast<size_t>(n));
    } else {
        ok = false;
    }
    if (!::CloseHandle(handle)) ok = false;
    return ok;
}
} // namespace smart_tempo_ci
