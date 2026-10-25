// Shared toolbar/popup command contract. No credentials or extension storage are exposed here.
#pragma once
#include "common.h"
namespace jisu {
inline constexpr int kExtensionsButton = 6100;
inline constexpr int kExtensionButtonBase = 6200;
inline constexpr UINT kMsgCloseExtensionPopup = WM_APP + 4;
inline constexpr UINT kMsgOpenExtensionSettings = WM_APP + 5;
inline constexpr UINT kExtensionOpenBase = 45000;
inline constexpr UINT kExtensionPinBase = 45100;
inline constexpr UINT kExtensionSettingsBase = 45200;
inline constexpr UINT kExtensionManage = 45300;
}  // namespace jisu
