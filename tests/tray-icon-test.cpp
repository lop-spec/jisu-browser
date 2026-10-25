#include "tray_icon.h"
#include <iostream>
#include <stdexcept>
using namespace jisu;
void require(bool ok) { if (!ok) throw std::runtime_error("Native tray gate failed"); }
int main() {
  const DWORD before = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
  for (int i = 0; i < 50; ++i) {
    HMENU menu = TrayIcon::CreateMenu(L"0.2.1 · test123");
    require(menu != nullptr && GetMenuItemCount(menu) == 6);
    require((GetMenuState(menu, 0, MF_BYPOSITION) & MF_GRAYED) != 0);
    require(GetMenuItemID(menu, 2) == TrayIcon::kOpen);
    require(GetMenuItemID(menu, 3) == TrayIcon::kNewTab);
    require(GetMenuItemID(menu, 5) == TrayIcon::kExit);
    wchar_t label[200]{};
    require(GetMenuStringW(menu, 5, label, ARRAYSIZE(label), MF_BYPOSITION) > 0);
    require(std::wstring(label) == L"彻底退出");
    require(GetMenuDefaultItem(menu, FALSE, 0) == TrayIcon::kOpen);
    DestroyMenu(menu);
  }
  require(TrayIcon::CallbackCommand(MAKELPARAM(NIN_SELECT, 1)) == TrayIcon::kOpen);
  require(TrayIcon::CallbackCommand(MAKELPARAM(NIN_KEYSELECT, 1)) == TrayIcon::kOpen);
  require(TrayIcon::CallbackCommand(MAKELPARAM(WM_LBUTTONDBLCLK, 1)) == TrayIcon::kOpen);
  require(TrayIcon::CallbackCommand(MAKELPARAM(NIN_SELECT, 2)) == 0);
  require(TrayIcon::CallbackCommand(MAKELPARAM(WM_CONTEXTMENU, 1)) == 0);
  { TrayIcon icon; icon.Remove(); icon.Remove(); require(!icon.Registered()); require(!icon.HandleExplorerRestart(WM_NULL)); }
  require(GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS) <= before + 1);
  std::cout << "PASS tray version/status; open/new/complete-exit menu; keyboard and click callbacks; unrelated icon rejected; idempotent delete; no USER handle leak\n";
}
