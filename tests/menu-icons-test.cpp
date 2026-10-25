#include "menu_icons.h"
#include <stdexcept>
#include <iostream>
void require(bool ok) { if (!ok) throw std::runtime_error("Native bookmark menu icon gate failed"); }
int wmain(int argc, wchar_t** argv) {
  require(argc == 2);
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  for (UINT dpi : {96u, 120u, 144u, 192u}) {
    const DWORD before = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    HMENU root = CreatePopupMenu(), sub = CreatePopupMenu();
    {
      jisu::BookmarkMenuIcons icons(dpi);
      require(icons.Append(root, MF_STRING, 10001, L"Cached website", argv[1], false));
      require(icons.Append(root, MF_STRING, 10002, L"Uncached website", L"", false));
      require(icons.Append(root, MF_STRING, 10003, L"Corrupt website image", L"missing.png", false));
      require(icons.Append(sub, MF_STRING, 10004, L"Nested website", argv[1], false));
      require(icons.Append(root, MF_POPUP, reinterpret_cast<UINT_PTR>(sub), L"Nested folder", L"", true));
      require(icons.SiteImages() == 2 && icons.FallbackImages() == 3);
      for (int i = 0; i < 4; ++i) {
        MENUITEMINFOW item{sizeof(item)}; item.fMask = MIIM_BITMAP | MIIM_ID | MIIM_SUBMENU;
        require(GetMenuItemInfoW(root, i, TRUE, &item) != FALSE);
        require(item.hbmpItem != nullptr);
        BITMAP bitmap{}; require(GetObjectW(item.hbmpItem, sizeof(bitmap), &bitmap) == sizeof(bitmap));
        require(bitmap.bmWidth == MulDiv(16, dpi, 96) && bitmap.bmBitsPixel == 32);
        const auto* pixel = static_cast<const BYTE*>(bitmap.bmBits);
        int visible = 0, transparent = 0, colorful = 0;
        for (int p = 0; p < bitmap.bmWidth * bitmap.bmHeight; ++p) {
          if (pixel[p*4+3] > 0) ++visible; else ++transparent;
          if (pixel[p*4] != pixel[p*4+2]) ++colorful;
          for (int c = 0; c < 3; ++c) require(pixel[p*4+c] <= pixel[p*4+3]);
        }
        require(visible > 0 && transparent > 0);
        if (i == 0) require(colorful > 0);
        if (i == 3) require(item.hSubMenu == sub);
        else require(item.wID == static_cast<UINT>(10001 + i));
      }
      DestroyMenu(root);
    }
    const DWORD after = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    require(after <= before + 1);
  }
  CoUninitialize();
  std::cout << "PASS cached PNG/ICO, missing-image globe, nested folder, command IDs, 100/125/150/200% DPI, alpha and GDI cleanup\n";
}
