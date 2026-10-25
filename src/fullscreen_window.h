// Native part of HTML/F11 fullscreen. Never activates or raises the window.
#pragma once
#include <windows.h>

namespace jisu {
class FullscreenWindow {
 public:
  bool Active() const { return active_; }
  bool Enter(HWND window) {
    if (active_) return true;
    placement_ = {sizeof(WINDOWPLACEMENT)};
    if (!GetWindowPlacement(window, &placement_)) return false;
    MONITORINFO monitor{sizeof(MONITORINFO)};
    if (!GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor)) return false;
    style_ = GetWindowLongPtrW(window, GWL_STYLE);
    active_ = true;  // WM_NCCALCSIZE must not constrain fullscreen to the work area.
    SetWindowLongPtrW(window, GWL_STYLE, style_ & ~(WS_OVERLAPPEDWINDOW | WS_MAXIMIZE | WS_MINIMIZE));
    if (Position(window, monitor.rcMonitor)) return true;
    Exit(window);
    return false;
  }
  void Exit(HWND window) {
    if (!active_) return;
    active_ = false;
    SetWindowLongPtrW(window, GWL_STYLE, style_);
    SetWindowPlacement(window, &placement_);
    SetWindowPos(window, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
  }
  void FitToMonitor(HWND window) const {
    if (!active_) return;
    MONITORINFO monitor{sizeof(MONITORINFO)};
    if (GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor)) Position(window, monitor.rcMonitor);
  }
  RECT ContentBounds(RECT client, LONG chromeTop) const {
    if (!active_) client.top = chromeTop < client.bottom ? chromeTop : client.bottom;
    return client;
  }
 private:
  static bool Position(HWND window, const RECT& bounds) {
    return SetWindowPos(window, nullptr, bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top,
                        SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED) != FALSE;
  }
  bool active_ = false;
  LONG_PTR style_ = 0;
  WINDOWPLACEMENT placement_{sizeof(WINDOWPLACEMENT)};
};
}  // namespace jisu
