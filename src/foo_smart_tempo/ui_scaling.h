#pragma once

namespace smart_tempo::ui {

inline UINT detect_window_dpi(HWND window) {
  UINT dpi = 96;
  if (window != nullptr) {
    using get_dpi_for_window_fn = UINT(WINAPI*)(HWND);
    static const auto get_dpi_for_window =
        reinterpret_cast<get_dpi_for_window_fn>(
            ::GetProcAddress(::GetModuleHandleW(L"user32.dll"),
                             "GetDpiForWindow"));
    if (get_dpi_for_window != nullptr) {
      const UINT queried = get_dpi_for_window(window);
      if (queried > 0) dpi = queried;
    } else if (HDC dc = ::GetDC(window)) {
      const int queried = ::GetDeviceCaps(dc, LOGPIXELSX);
      if (queried > 0) dpi = static_cast<UINT>(queried);
      ::ReleaseDC(window, dc);
    }
  }
  return dpi;
}

inline int scale_for_window_dpi(HWND window, int value) {
  return ::MulDiv(value, static_cast<int>(detect_window_dpi(window)), 96);
}

inline int scale_between_dpi(int value, UINT oldDpi, UINT newDpi) {
  const UINT safeOldDpi = oldDpi > 0 ? oldDpi : 96;
  const UINT safeNewDpi = newDpi > 0 ? newDpi : 96;
  return ::MulDiv(value, static_cast<int>(safeNewDpi),
                  static_cast<int>(safeOldDpi));
}

}  // namespace smart_tempo::ui
