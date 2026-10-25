#include "fullscreen_window.h"
#include <iostream>
#include <cstdlib>
using namespace jisu;
void require(bool ok, const char* message) { if (!ok) { std::cerr << "FAIL fullscreen: " << message << std::endl; std::exit(1); } }
FullscreenWindow fullscreen;
LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM w, LPARAM l) {
  if (message == WM_NCCALCSIZE && w) return 0;  // Same borderless client policy as BrowserWindow.
  return DefWindowProcW(window, message, w, l);
}
int main() {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  WNDCLASSW wc{}; wc.lpfnWndProc = WindowProc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"JisuFullscreenGate";
  require(RegisterClassW(&wc) != 0, "register window");
  // Hidden native window only: no browser/profile, no visible window and no focus changes.
  HWND window = CreateWindowW(wc.lpszClassName, L"Fullscreen gate", WS_OVERLAPPEDWINDOW, 80, 90, 1024, 768,
                              nullptr, nullptr, wc.hInstance, nullptr);
  require(window != nullptr, "create hidden window");
  const HWND foreground = GetForegroundWindow();
  for (bool maximized : {false, true}) {
    auto style = GetWindowLongPtrW(window, GWL_STYLE);
    if (maximized) SetWindowLongPtrW(window, GWL_STYLE, style | WS_MAXIMIZE);
    RECT before{}; GetWindowRect(window, &before);
    WINDOWPLACEMENT placement{sizeof(placement)}; GetWindowPlacement(window, &placement);
    const auto originalStyle = GetWindowLongPtrW(window, GWL_STYLE);
    for (int cycle = 0; cycle < 3; ++cycle) {
      require(fullscreen.Enter(window), "enter fullscreen");
      require(fullscreen.Enter(window), "idempotent enter");
      require(fullscreen.Active(), "native state active");
      require(!(GetWindowLongPtrW(window, GWL_STYLE) & WS_OVERLAPPEDWINDOW), "native frame removed");
      MONITORINFO monitor{sizeof(monitor)}; GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor);
      RECT bounds{}, client{}; GetWindowRect(window, &bounds); GetClientRect(window, &client);
      require(EqualRect(&bounds, &monitor.rcMonitor), "monitor bounds, not work area");
      const auto content = fullscreen.ContentBounds(client, 121);
      require(content.top == 0 && EqualRect(&content, &client), "no tab/address/bookmark gutter");
      fullscreen.FitToMonitor(window);
      require(!IsWindowVisible(window), "never show hidden window");
      require(GetForegroundWindow() == foreground, "never activate window");
      fullscreen.Exit(window);
      fullscreen.Exit(window);
      require(!fullscreen.Active(), "exit state");
      require((GetWindowLongPtrW(window, GWL_STYLE) & WS_OVERLAPPEDWINDOW) == (originalStyle & WS_OVERLAPPEDWINDOW), "restore native frame");
      WINDOWPLACEMENT restored{sizeof(restored)}; GetWindowPlacement(window, &restored);
      require(EqualRect(&placement.rcNormalPosition, &restored.rcNormalPosition), "restore saved normal placement");
      require((GetWindowLongPtrW(window, GWL_STYLE) & WS_MAXIMIZE) == (originalStyle & WS_MAXIMIZE), "restore maximized style");
      GetClientRect(window, &client);
      require(fullscreen.ContentBounds(client, 121).top == 121, "restore chrome gutter");
      require(!IsWindowVisible(window) && GetForegroundWindow() == foreground, "exit does not show or activate");
    }
  }
  DestroyWindow(window);
  require(!fullscreen.Enter(nullptr), "invalid host fails closed");
  std::cout << "PASS fullscreen: monitor bounds; zero chrome gutter; frame/placement/show-state restore; repeated entry/exit; no activation; hidden host stays hidden; invalid host rejected\n";
}
