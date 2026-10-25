#include "tray_icon.h"
#include <strsafe.h>
#include <windowsx.h>

namespace jisu {
TrayIcon::~TrayIcon() { Remove(); }
NOTIFYICONDATAW TrayIcon::Data() const {
  NOTIFYICONDATAW data{};
  data.cbSize = sizeof(data);
  data.hWnd = owner_;
  data.uID = kIconId;
  data.guidItem = kGuid;
  data.uFlags = NIF_GUID;
  return data;
}
bool TrayIcon::Install(HWND owner, HINSTANCE instance, const std::wstring& version) {
  owner_ = owner;
  version_ = version;
  taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
  if (!icon_) icon_ = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON,
      GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
  if (!icon_) return false;
  auto data = Data();
  data.uFlags |= NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
  data.uCallbackMessage = kCallback;
  data.hIcon = icon_;
  StringCchCopyW(data.szTip, ARRAYSIZE(data.szTip), (L"极速浏览器 " + version_ + L" · 运行中\n右键 → 彻底退出").c_str());
  if (registered_ && Shell_NotifyIconW(NIM_MODIFY, &data)) return true;
  registered_ = Shell_NotifyIconW(NIM_ADD, &data) != FALSE;
  if (registered_) {
    data.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &data);
  }
  return registered_;
}
void TrayIcon::SetExiting() {
  if (!registered_) return;
  auto data = Data();
  data.uFlags |= NIF_TIP | NIF_SHOWTIP;
  StringCchCopyW(data.szTip, ARRAYSIZE(data.szTip), L"极速浏览器 · 正在退出");
  Shell_NotifyIconW(NIM_MODIFY, &data);
}
void TrayIcon::Remove() {
  if (registered_) {
    auto data = Data();
    Shell_NotifyIconW(NIM_DELETE, &data);
    registered_ = false;
  }
  if (icon_) { DestroyIcon(icon_); icon_ = nullptr; }
}
bool TrayIcon::HandleExplorerRestart(UINT message) {
  if (!taskbarCreated_ || message != taskbarCreated_) return false;
  registered_ = false;
  return true;
}
HMENU TrayIcon::CreateMenu(const std::wstring& version) {
  HMENU menu = CreatePopupMenu();
  if (!menu) return nullptr;
  AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, (L"极速浏览器 " + version + L" · 运行中").c_str());
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, kOpen, L"打开浏览器");
  AppendMenuW(menu, MF_STRING, kNewTab, L"新建标签页");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, kExit, L"彻底退出");
  SetMenuDefaultItem(menu, kOpen, FALSE);
  return menu;
}
UINT TrayIcon::CallbackCommand(LPARAM lParam) {
  if (HIWORD(lParam) != kIconId) return 0;
  const UINT event = LOWORD(lParam);
  return event == NIN_SELECT || event == NIN_KEYSELECT || event == WM_LBUTTONUP || event == WM_LBUTTONDBLCLK ? kOpen : 0;
}
UINT TrayIcon::HandleCallback(WPARAM wParam, LPARAM lParam) {
  if (const UINT command = CallbackCommand(lParam)) return command;
  if (HIWORD(lParam) != kIconId || LOWORD(lParam) != WM_CONTEXTMENU) return 0;
  POINT point{GET_X_LPARAM(wParam), GET_Y_LPARAM(wParam)};
  if (point.x == -1 && point.y == -1) GetCursorPos(&point);
  HMENU menu = CreateMenu(version_);
  if (!menu) return 0;
  // This callback is a real tray click/keyboard action, not a background startup.
  SetForegroundWindow(owner_);
  const UINT command = static_cast<UINT>(TrackPopupMenuEx(menu,
      TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON | TPM_RIGHTALIGN | TPM_BOTTOMALIGN,
      point.x, point.y, owner_, nullptr));
  DestroyMenu(menu);
  PostMessageW(owner_, WM_NULL, 0, 0);
  return command;
}
}  // namespace jisu
