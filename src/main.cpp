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

// 同一用户数据目录只开一个主实例：后起的把网址交给已开的窗口后退出（双击图标、别的程序打开链接都走这里）
bool ForwardToRunningInstance(const jisu::Options& options) {
  using jisu::BrowserWindow;
  if (!options.metricsPath.empty() || options.preheatSeconds > 0) {
    return false;  // 测量与预热运行各自独立
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
    return false;
  }
  for (int attempt = 0; attempt < 30; ++attempt) {  // 主实例可能还在建窗：最多等 3 秒
    const HWND target = BrowserWindow::FindRunningWindow(options.InstanceKey());
    if (target) {
      DWORD pid = 0;
      GetWindowThreadProcessId(target, &pid);
      if (!options.noActivate) AllowSetForegroundWindow(pid);
      COPYDATASTRUCT data{};
      data.dwData = options.appMode && options.noActivate ? BrowserWindow::kCopyDataOpenUrl + 1 : BrowserWindow::kCopyDataOpenUrl;
      data.cbData = static_cast<DWORD>((options.startUrl.size() + 1) * sizeof(wchar_t));
      data.lpData = const_cast<wchar_t*>(options.startUrl.c_str());
      DWORD_PTR result = 0;
      if (SendMessageTimeoutW(target, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data), SMTO_ABORTIFHUNG, 5000,
                              &result)) {
        return true;
      }
      break;
    }
    Sleep(100);
  }
  return false;  // 找不到能用的窗口：自己正常启动
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCmd) {
  using namespace jisu;
  ProcessClock::Init();
  Metrics::Get().Mark("mainEnter");
  const Options options = ParseOptions(GetCommandLineW());
  if (ForwardToRunningInstance(options)) {
    return 0;
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
