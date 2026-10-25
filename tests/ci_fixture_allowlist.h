#pragma once
// Shared ephemeral-only public synthetic identity gate; production builds
// never include this header. Neither observer accepts arbitrary owner files.
#include <cwchar>
#include <string>

namespace smart_tempo::ci_fixture {
inline bool permitted(const std::wstring& fixture,
                      const std::wstring& runner_temp) {
  if (fixture.empty() || runner_temp.empty()) return false;
  std::wstring root = runner_temp;
  if (root.back() != L'\\') root.push_back(L'\\');
  if (fixture.size() <= root.size() ||
      _wcsnicmp(fixture.c_str(), root.c_str(), root.size()) != 0)
    return false;
  constexpr const wchar_t* suffixes[] = {
      L"\\synthetic-oracle\\pulse_60.wav",
      L"\\synthetic-oracle\\pulse_128.wav",
      L"\\synthetic-oracle\\pulse_120_sparse.wav",
      L"\\synthetic-oracle\\silence.wav",
  };
  for (const wchar_t* suffix : suffixes) {
    const std::size_t n = std::wcslen(suffix);
    if (fixture.size() >= n &&
        _wcsicmp(fixture.c_str() + fixture.size() - n, suffix) == 0)
      return true;
  }
  return false;
}
}  // namespace smart_tempo::ci_fixture
