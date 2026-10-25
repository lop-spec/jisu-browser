// 浏览器主窗口：原生标签条 + 地址栏 + 书签栏，每个标签一个 WebView2 控件，另预建一个隐藏的备用控件。
// 智能体标签（Pi 的 browser 工具经 AgentBridge 驱动）也在标签条里，平时放在屏幕外宿主窗口，用户点开时移进主窗口。
#pragma once

#include "agent_bridge.h"
#include "bookmarks.h"
#include "common.h"
#include "graphics.h"
#include "favicons.h"
#include "fullscreen_window.h"
#include "history.h"
#include "media_session.h"
#include "options.h"
#include "palette.h"
#include "tray_icon.h"

#include <WebView2.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace jisu {

class BrowserWindow : public AgentHost {
 public:
  BrowserWindow(const Options& options, GraphicsWarmup& warmup);
  ~BrowserWindow() override;
  BrowserWindow(const BrowserWindow&) = delete;
  BrowserWindow& operator=(const BrowserWindow&) = delete;

  bool Create(HINSTANCE instance);    // 建窗（暂不显示）与地址栏；后台读书签与设置
  void ShowFirstFrame(int showCmd);   // 建渲染目标，显示窗口并同步画出首帧
  void StartEnvironment();            // 异步创建 WebView2 环境，不阻塞首帧
  bool PreTranslate(const MSG& msg);  // 地址栏回车/Esc 与外壳快捷键

  // AgentHost
  void LeaseAgentTab(LeaseCallback done) override;
  void ReturnAgentTab(uint32_t tabId) override;
  void SetAgentTabAwake(uint32_t tabId, bool awake) override;
  void ResumeAgentTab(uint32_t tabId) override;
  bool IsAgentTabViewed(uint32_t tabId) override;
  std::string AgentTabInfo(uint32_t tabId) override;

  static constexpr ULONG_PTR kCopyDataOpenUrl = 0x4A495355;  // 'JISU'：另一个实例转来的网址
  // 同一用户数据目录已经开着的主窗口（没有返回 nullptr）
  static HWND FindRunningWindow(const std::wstring& profileDir);

 private:
  struct Tab {
    uint32_t id = 0;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> webview;
    std::wstring title;
    std::wstring url;
    std::wstring pendingUrl;  // 控件建好后要打开的网址；空 = 新标签页
    bool creating = false;
    bool retried = false;     // 控件创建失败后已重试过一次
    bool visible = true;      // 控件新建时默认可见
    bool loading = false;
    bool canBack = false;
    bool canForward = false;
    std::unique_ptr<MediaSession> media;
    HWND mediaSurface = nullptr;
    uint32_t mediaSource = 0;
    uint32_t mediaTarget = 0;
    std::string mediaPage;
    std::string mediaOffer;
    std::string mediaStatus;
    ULONGLONG mediaSaved = 0;
    bool mediaIntegration = false;
    bool mediaAuto = false;
    HWND parent = nullptr;    // 控件当前的父窗口：主窗口或屏幕外宿主
    // 智能体标签
    bool agent = false;
    bool leased = false;
    bool agentAwake = false;
    bool suspended = false;     // 已 TrySuspend
    bool suspending = false;    // TrySuspend 进行中
    ULONGLONG hiddenSince = 0;  // 智能体标签在宿主里隐藏的起点（挂起计时）
    LeaseCallback leaseDone;  // 新建的智能体标签首次导航完成后交给桥
    // window.open 等新窗口请求：控件建好后交给它
    ComPtr<ICoreWebView2NewWindowRequestedEventArgs> newWindowArgs;
    ComPtr<ICoreWebView2Deferral> newWindowDeferral;
  };

  struct Settings {
    bool bookmarkBar = true;
    bool importedDefaultBrowser = false;
    int maxRegularTabs = 10;
    int64_t bookmarkFolder = 1;
  };

  static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
  static LRESULT CALLBACK AgentHostProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
  static DWORD WINAPI ProfileThreadMain(void* param);
  LRESULT WndProc(UINT msg, WPARAM wParam, LPARAM lParam);
  void OnCreate();
  void OnDestroy();
  void InstallTrayIcon();
  void WriteTrayState();
  void ExecuteTrayCommand(UINT command);
  void ApplyWindowTheme();
  void OnThemeChanged();
  void UpdateEditFont();
  void Relayout();
  void OnContentFullscreenChanged(uint32_t id);
  void UpdateFullscreen();
  void ExitContentFullscreen();
  void ExitFullscreen();
  void LayoutBookmarkBar();
  RECT ContentRect() const;
  ChromeState BuildChromeState() const;
  ChromeHit HitTest(POINT point) const;
  void InvalidateChrome();
  void OnMouseMove(POINT point);
  void OnLButtonDown(POINT point);
  void OnLButtonUp(POINT point);
  void OnMButtonUp(POINT point);
  void OnRButtonUp(POINT point);

  void OnEnvironmentCreated(HRESULT result, ICoreWebView2Environment* environment);
  void StartAgentBridge();
  void CreateController(Tab& tab);
  void OnControllerCreated(uint32_t id, HRESULT result, ICoreWebView2Controller* controller);
  void OnControllerFailed(Tab& tab, HRESULT result);
  void AbandonNewWindow(Tab& tab);
  void ConfigureWebView(Tab& tab);
  void ConfigureProfile(Tab& tab);
  void InstallNextExtension();
  void PruneUnlistedExtensions();
  void RemoveNextUnlistedExtension();
  void EnforceTabLimit();
  bool HandleTabLimitMessage(Tab& tab, const std::wstring& source, const std::string& text);
  void SaveExtensionStatus();
  void OpenExtensions();
  void OpenHistory();
  void RecordVisit(Tab& tab);
  bool HandleHistoryMessage(Tab& tab, const std::string& text);
  void OpenMedia();
  bool EnsureMedia(Tab& tab);
  void CaptureMedia(Tab& source);
  void StartMedia(uint32_t source, const std::string& offer);
  bool HandleMediaMessage(Tab& tab, const std::wstring& origin, const std::string& text);
  void PollMedia();
  void PostMediaShortcut(const std::string& shortcut);
  void SourceMediaAction(Tab& player, const std::string& action, int line = -1);
  void StopMedia(Tab& tab);
  void RefreshExtensionToolbar();
  float LayoutExtensionToolbar(float right, float top, float scale);
  void DrawExtensionButton(const DRAWITEMSTRUCT& draw);
  bool HandleExtensionCommand(UINT command);
  void SetExtensionPinned(size_t index, bool pinned);
  void ShowExtensionsMenu();
  void ShowExtensionPopup(size_t index);
  void CloseExtensionPopup();
  void WriteExtensionToolbarState();
  static LRESULT CALLBACK ExtensionPopupProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
  void NavigateNewTabPage(Tab& tab);
  void OnWebMessage(uint32_t id, ICoreWebView2WebMessageReceivedEventArgs* args);
  void OnPageMessage(Tab& tab, const std::string& json);
  void PostToPage(Tab& tab, const std::string& json);
  void OpenNewWindowAsTab(ICoreWebView2NewWindowRequestedEventArgs* args, bool activate);
  void CreateSpare();

  void NewTab(const std::wstring& url, bool activate = true);
  void CloseTab(size_t index);
  void Activate(size_t index);
  void PlaceTab(Tab& tab);
  void PlaceAllTabs();
  void ApplyAgentScale(Tab& tab, bool inHost);
  void ResumeTab(Tab& tab);
  void SuspendIdleAgentTabs();
  void SetTabVisible(Tab& tab, bool visible);
  Tab* FindTab(uint32_t id);
  size_t IndexOf(const Tab& tab) const;
  Tab* ActiveTab();
  const Tab* ActiveTab() const;
  bool IsActive(const Tab& tab) const;
  bool IsSpare(const Tab& tab) const;
  HWND EnsureAgentHost();
  RECT AgentHostRect() const;
  SIZE AgentViewport() const;   // 智能体标签的 CSS 视口
  SIZE AgentHostSize() const;   // 宿主窗口物理像素 = CSS 视口 × 光栅化比例

  bool DispatchShortcut(UINT key, bool repeat);
  void ExecuteCommand(WPARAM command);
  void FocusAddressBar();
  void UpdateAddressBar();
  void UpdateWindowTitle();
  void NavigateFromAddressBar();
  void OpenUrl(const std::wstring& url, bool newTab);

  // 书签
  void ToggleBookmark(bool fromKeyboard);
  void EditBookmark(BookmarkNode* node, const std::wstring& title, const std::wstring& url);
  void CancelBookmarkDrag();
  void UpdateBookmarkDrop(POINT point);
  void FinishBookmarkDrag(POINT point);
  void ShowBookmarkFolderMenu(BookmarkNode& folder, POINT screen);
  void ShowBookmarkContextMenu(BookmarkNode& node, POINT screen);
  void ShowOverflowMenu(POINT screen);
  void ShowBookmarkMenu(const std::vector<BookmarkNode*>& nodes, BookmarkNode* openAllFolder, POINT screen,
                        bool alignRight);
  void ShowMainMenu(POINT screen);
  void OpenBookmarkManager(const std::wstring& fragment);
  void OnBookmarksChanged();
  void BroadcastBookmarks();
  void LoadSettings();
  void SaveSettings();
  void RegisterAsDefaultBrowser();
  void ShowAbout();
  void OnCopyData(const COPYDATASTRUCT& data);

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
  HWND agentHost_ = nullptr;
  TrayIcon tray_;
  UINT trayInstallAttempts_ = 0;
  HFONT editFont_ = nullptr;
  HBRUSH omniboxBrush_ = nullptr;
  int editHeightPx_ = 20;
  UINT dpi_ = 96;
  FullscreenWindow fullscreen_;
  bool browserFullscreen_ = false;
  uint32_t fullscreenTab_ = 0;

  ComPtr<ICoreWebView2Environment> environment_;
  std::vector<std::unique_ptr<Tab>> tabs_;
  std::unique_ptr<Tab> spare_;
  size_t active_ = 0;
  uint32_t nextTabId_ = 1;
  uint32_t firstTabId_ = 0;
  std::wstring inlineNtp_;
  std::wstring message_;
  std::unique_ptr<AgentBridge> bridge_;
  AgentBridgeConfig agentConfig_;  // 智能体标签的省电设置（桥启用时从配置读入）
  bool freezeTimerRunning_ = false;

  // 书签与设置：Create 时起后台线程读，首帧前等它读完
  BookmarkStore bookmarks_;
  HistoryStore history_;
  mutable FaviconCache favicons_;
  bool profileConfigured_ = false;
  struct ExtensionEntry {
    std::wstring path, name, id, popup, options;
    bool pinned = true;
    HWND button = nullptr;
    HICON toolbarIcon = nullptr;
    bool enabled = false;
    bool nativeTabLimit = false;
    HRESULT result = E_PENDING;
    ComPtr<ICoreWebView2BrowserExtension> handle;
  };
  std::vector<ExtensionEntry> extensions_;
  HWND extensionsButton_ = nullptr;
  HWND extensionTooltip_ = nullptr;
  HWND extensionPopupWindow_ = nullptr;
  std::unique_ptr<Tab> extensionPopup_;
  size_t popupExtensionIndex_ = static_cast<size_t>(-1);
  uint64_t extensionPopupGeneration_ = 0;
  bool extensionPopupLoaded_ = false;
  size_t extensionIndex_ = 0;
  bool removeUnlistedExtensions_ = false;
  bool extensionPruneStarted_ = false;
  std::vector<ComPtr<ICoreWebView2BrowserExtension>> unlistedExtensions_;
  size_t removedExtensionCount_ = 0;
  ComPtr<ICoreWebView2Profile7> extensionProfile_;
  std::shared_ptr<int> alive_ = std::make_shared<int>(0);
  Settings settings_;
  HANDLE profileThread_ = nullptr;
  std::wstring profileDir_;     // 用户数据目录（书签、设置文件所在）
  bool profileLoaded_ = false;
  std::vector<BookmarkNode*> barItems_;  // 书签栏上排出来的项（与 layout_.bookmarks 对应）
  std::vector<BookmarkNode*> overflowItems_;

  ChromeHit hover_;
  ChromeHit pressed_;
  int64_t dragBookmarkId_ = 0;
  POINT dragStart_{};
  bool draggingBookmark_ = false;
  int dropBookmark_ = -1;
  bool dropIntoFolder_ = false;
  int64_t dropParentId_ = 0;
  size_t dropIndex_ = 0;
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
