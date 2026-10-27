#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>
#include "tab_strip.h"

namespace {
void Require(bool ok, const char* label) {
  printf("%s: %s\n", label, ok ? "PASS" : "FAIL");
  fflush(stdout);
  if (!ok) throw std::runtime_error(label);
}
std::vector<HWND> Windows(DWORD pid) {
  struct Search { DWORD pid; std::vector<HWND> windows; } search{pid, {}};
  EnumWindows([](HWND window, LPARAM data) -> BOOL {
    auto& s = *reinterpret_cast<Search*>(data);
    DWORD owner = 0; GetWindowThreadProcessId(window, &owner);
    wchar_t cls[128]{}; GetClassNameW(window, cls, 128);
    if (owner == s.pid && std::wstring(cls) == L"JisuBrowserWindow" && IsWindowVisible(window))
      s.windows.push_back(window);
    return TRUE;
  }, reinterpret_cast<LPARAM>(&search));
  return search.windows;
}
std::wstring Title(HWND window) {
  wchar_t text[1024]{}; GetWindowTextW(window, text, 1024); return text;
}
template<class Predicate> bool Await(Predicate predicate, int attempts = 150) {
  for (int i = 0; i < attempts; ++i) { if (predicate()) return true; Sleep(100); }
  return false;
}
bool CaptureReleased(HWND window) {
  GUITHREADINFO info{sizeof(info)};
  return GetGUIThreadInfo(GetWindowThreadProcessId(window, nullptr), &info) && info.hwndCapture == nullptr;
}
void Mouse(HWND window, UINT message, POINT point) {
  SendMessageW(window, message, message == WM_LBUTTONUP ? 0 : MK_LBUTTON, MAKELPARAM(point.x, point.y));
}
POINT TabPoint(HWND window, size_t count, size_t index, bool close = false) {
  RECT rect{}; GetClientRect(window, &rect);
  const float scale = GetDpiForWindow(window) / 96.0f;
  const auto strip = jisu::LayoutTabStrip(rect.right / scale, count);
  const auto& r = close ? strip.closes[index] : strip.tabs[index];
  return POINT{static_cast<LONG>((r.left + r.right) * scale / 2), static_cast<LONG>(jisu::kTabCenterY * scale)};
}
void Click(HWND window, POINT point) { Mouse(window, WM_LBUTTONDOWN, point); Mouse(window, WM_LBUTTONUP, point); }
void Drag(HWND window, POINT from, POINT to) {
  Mouse(window, WM_LBUTTONDOWN, from);
  Mouse(window, WM_MOUSEMOVE, to);
  Mouse(window, WM_LBUTTONUP, to);
}
std::wstring Token(HWND window) {
  const auto title = Title(window);
  const auto end = title.find(L':', title.find(L':') + 1);
  return title.substr(0, end);
}
std::wstring Fixture(const std::filesystem::path& directory, char label) {
  const auto file = directory / (std::string(1, label) + ".html");
  std::ofstream html(file);
  html << "<!doctype html><meta charset=utf-8><input value='keep me'><script>"
          "const token='" << label << ":'+Math.random().toString(36).slice(2);let n=0;"
          "document.title=token+':'+n;setInterval(()=>document.title=token+':'+(++n),100);"
          "</script>";
  html.close();
  return L"file:///" + file.generic_wstring();
}
void Open(HWND window, const std::wstring& url) {
  COPYDATASTRUCT data{0x4A495355, static_cast<DWORD>((url.size() + 1) * sizeof(wchar_t)), const_cast<wchar_t*>(url.c_str())};
  SendMessageW(window, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data));
}
}
int wmain(int argc, wchar_t** argv) {
  if (argc != 2) return 2;
  wchar_t temp[MAX_PATH]{}; GetTempPathW(MAX_PATH, temp);
  const auto directory = std::filesystem::path(temp) / (L"jisu-tab-drag-" + std::to_wstring(GetCurrentProcessId()));
  std::filesystem::create_directories(directory);
  PROCESS_INFORMATION pi{};
  int result = 0;
  try {
    const auto a = Fixture(directory, 'A'), b = Fixture(directory, 'B'), c = Fixture(directory, 'C');
    std::wstring command = L"\"" + std::wstring(argv[1]) + L"\" --no-activate --no-agent --no-spare --bounds 50,50,1024,768 --udf \"" +
        (directory / L"profile").wstring() + L"\" \"" + a + L"\"";
    STARTUPINFOW si{sizeof(si)}; si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_SHOWNOACTIVATE;
    Require(CreateProcessW(argv[1], command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi) != FALSE, "start isolated browser");
    HWND source = nullptr;
    Require(Await([&] { auto windows = Windows(pi.dwProcessId); if (windows.size() == 1) source = windows.front(); return source && Title(source).starts_with(L"A:"); }), "first page ready");
    Open(source, b); Require(Await([&] { return Title(source).starts_with(L"B:"); }), "second page ready");
    Open(source, c); Require(Await([&] { return Title(source).starts_with(L"C:"); }), "third page ready");
    const auto first = TabPoint(source, 3, 0), second = TabPoint(source, 3, 1), third = TabPoint(source, 3, 2);
    Click(source, first);
    const auto token = Token(source);
    Require(token.starts_with(L"A:"), "select first tab");
    Drag(source, first, third);
    Require(Token(source) == token, "reorder preserves selected page");
    Click(source, first); Require(Title(source).starts_with(L"B:"), "left-to-right reorder");
    Click(source, second); Require(Title(source).starts_with(L"C:"), "middle tab order");
    Drag(source, third, first); Require(Token(source) == token, "right-to-left reorder");
    Mouse(source, WM_LBUTTONDOWN, first); Mouse(source, WM_MOUSEMOVE, third);
    SendMessageW(source, WM_KEYDOWN, VK_ESCAPE, 0); Mouse(source, WM_LBUTTONUP, third);
    Click(source, first); Require(Token(source) == token && CaptureReleased(source), "escape restores original order and releases capture");
    Mouse(source, WM_LBUTTONDOWN, first); Mouse(source, WM_MOUSEMOVE, third);
    SendMessageW(source, WM_CANCELMODE, 0, 0); Mouse(source, WM_LBUTTONUP, POINT{third.x, 250});
    Click(source, first); Require(Token(source) == token && Windows(pi.dwProcessId).size() == 1, "capture cancellation never tears out");
    Drag(source, first, POINT{first.x, 250});
    HWND detached = nullptr;
    Require(Await([&] {
      for (HWND w : Windows(pi.dwProcessId)) if (w != source && Token(w) == token) detached = w;
      return detached && Windows(pi.dwProcessId).size() == 2;
    }), "drag out creates a second native window without reload");
    Require(IsWindow(source) && Title(source).starts_with(L"B:"), "remaining tabs stay in source");
    const auto before = Title(detached); Sleep(350);
    Require(Title(detached) != before && Token(detached) == token, "migrated page events target new window");
    SendMessageW(source, WM_CLOSE, 0, 0);
    Require(Await([&] { return !IsWindow(source); }) && IsWindow(detached) && WaitForSingleObject(pi.hProcess, 0) == WAIT_TIMEOUT, "closing original does not exit detached window");
    const auto afterClose = Title(detached); Sleep(350);
    Require(Token(detached) == token && Title(detached) != afterClose, "page callbacks survive original-window destruction");
    Open(detached, b); Require(Await([&] { return Title(detached).starts_with(L"B:"); }), "new tabs work in detached window");
    Drag(detached, TabPoint(detached, 2, 1), POINT{100, 240});
    HWND secondDetached = nullptr;
    Require(Await([&] { for (HWND w : Windows(pi.dwProcessId)) if (w != detached && Title(w).starts_with(L"B:")) secondDetached = w; return secondDetached != nullptr; }), "detach again after original window closed");
    RECT oldBounds{}, newBounds{}; GetWindowRect(secondDetached, &oldBounds);
    Drag(secondDetached, TabPoint(secondDetached, 1, 0), POINT{350, 300}); GetWindowRect(secondDetached, &newBounds);
    Require(Windows(pi.dwProcessId).size() == 2 && (oldBounds.left != newBounds.left || oldBounds.top != newBounds.top), "dragging final tab moves window without duplicating page");
    Click(secondDetached, TabPoint(secondDetached, 1, 0, true));
    Require(!IsWindow(secondDetached) && IsWindow(detached), "close button still closes only its tab window");
    SendMessageW(detached, WM_CLOSE, 0, 0);
    Require(WaitForSingleObject(pi.hProcess, 10000) == WAIT_OBJECT_0, "last window closes process");
  } catch (const std::exception& e) { fprintf(stderr, "tab drag test: %s\n", e.what()); result = 1; }
  if (pi.hProcess) {
    for (HWND w : Windows(pi.dwProcessId)) PostMessageW(w, WM_CLOSE, 0, 0);
    if (WaitForSingleObject(pi.hProcess, 10000) != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 3);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
  }
  // WebView2 may hold cache handles briefly; leave only this isolated test profile if still busy.
  std::error_code error; std::filesystem::remove_all(directory, error);
  return result;
}
