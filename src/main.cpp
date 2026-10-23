// 极速浏览器 Windows 外壳入口。
// 启动顺序：后台线程预热图形对象 → 建窗（含地址栏）→ 发起 WebView2 环境 → 画首帧 → 消息循环；
// 环境就绪后建首个控件打开新标签页，新标签页画出来后再预建一个隐藏的备用控件。
#include "browser_window.h"
#include "common.h"
#include "graphics.h"
#include "metrics.h"
#include "options.h"

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCmd) {
  using namespace jisu;
  ProcessClock::Init();
  Metrics::Get().Mark("mainEnter");
  const Options options = ParseOptions(GetCommandLineW());
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
