#pragma once

#include <windows.h>

namespace jisu {
// A permission/dialog window owned by the browser is part of the same interaction.
// Destroying its WebView while the owner is disabled can strand the owner disabled.
inline bool ShouldCloseExtensionPopupOnDeactivate(HWND owner, HWND popup, HWND next) {
  if (!owner || !popup || !IsWindowEnabled(owner)) return false;
  if (!next) return true;
  if (next == owner || next == popup) return false;
  return GetAncestor(next, GA_ROOTOWNER) != owner;
}

inline bool MayProcessExtensionPopupClose(HWND owner) {
  return owner && IsWindowEnabled(owner);
}
}  // namespace jisu
