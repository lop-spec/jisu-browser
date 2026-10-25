// Native notification-area icon. Owned by the browser process, never a helper.
#pragma once
#include "common.h"
#include <shellapi.h>

namespace jisu {
class TrayIcon {
 public:
  static constexpr UINT kCallback = WM_APP + 40;
  static constexpr UINT kOpen = 42010;
  static constexpr UINT kNewTab = 42011;
  static constexpr UINT kExit = 42012;
  static constexpr UINT kIconId = 1;
  inline static constexpr GUID kGuid{0x7be56475,0x94d3,0x44cd,{0xb8,0x63,0x52,0x71,0x65,0xa1,0xda,0x77}};
  ~TrayIcon();
  bool Install(HWND owner, HINSTANCE instance, const std::wstring& version);
  void Remove();
  void SetExiting();
  bool HandleExplorerRestart(UINT message);
  UINT HandleCallback(WPARAM wParam, LPARAM lParam);
  bool Registered() const { return registered_; }
  static HMENU CreateMenu(const std::wstring& version);
  static UINT CallbackCommand(LPARAM lParam);
 private:
  NOTIFYICONDATAW Data() const;
  HWND owner_ = nullptr;
  HICON icon_ = nullptr;
  std::wstring version_;
  UINT taskbarCreated_ = 0;
  bool registered_ = false;
};
}  // namespace jisu
