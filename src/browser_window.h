// 浏览器主窗口：原生标签条 + 地址栏，每个标签一个 WebView2 控件，另预建一个隐藏的备用控件。
#pragma once

#include "common.h"
#include "graphics.h"
#include "options.h"
#include "palette.h"

#include <WebView2.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace jisu {

class BrowserWindow {
 public:
  BrowserWindow(const Options& options, GraphicsWarmup& warmup);
  ~BrowserWindow();
  BrowserWindow(const BrowserWindow&) = delete;
  BrowserWindow& operator=(const BrowserWindow&) = delete;

  bool Create(HINSTANCE instance);    // 建窗（暂不显示）与地址栏
  void ShowFirstFrame(int showCmd);   // 建渲染目标，显示窗口并同步画出首帧
  void StartEnvironment();            // 异步创建 WebView2 环境，不阻塞首帧
  bool PreTranslate(const MSG& msg);  // 地址栏回车/Esc 与外壳快捷键

 private:
  struct Tab {
    uint32_t id = 0;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> webview;
    std::wstring title;
    std::wstring url;
    std::wstring pendingUrl;  // 控件建好后要打开的网址；空 = 新标签页
    bool creating = false;
    bool visible = true;      // 控件新建时默认可见
    bool loading = false;
    bool canBack = false;
    bool canForward = false;
    // window.open 等新窗口请求：控件建好后交给它
    ComPtr<ICoreWebView2NewWindowRequestedEventArgs> newWindowArgs;
    ComPtr<ICoreWebView2Deferral> newWindowDeferral;
  };

  static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
  LRESULT WndProc(UINT msg, WPARAM wParam, LPARAM lParam);
  void OnCreate();
  void OnDestroy();
  void ApplyWindowTheme();
  void OnThemeChanged();
  void UpdateEditFont();
  void Relayout();
  RECT ContentRect() const;
  ChromeState BuildChromeState() const;
  ChromeHit HitTest(POINT point) const;
  void InvalidateChrome();
  void OnMouseMove(POINT point);
  void OnLButtonDown(POINT point);
  void OnLButtonUp(POINT point);
  void OnMButtonUp(POINT point);

  void OnEnvironmentCreated(HRESULT result, ICoreWebView2Environment* environment);
  void CreateController(Tab& tab);
  void OnControllerCreated(uint32_t id, HRESULT result, ICoreWebView2Controller* controller);
  void ConfigureWebView(Tab& tab);
  void NavigateNewTabPage(Tab& tab);
  void OnWebMessage(uint32_t id, ICoreWebView2WebMessageReceivedEventArgs* args);
  void OpenNewWindowAsTab(ICoreWebView2NewWindowRequestedEventArgs* args);
  void CreateSpare();

  void NewTab(const std::wstring& url);
  void CloseTab(size_t index);
  void Activate(size_t index);
  void SetTabVisible(Tab& tab, bool visible);
  Tab* FindTab(uint32_t id);
  Tab* ActiveTab();
  const Tab* ActiveTab() const;
  bool IsActive(const Tab& tab) const;
  bool IsSpare(const Tab& tab) const;

  bool DispatchShortcut(UINT key, bool repeat);
  void ExecuteCommand(WPARAM command);
  void FocusAddressBar();
  void UpdateAddressBar();
  void UpdateWindowTitle();
  void NavigateFromAddressBar();

  void MaybeWriteMetrics();
  void WriteMetrics(bool forced);

  const Options& options_;
  GraphicsWarmup& warmup_;
  Palette palette_;
  ChromePainter painter_;
  ChromeLayout layout_;
  HINSTANCE instance_ = nullptr;
  HWND hwnd_ = nullptr;
  HWND edit_ = nullptr;
  HFONT editFont_ = nullptr;
  HBRUSH omniboxBrush_ = nullptr;
  int editHeightPx_ = 20;
  UINT dpi_ = 96;

  ComPtr<ICoreWebView2Environment> environment_;
  std::vector<std::unique_ptr<Tab>> tabs_;
  std::unique_ptr<Tab> spare_;
  size_t active_ = 0;
  uint32_t nextTabId_ = 1;
  uint32_t firstTabId_ = 0;
  std::wstring inlineNtp_;
  std::wstring message_;

  ChromeHit hover_;
  ChromeHit pressed_;
  bool trackingMouse_ = false;
  bool omniboxFocused_ = false;
  bool focusInWebView_ = false;
  bool editing_ = false;          // 用户正在地址栏里输入，网页地址变化时不覆盖
  bool settingAddress_ = false;   // 程序在改地址栏文字（EN_CHANGE 不算用户输入）
  bool destroyed_ = false;
  bool metricsWritten_ = false;
  double newTabRequestMs_ = 0;    // 第一次 Ctrl+T/点 + 的时刻，用来量备用控件换上的耗时
  uint32_t newTabMeasureId_ = 0;
};

}  // namespace jisu
