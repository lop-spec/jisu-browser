#include "extension_popup_focus.h"

#include <cstdio>

int main() {
  // Hidden Win32 windows exercise real owner/child relationships without creating
  // a browser, activating a window, or accessing a profile or extension storage.
  const HINSTANCE instance = GetModuleHandleW(nullptr);
  auto make = [instance](HWND owner) {
    return CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, L"STATIC", L"",
                           WS_POPUP, 0, 0, 40, 40, owner, nullptr, instance, nullptr);
  };
  HWND browser = make(nullptr);
  HWND popup = make(browser);
  HWND dialog = make(popup);
  HWND browserDialog = make(browser);
  HWND other = make(nullptr);
  if (!browser || !popup || !dialog || !browserDialog || !other) {
    std::fprintf(stderr, "could not create hidden test windows\n");
    return 1;
  }
  int failures = 0;
  auto check = [&failures](bool condition, const char* message) {
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
  };
  using jisu::ShouldCloseExtensionPopupOnDeactivate;
  check(!ShouldCloseExtensionPopupOnDeactivate(browser, popup, browser), "owner focus must not close popup");
  check(!ShouldCloseExtensionPopupOnDeactivate(browser, popup, popup), "popup focus must not close popup");
  check(!ShouldCloseExtensionPopupOnDeactivate(browser, popup, dialog), "popup-owned dialog must stay open");
  check(!ShouldCloseExtensionPopupOnDeactivate(browser, popup, browserDialog), "browser-owned dialog must stay open");
  check(ShouldCloseExtensionPopupOnDeactivate(browser, popup, other), "external window must close popup");
  check(ShouldCloseExtensionPopupOnDeactivate(browser, popup, nullptr), "lost focus must close popup");
  check(jisu::MayProcessExtensionPopupClose(browser), "enabled owner may process close");
  EnableWindow(browser, FALSE);
  check(!ShouldCloseExtensionPopupOnDeactivate(browser, popup, other), "modal owner must retain popup");
  check(!jisu::MayProcessExtensionPopupClose(browser), "queued close must not destroy a modal popup");
  EnableWindow(browser, TRUE);
  check(jisu::MayProcessExtensionPopupClose(browser), "owner must process close again after modal exits");
  DestroyWindow(other);
  DestroyWindow(browserDialog);
  DestroyWindow(dialog);
  DestroyWindow(popup);
  DestroyWindow(browser);
  if (!failures) std::puts("PASS extension popup ownership and modal focus lifecycle");
  return failures ? 1 : 0;
}
