#pragma once

#include <windows.h>

namespace huxerui::detail {

// Burn requires a real owner HWND even when the bootstrapper has no visible interface.
class InstallerNativeWindow {
public:
  InstallerNativeWindow()
      : window_(CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"HuxerUI installer", WS_POPUP,
                                0, 0, 0, 0, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr)) {}

  ~InstallerNativeWindow() {
    if (window_ != nullptr) {
      DestroyWindow(window_);
    }
  }

  InstallerNativeWindow(const InstallerNativeWindow&) = delete;
  InstallerNativeWindow& operator=(const InstallerNativeWindow&) = delete;

  [[nodiscard]] HWND Handle() const { return window_; }

  [[nodiscard]] DWORD Run() const {
    MSG message{};
    for (;;) {
      const BOOL result = GetMessageW(&message, nullptr, 0, 0);
      if (result == -1) {
        return GetLastError();
      }
      if (result == 0 || (message.hwnd == window_ && message.message == exit_message)) {
        return static_cast<DWORD>(message.wParam);
      }
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
  }

  [[nodiscard]] static bool RequestExit(HWND window, DWORD exit_code) {
    return PostMessageW(window, exit_message, exit_code, 0) != FALSE;
  }

private:
  static constexpr UINT exit_message = WM_APP + 1;
  HWND window_ = nullptr;
};

} // namespace huxerui::detail
