// 极速浏览器 Windows 外壳入口。
// 启动顺序：后台线程预热图形对象 → 建窗（含地址栏）→ 发起 WebView2 环境 → 画首帧 → 消息循环；
// 环境就绪后建首个控件打开新标签页，新标签页画出来后再预建一个隐藏的备用控件。
#include "browser_window.h"
#include "common.h"
#include "graphics.h"
#include "metrics.h"
#include "options.h"

#include <shobjidl.h>
#include <cstdint>
#include <cstdio>
#include <cwctype>

namespace {

enum class Forward { kNone, kDone, kHung };

// 同一用户数据目录只开一个主实例：后起的把网址交给已开的窗口后退出（双击图标、别的程序打开链接都走这里）
Forward ForwardToRunningInstance(const jisu::Options& options) {
  using jisu::BrowserWindow;
  if (!options.metricsPath.empty() || options.preheatSeconds > 0) {
    return Forward::kNone;  // 测量与预热运行各自独立
  }
  uint32_t hash = 2166136261u;
  for (const wchar_t c : options.InstanceKey()) {
    hash ^= static_cast<uint32_t>(towlower(c));
    hash *= 16777619u;
  }
  wchar_t name[64];
  swprintf_s(name, L"Local\\JisuBrowser-%08X", hash);
  // 句柄留到进程退出：主实例靠它让后来者知道自己在
  const HANDLE mutex = CreateMutexW(nullptr, FALSE, name);
  if (!mutex || GetLastError() != ERROR_ALREADY_EXISTS) {
    return Forward::kNone;
  }
  // 主实例冷启动可能还没建好窗口（慢盘、开机时）：最多等 10 秒，期间不另起一个实例
  for (int attempt = 0; attempt < 100; ++attempt) {
    const HWND target = BrowserWindow::FindRunningWindow(options.InstanceKey());
    if (target) {
      DWORD pid = 0;
      GetWindowThreadProcessId(target, &pid);
      if (!options.noActivate) AllowSetForegroundWindow(pid);
      // 日常窗口的「不激活」转发：有网址就开在后台标签，没网址只是确认它在运行，不新开空白页也不抢焦点
      const bool passive = options.noActivate && !options.appMode;
      COPYDATASTRUCT data{};
      data.dwData = options.appMode ? (options.noActivate ? BrowserWindow::kCopyDataOpenUrl + 1 : BrowserWindow::kCopyDataOpenUrl)
                                    : (passive ? BrowserWindow::kCopyDataPassive : BrowserWindow::kCopyDataOpenUrl);
      data.cbData = static_cast<DWORD>((options.startUrl.size() + 1) * sizeof(wchar_t));
      data.lpData = const_cast<wchar_t*>(options.startUrl.c_str());
      DWORD_PTR result = 0;
      if (SendMessageTimeoutW(target, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data), SMTO_ABORTIFHUNG, 5000,
                              &result)) {
        if (passive && result != BrowserWindow::kCopyDataPassiveAck && !options.startUrl.empty()) {
          // 升级前的旧实例不认「不激活」转发：退回普通转发，网址不丢
          data.dwData = BrowserWindow::kCopyDataOpenUrl;
          SendMessageTimeoutW(target, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data), SMTO_ABORTIFHUNG, 5000, &result);
        }
        return Forward::kDone;
      }
      // 窗口还在却不响应：主实例卡住了。窗口已经没了（正在退出）就接着等
      if (IsWindow(target)) {
        return Forward::kHung;
      }
    }
    Sleep(100);
  }
  return Forward::kNone;  // 有互斥体但始终没有窗口：上一个实例已在退出，自己正常启动
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCmd) {
  using namespace jisu;
  ProcessClock::Init();
  Metrics::Get().Mark("mainEnter");
  Options options = ParseOptions(GetCommandLineW());
  switch (ForwardToRunningInstance(options)) {
    case Forward::kDone:
      return 0;
    case Forward::kHung:
      // 主实例卡住时仍给用户一个能用的窗口，但不开智能体桥、不装托盘、不写会话：两边不抢 broker 邀请和会话文件
      options.secondary = true;
      options.noAgent = true;
      break;
    case Forward::kNone:
      break;
  }
  if (options.appMode) {
    const std::wstring identity = L"JisuBrowser.App." + options.appId;
    SetCurrentProcessExplicitAppUserModelID(identity.c_str());
  }
  const bool preheat = options.preheatSeconds > 0;

  GraphicsWarmup warmup;
  if (!preheat) {
    warmup.Start(options);
  }

  if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) {
    return 1;
  }
  Metrics::Get().Mark("comReady");

  int exitCode = 0;
  {
    BrowserWindow window(options, warmup);
    if (window.Create(instance)) {
      Metrics::Get().Mark("windowCreated");
      // 默认在首帧之前发起环境：它在 UI 线程上的同步部分与后台图形预热重叠
      const bool environmentFirst = preheat || options.envStart == EnvStart::kEarly;
      if (environmentFirst) {
        window.StartEnvironment();
      }
      if (!preheat) {
        window.ShowFirstFrame(showCmd);
      }
      if (!environmentFirst) {
        window.StartEnvironment();
      }

      MSG msg{};
      while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!window.PreTranslate(msg)) {
          TranslateMessage(&msg);
          DispatchMessageW(&msg);
        }
      }
      exitCode = static_cast<int>(msg.wParam);
    } else {
      exitCode = 2;
    }
  }
  CoUninitialize();
  return exitCode;
}
