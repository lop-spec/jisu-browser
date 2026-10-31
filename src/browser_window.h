// 浏览器主窗口：原生标签条 + 地址栏 + 书签栏，每个标签一个 WebView2 控件，另预建一个隐藏的备用控件。
// 智能体标签（Pi 的 browser 工具经 AgentBridge 驱动）也在标签条里，平时放在屏幕外宿主窗口，用户点开时移进主窗口。
#pragma once

#include "agent_bridge.h"
#include "bookmarks.h"
#include "common.h"
#include "graphics.h"
#include "favicons.h"
#include "file_writer.h"
#include "fullscreen_window.h"
#include "history.h"
#include "media_session.h"
#include "menu_icons.h"
#include "options.h"
#include "palette.h"
#include "tray_icon.h"

#include <WebView2.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace jisu {

class BrowserWindow : public AgentHost {
 public:
  BrowserWindow(const Options& options, GraphicsWarmup& warmup, BrowserWindow* root = nullptr);
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
  // 不激活的转发：有网址开在后台标签，没网址只是确认在运行（不新开空白页、不抢焦点、不闪任务栏）
  static constexpr ULONG_PTR kCopyDataPassive = kCopyDataOpenUrl + 2;
  static constexpr LRESULT kCopyDataPassiveAck = 0x4A53;  // 认得「不激活」转发的版本回这个值；旧版回 TRUE
  // 同一用户数据目录已经开着的主窗口（没有返回 nullptr）
  static HWND FindRunningWindow(const std::wstring& profileDir);

 private:
  struct Tab {
    // Stable indirection follows the live WebView when it moves between windows.
    std::shared_ptr<BrowserWindow*> owner = std::make_shared<BrowserWindow*>(nullptr);
    ~Tab() { *owner = nullptr; }
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
    uint32_t openerId = 0;  // 发起请求的标签，弹出页首次导航完成前有效：它先关掉的话请求要先放弃，内核在 put_NewWindow 里会崩
    uint32_t openedBy = 0;  // 打开它的标签（一直保留）：新链接标签排在打开者右侧
    bool scriptOpened = false;  // window.open 打开的：页面自己 window.close() 时可以关掉
    bool fromAgent = false;     // 智能体页面弹出的：不计入、也不被「自动关旧标签」关掉
    bool pinned = false;        // 固定：不被「自动关旧标签」关掉
    bool lazy = false;          // 恢复出来、还没看过的后台标签：点到它才建网页控件
    bool audible = false;       // 正在出声
    bool muted = false;
    bool allowInternal = false; // 下一次导航到内部页面由外壳发起（网页自己不能跳到书签、历史、扩展管理）
    bool faviconHost = false;   // 当前映射了图标缓存虚拟主机（只在书签管理页期间）
    ULONGLONG crashedAt = 0;    // 渲染进程上次崩溃的时刻：一分钟内再崩就不再自动重载
    bool crashed = false;       // 后台时崩溃：切过来时重载
    std::wstring error;         // 控件建不起来的原因，只在这个标签显示
    std::wstring agentScript;   // 智能体「禁自动播放」脚本：用户接管这个标签时移除
    std::vector<std::wstring> blockedPopups;  // 当前页面没经用户操作就要弹出的窗口（已拦截）
    bool focusOnCreate = false;  // 切到它时控件还没建好：建好后把键盘焦点给网页
  };

  struct Settings {
    bool bookmarkBar = true;
    bool importedDefaultBrowser = false;
    int maxRegularTabs = 10;
    int64_t bookmarkFolder = 1;
    std::vector<std::wstring> popupAllow;  // 允许不经用户操作弹出窗口的站点（源站）
    bool hasWindow = false;                // 上次关窗时的位置（工作区坐标）与是否最大化
    RECT window{};
    bool windowMaximized = false;
  };

  // 会话里的一个标签：非正常退出（崩溃、被结束、关机）后下次启动恢复
  struct SessionTab {
    std::wstring url;
    std::wstring title;
    bool active = false;
    bool pinned = false;
  };
  struct ClosedTab {
    std::wstring url;
    std::wstring title;
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
  void UpdateTabDrag(POINT point);
  void CancelTabDrag(bool restore = true);
  void FinishTabDrag(POINT point);
  void MoveTab(size_t from, size_t to);
  bool MoveTabTo(uint32_t id, BrowserWindow& destination);
  bool OutsideTabStrip(POINT point) const;

  void OnEnvironmentCreated(HRESULT result, ICoreWebView2Environment* environment);
  void StartAgentBridge();
  void CreateController(Tab& tab);
  void OnControllerCreated(uint32_t id, HRESULT result, ICoreWebView2Controller* controller);
  void OnControllerFailed(Tab& tab, HRESULT result);
  // 独立应用窗口没有标签条可以重开：内核建不起来或中途退出时自己重建（delayMs 为 0 = 按退避）
  void ScheduleAppRecovery(UINT delayMs, const wchar_t* message);
  void RecoverAppWebView();
  bool AppWebViewLive() const;
  void AbandonNewWindow(Tab& tab);
  void ConfigureWebView(Tab& tab);
  void ConfigureProfile(Tab& tab);
  void InstallNextExtension();
  void PruneUnlistedExtensions();
  void RemoveNextUnlistedExtension();
  void EnforceTabLimit();
  bool HandleTabLimitMessage(Tab& tab, const std::wstring& source, const std::string& text);
  void SaveExtensionStatus();   // 合并：同一轮消息里多次调用只写一次
  void WriteExtensionStatus();
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
  void OpenNewWindowAsTab(ICoreWebView2NewWindowRequestedEventArgs* args, bool activate, uint32_t openerId);
  void CreateSpare();

  void NewTab(const std::wstring& url, bool activate = true, size_t at = static_cast<size_t>(-1));
  size_t InsertTab(std::unique_ptr<Tab> tab, size_t at);  // 普通标签不插进固定标签中间；返回下标
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
  void ExecuteCommand(WPARAM command, LPARAM arg = 0);
  void FocusAddressBar();
  void UpdateAddressBar();
  void UpdateWindowTitle();
  void NavigateFromAddressBar(bool newTab = false);
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
  LRESULT OnCopyData(const COPYDATASTRUCT& data);
  void OpenFromAppWindow(const std::wstring& url, bool userInitiated);

  // 标签：右键菜单、固定、静音、关闭的标签重开、会话恢复（browser_tabs.cpp）
  void ShowTabContextMenu(size_t index, POINT screen);
  void DuplicateTab(size_t index);
  void CloseTabs(const std::vector<uint32_t>& ids);
  void TogglePinned(size_t index);
  void ToggleMuted(size_t index);
  void RememberClosedTab(const Tab& tab);
  void ReopenClosedTab();
  size_t InsertIndexFor(uint32_t openerId) const;
  void HardReload();
  bool AdoptAgentTab(Tab& tab);
  void NavigateTab(Tab& tab, const std::wstring& url);
  void ScheduleSessionSave();
  void SaveSession(bool clean, bool durable);
  void RestoreSession();
  std::wstring SessionFile() const;
  void OnProcessFailed(uint32_t id, COREWEBVIEW2_PROCESS_FAILED_KIND kind, ICoreWebView2* sender);
  void ScheduleEnvironmentRecovery();
  void RecoverEnvironment();
  BrowserWindow& PrimaryWindow();  // 同一资料里管智能体桥、扩展与内核重建的那个窗口
  void ResetTabWebView(Tab& tab);
  void UpdateMemoryTarget(Tab& tab);
  void FocusActiveContent();

  // 外壳界面：悬停提示、地址栏菜单与按键、书签栏菜单、弹窗拦截（browser_chrome.cpp）
  void UpdateChromeTooltip();
  std::wstring TooltipText(const ChromeHit& hit) const;
  static LRESULT CALLBACK AddressBarProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR data);
  static LRESULT CALLBACK ExtensionButtonProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR id,
                                              DWORD_PTR data);
  void ShowAddressBarMenu(POINT screen);
  void PasteAndGo();
  bool PasteIntoAddressBar();
  void DeletePreviousWord();
  void ShowBookmarkBarMenu(POINT screen);
  void ShowPopupBlockedMenu(POINT screen);
  void OnPopupBlocked(Tab& source, const std::wstring& url);
  void OnFaviconUpdated(uint64_t hash);
  bool PopupAllowed(const std::wstring& url) const;
  void ShowNotice(const std::wstring& text);
  void PostTask(std::function<void()> task);
  void ScheduleBookmarkSave();
  void FlushBookmarkSave();
  void SaveWindowPlacement();
  int ApplyWindowPlacement(int showCmd);
  std::wstring AppPlacementFile() const;
  void StartExtensions();
  void FlushForEndSession();

  void MaybeWriteMetrics();
  void WriteMetrics(bool forced);

  Options options_;
  BrowserWindow* root_ = nullptr;
  bool ownsPrimary_ = true;
  std::vector<std::unique_ptr<BrowserWindow>> detachedWindows_;
  inline static std::vector<BrowserWindow*> liveWindows_;
  struct ProfileState {
    BookmarkStore bookmarks;
    HistoryStore history;
    Settings settings;
    FaviconCache favicons;   // 所有窗口共用一份：不再每个窗口各起一个抓图标线程
    FileWriter writer;       // 书签、会话、设置在后台写盘
    std::vector<ClosedTab> closedTabs;   // 最近关闭的标签（Ctrl+Shift+T）
    std::vector<SessionTab> restoreTabs;  // 上次非正常退出时开着的标签
    bool bookmarksDirty = false;
    bool sessionWritable = false;  // 日常主实例才写会话
    bool endingSession = false;    // 关机/注销中：之后的关窗不算正常退出，下次照样恢复
    bool recovering = false;       // 内核退出后正在重建（各窗口的标签各报一次，只处理一次）
  };
  std::shared_ptr<ProfileState> profile_;
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
  uint32_t envGeneration_ = 0;   // 每次重建环境加一：旧环境迟到的回调直接丢弃
  UINT appRecoverDelay_ = 0;     // 应用窗口重建退避（毫秒），成功后清零
  std::vector<std::unique_ptr<Tab>> tabs_;
  std::unique_ptr<Tab> spare_;
  size_t active_ = 0;
  inline static uint32_t nextTabId_ = 1;
  uint32_t firstTabId_ = 0;
  std::wstring inlineNtp_;
  std::wstring message_;
  std::unique_ptr<AgentBridge> bridge_;
  AgentBridgeConfig agentConfig_;  // 智能体标签的省电设置（桥启用时从配置读入）
  bool freezeTimerRunning_ = false;

  // 书签与设置：Create 时起后台线程读，首帧前等它读完
  BookmarkStore& bookmarks_;
  HistoryStore& history_;
  FaviconCache& favicons_;
  bool profileConfigured_ = false;
  bool extensionsConfigLoaded_ = false;  // extensions.json 只读一次：内核重建后重装扩展不重复加条目
  bool extensionsStarted_ = false;       // 首个页面加载完才开始装扩展，不和首屏抢
  bool extensionStatusQueued_ = false;   // 扩展状态合并成一次写
  std::string lastToolbarState_;         // 工具栏状态没变就不再写文件
  HWND chromeTooltip_ = nullptr;         // 标签与图标按钮的悬停提示
  ChromeHit tooltipHit_;
  std::wstring tooltipText_;
  HWND hoverExtensionButton_ = nullptr;
  float frozenTabWidth_ = 0;             // 用鼠标连续关标签时保持的宽度，鼠标离开标签条后展开
  UINT recoverDelay_ = 0;                // 日常窗口内核重建的退避（毫秒）
  bool sessionTimerPending_ = false;
  RECT editRect_{};                      // 地址栏上次的位置：没变就不再 MoveWindow 重画
  HICON appIconBig_ = nullptr;           // --app-icon 的大小两档
  HICON appIconSmall_ = nullptr;
  bool firstFrameShown_ = false;
  // 书签菜单子菜单在展开时才填（WM_INITMENUPOPUP），命令号对应书签 id，菜单返回后再按 id 取
  struct BookmarkMenuState {
    BookmarkMenuIcons* icons = nullptr;
    std::map<HMENU, int64_t> folders;
    std::set<HMENU> filled;
    std::vector<int64_t> urls;
    std::vector<int64_t> openAll;
  };
  BookmarkMenuState* bookmarkMenu_ = nullptr;
  void FillBookmarkMenu(HMENU menu, const std::vector<BookmarkNode*>& nodes, BookmarkNode* owner);
  // 当前页是否已收藏：按网址与书签版本缓存，重绘不再整树查找
  mutable std::wstring starredUrl_;
  mutable uint64_t starredRevision_ = 0;
  mutable bool starredValue_ = false;
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
  Settings& settings_;
  HANDLE profileThread_ = nullptr;
  std::wstring profileDir_;     // 用户数据目录（书签、设置文件所在）
  bool profileLoaded_ = false;
  std::vector<BookmarkNode*> barItems_;  // 书签栏上排出来的项（与 layout_.bookmarks 对应）
  std::vector<BookmarkNode*> overflowItems_;

  ChromeHit hover_;
  ChromeHit pressed_;
  uint32_t dragTabId_ = 0;
  size_t dragTabOriginalIndex_ = 0;
  POINT dragTabStart_{};
  float dragTabGrabX_ = 0;
  bool draggingTab_ = false;
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
