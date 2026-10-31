#include "browser_window.h"
#include "browser_url.h"

#include "json.h"
#include "metrics.h"
#include "menu_icons.h"
#include "bookmark_editor.h"
#include "extension_popup_focus.h"
#include "extension_toolbar.h"
#include "ntp_html.h"

#include <WebView2EnvironmentOptions.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <thread>

#ifndef JISU_COMMIT
#define JISU_COMMIT "local"
#endif
#ifndef JISU_WEBVIEW2_SDK
#define JISU_WEBVIEW2_SDK "unknown"
#endif
#ifndef JISU_VERSION
#define JISU_VERSION "0.2.0"
#endif

using Microsoft::WRL::Callback;
using Microsoft::WRL::Make;

namespace jisu {
namespace {

constexpr wchar_t kWindowClass[] = L"JisuBrowserWindow";
constexpr wchar_t kAgentHostClass[] = L"JisuAgentHost";
constexpr wchar_t kNewTabTitle[] = L"新标签页";
constexpr wchar_t kAgentTabTitle[] = L"智能体";
constexpr wchar_t kProfileProp[] = L"JisuBrowser.Profile";

constexpr UINT kMsgCommand = WM_APP + 1;
constexpr UINT kMsgFavicon = WM_APP + 2;   // wParam：图标的 FaviconHash
constexpr UINT kMsgTabLimit = WM_APP + 3;
constexpr UINT kMsgTask = WM_APP + 6;      // lParam：std::function<void()>*，在界面线程上、WebView2 回调之外执行
constexpr UINT kMsgFullscreenChanged = WM_APP + 42;
constexpr UINT kMsgInstallTray = WM_APP + 41;
constexpr UINT_PTR kTimerSpare = 1;
constexpr UINT_PTR kTimerMetrics = 2;
constexpr UINT_PTR kTimerWatchdog = 3;
constexpr UINT_PTR kTimerPreheat = 4;
constexpr UINT_PTR kTimerAgentFreeze = 5;
constexpr UINT_PTR kTimerTrayRetry = 6;
constexpr UINT_PTR kTimerAppRecover = 8;  // 7 是媒体轮询
constexpr UINT_PTR kTimerSession = 9;      // 会话合并写盘
constexpr UINT_PTR kTimerBookmarks = 10;   // 书签合并写盘
constexpr UINT_PTR kTimerRecover = 11;     // 日常窗口内核重建
constexpr UINT_PTR kTimerExtensions = 12;  // 首个页面迟迟不加载完时也开始装扩展
constexpr UINT kAppWatchdogMs = 15000;     // 应用窗口环境/控件迟迟建不好（卡住而非失败）也重建

// 智能体标签里媒体不自动播放：play() 像被浏览器自动播放策略拒绝一样返回 NotAllowedError，autoplay 属性起播的立即暂停。
// 只装在智能体标签上；window.__jisuAllowMedia = true 可放行
constexpr wchar_t kAgentMediaScript[] =
    L"(()=>{if(window.__jisuAgentMedia)return;window.__jisuAgentMedia=1;"
    L"const p=HTMLMediaElement.prototype,play=p.play;"
    L"Object.defineProperty(p,'play',{configurable:true,writable:true,value:function(){"
    L"if(window.__jisuAllowMedia)return play.call(this);"
    L"return Promise.reject(new DOMException('play() is blocked in agent tabs','NotAllowedError'));}});"
    L"document.addEventListener('play',e=>{const t=e.target;"
    L"if(!window.__jisuAllowMedia&&t&&t.pause)t.pause();},true);})();";
constexpr int kAddressBarId = 100;

// 弹出菜单命令号
constexpr UINT kMenuNewTab = 1001;
constexpr UINT kMenuBookmarkManager = 1002;
constexpr UINT kMenuToggleBar = 1003;
constexpr UINT kMenuImport = 1004;
constexpr UINT kMenuDefaultBrowser = 1005;
constexpr UINT kMenuAbout = 1006;
constexpr UINT kMenuExtensions = 1007;
constexpr UINT kMenuHistory = 1008;
constexpr UINT kMenuMedia = 1009;
constexpr UINT kMenuReopenClosed = 1010;
constexpr UINT kMenuOpen = 2001;
constexpr UINT kMenuOpenNewTab = 2002;
constexpr UINT kMenuEdit = 2003;
constexpr UINT kMenuDelete = 2004;
constexpr UINT kMenuOpenAll = 2005;
constexpr UINT kMenuCopyUrl = 2006;
constexpr UINT kMenuRemoveStar = 3001;
constexpr UINT kMenuEditStar = 3002;
constexpr UINT kMenuItemBase = 10000;    // 书签菜单里的网址：kMenuItemBase + 序号
constexpr UINT kMenuOpenAllBase = 40000;  // 文件夹菜单末尾的「全部打开」：kMenuOpenAllBase + 文件夹序号
constexpr size_t kMenuItemLimit = 2000;

// DWM 属性值：较旧的 SDK 头里没有这些名字
constexpr DWORD kDwmUseImmersiveDarkMode = 20;
constexpr DWORD kDwmCaptionColor = 35;
constexpr DWORD kDwmTextColor = 36;

enum Command : WPARAM {
  kCmdNone = 0,
  kCmdNewTab,
  kCmdCloseTab,
  kCmdNextTab,
  kCmdPrevTab,
  kCmdFocusAddress,
  kCmdReload,
  kCmdStopOrReload,
  kCmdBack,
  kCmdForward,
  kCmdToggleBookmark,
  kCmdToggleBookmarkBar,
  kCmdBookmarkManager,
  kCmdToggleFullscreen,
  kCmdExitFullscreen,
  kCmdHistory,
  kCmdMedia,
  kCmdHardReload,    // 跳过缓存重新加载：Ctrl+Shift+R、Shift+F5、Ctrl+F5
  kCmdReopenClosed,  // Ctrl+Shift+T
  kCmdSelectTab,     // Ctrl+1…8 第几个标签，Ctrl+9 最后一个（lParam 为数字）
};

COREWEBVIEW2_COLOR ToWebViewColor(Rgb c) { return COREWEBVIEW2_COLOR{255, c.r, c.g, c.b}; }

POINT PointFromLParam(LPARAM lParam) {
  return POINT{static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam))};
}

std::wstring ExePath() {
  wchar_t buffer[MAX_PATH];
  const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
  return std::wstring(buffer, length);
}

std::wstring ExeDirectory() {
  const std::wstring path = ExePath();
  const size_t slash = path.find_last_of(L"\\/");
  return slash == std::wstring::npos ? std::wstring(L".") : path.substr(0, slash);
}

int MonitorRefreshHz(HWND hwnd) {
  MONITORINFOEXW info{};
  info.cbSize = static_cast<DWORD>(sizeof(info));
  if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &info)) {
    return 0;
  }
  DEVMODEW mode{};
  mode.dmSize = static_cast<WORD>(sizeof(mode));
  if (!EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode)) {
    return 0;
  }
  return static_cast<int>(mode.dmDisplayFrequency);
}

std::wstring BookmarkLabel(const BookmarkNode& node) { return node.title.empty() ? node.url : node.title; }

// 用户数据目录的短指纹：同一目录只开一个主实例，另一个实例把网址转过来
uint32_t ProfileHash(const std::wstring& path) {
  uint32_t hash = 2166136261u;
  for (const wchar_t c : path) {
    hash ^= static_cast<uint32_t>(std::towlower(c));
    hash *= 16777619u;
  }
  return hash == 0 ? 1 : hash;
}

int CountUrls(const BookmarkNode& folder) {
  int count = 0;
  for (const auto& child : folder.children) {
    count += child->folder ? 0 : 1;
  }
  return count;
}

bool SetRegString(HKEY root, const std::wstring& key, const wchar_t* name, const std::wstring& value) {
  HKEY handle = nullptr;
  if (RegCreateKeyExW(root, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &handle, nullptr) != ERROR_SUCCESS) {
    return false;
  }
  const LONG result =
      RegSetValueExW(handle, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                     static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
  RegCloseKey(handle);
  return result == ERROR_SUCCESS;
}

}  // namespace

BrowserWindow::BrowserWindow(const Options& options, GraphicsWarmup& warmup, BrowserWindow* root)
    : options_(options), root_(root ? root : this), ownsPrimary_(!root),
      profile_(root ? root->profile_ : std::make_shared<ProfileState>()),
      warmup_(warmup), palette_(LoadPalette()), bookmarks_(profile_->bookmarks),
      history_(profile_->history), favicons_(profile_->favicons), settings_(profile_->settings) {}

HWND BrowserWindow::FindRunningWindow(const std::wstring& profileDir) {
  const uintptr_t hash = ProfileHash(profileDir);
  HWND window = nullptr;
  while ((window = FindWindowExW(nullptr, window, kWindowClass, nullptr)) != nullptr) {
    if (reinterpret_cast<uintptr_t>(GetPropW(window, kProfileProp)) == hash) {
      return window;
    }
  }
  return nullptr;
}

BrowserWindow::~BrowserWindow() {
  if (profileThread_) {
    WaitForSingleObject(profileThread_, INFINITE);
    CloseHandle(profileThread_);
  }
  if (editFont_) {
    DeleteObject(editFont_);
  }
  if (omniboxBrush_) {
    DeleteObject(omniboxBrush_);
  }
  for (HICON icon : {appIconBig_, appIconSmall_}) {
    if (icon) DestroyIcon(icon);
  }
}

// ---------------------------------------------------------------------------
// 启动

DWORD WINAPI BrowserWindow::ProfileThreadMain(void* param) {
  auto* self = static_cast<BrowserWindow*>(param);
  Metrics::Get().Mark("profileBegin");
  if (self->options_.appMode) {
    // 应用窗口只读自己上次的位置大小（按 app-id 分开存），不碰日常书签与设置
    std::string text;
    json::Value root;
    if (!self->profileDir_.empty() && ReadFileBytes(self->AppPlacementFile(), text) && json::Parse(text, root)) {
      const json::Value& window = root["window"];
      self->settings_.hasWindow = window.IsObject();
      self->settings_.window = RECT{static_cast<LONG>(window["left"].AsNumber()), static_cast<LONG>(window["top"].AsNumber()),
                                    static_cast<LONG>(window["right"].AsNumber()), static_cast<LONG>(window["bottom"].AsNumber())};
      self->settings_.windowMaximized = window["max"].AsBool();
    }
    Metrics::Get().Mark("profileReady");
    return 0;
  }
  self->LoadSettings();
  // 上次没有正常退出（崩溃、被结束、关机重启）：记下当时开着的标签，首帧前恢复
  if (self->profile_->sessionWritable) {
    std::string text;
    json::Value root;
    if (ReadFileBytes(self->SessionFile(), text) && json::Parse(text, root) && !root["clean"].AsBool(true)) {
      for (const json::Value& item : root["tabs"].Items()) {
        SessionTab tab;
        tab.url = FromUtf8(item["url"].AsString());
        if (tab.url.empty() || tab.url.size() > 16384) continue;
        tab.title = TruncateUtf16(FromUtf8(item["title"].AsString()), 512);
        tab.active = item["active"].AsBool();
        tab.pinned = item["pinned"].AsBool();
        self->profile_->restoreTabs.push_back(std::move(tab));
        if (self->profile_->restoreTabs.size() >= 50) break;
      }
    }
  }
  if (!self->profileDir_.empty()) {
    self->bookmarks_.Load(self->profileDir_ + L"\\Bookmarks");
    // 第一次用默认配置启动：像主流浏览器一样从系统默认浏览器导入一次书签
    if (self->options_.defaultUserData && self->options_.metricsPath.empty() && !self->bookmarks_.Existed() &&
        !self->settings_.importedDefaultBrowser) {
      std::wstring name;
      const std::wstring source = BookmarkStore::DefaultBrowserBookmarksFile(&name);
      if (!source.empty()) {
        self->bookmarks_.Import(source, name, nullptr);
      }
      self->settings_.importedDefaultBrowser = true;
      self->SaveSettings();
    }
  }
  Metrics::Get().Mark("profileReady");
  return 0;
}

bool BrowserWindow::Create(HINSTANCE instance) {
  instance_ = instance;
  profileDir_ = options_.userDataFolder;
  // 会话只由日常主实例写：测量、预热、应用窗口、主实例卡住时的备用实例都不碰
  if (root_ == this) {
    profile_->sessionWritable = !options_.appMode && !options_.secondary && options_.metricsPath.empty() &&
                                options_.preheatSeconds <= 0 && !profileDir_.empty();
  }
  // 书签与设置在后台读，和建窗、预热并行；首帧前汇合
  if (root_ == this) {
    profileThread_ = CreateThread(nullptr, 0, &BrowserWindow::ProfileThreadMain, this, 0, nullptr);
    if (!profileThread_) ProfileThreadMain(this);
  }

  WNDCLASSEXW windowClass{};
  windowClass.cbSize = static_cast<UINT>(sizeof(windowClass));
  windowClass.style = CS_HREDRAW | CS_VREDRAW;
  windowClass.lpfnWndProc = &BrowserWindow::WndProcThunk;
  windowClass.hInstance = instance;
  windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  windowClass.hbrBackground = CreateSolidBrush(ToColorRef(palette_.content));
  windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
  // 小图标单独按小尺寸取：标题栏、任务栏、Alt+Tab 不用大图缩出来的糊图
  windowClass.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                                      GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR | LR_SHARED));
  windowClass.lpszClassName = kWindowClass;
  if (!RegisterClassExW(&windowClass)) {
    const DWORD error = GetLastError();
    DeleteObject(windowClass.hbrBackground);
    if (error != ERROR_CLASS_ALREADY_EXISTS) return false;
  }

  int x = CW_USEDEFAULT;
  int y = CW_USEDEFAULT;
  int width = 0;
  int height = 0;
  if (options_.hasBounds) {
    x = options_.x;
    y = options_.y;
    width = options_.width;
    height = options_.height;
  } else {
    const int dpi = static_cast<int>(GetDpiForSystem());
    width = MulDiv(1280, dpi, 96);
    height = MulDiv(860, dpi, 96);
  }
  // WS_CLIPCHILDREN：Direct2D 重画标签条时不覆盖地址栏和网页子窗口
  const HWND created = CreateWindowExW(0, kWindowClass, options_.appMode ? options_.appTitle.c_str() : kAppName, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, x, y, width,
                                       height, nullptr, nullptr, instance, this);
  if (!created) {
    return false;
  }
  SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
  // 图标缓存同一资料只起一份，由主窗口启动；拆出的窗口共用
  if (root_ == this && !options_.appMode && options_.metricsPath.empty() && options_.preheatSeconds <= 0) {
    favicons_.Start(profileDir_, hwnd_, kMsgFavicon);
  }
  SetPropW(hwnd_, kProfileProp, reinterpret_cast<HANDLE>(static_cast<uintptr_t>(ProfileHash(options_.InstanceKey()))));
  if (options_.appMode) {
    SetPropW(hwnd_, L"JisuBrowser.AppMode", reinterpret_cast<HANDLE>(1));
    if (!options_.appIcon.empty()) {
      // 大小两档分别从 .ico 里取最合适的一张；窗口关闭时释放
      const UINT dpi = GetDpiForWindow(hwnd_);
      appIconBig_ = static_cast<HICON>(LoadImageW(nullptr, options_.appIcon.c_str(), IMAGE_ICON,
                                                   GetSystemMetricsForDpi(SM_CXICON, dpi), GetSystemMetricsForDpi(SM_CYICON, dpi),
                                                   LR_LOADFROMFILE));
      appIconSmall_ = static_cast<HICON>(LoadImageW(nullptr, options_.appIcon.c_str(), IMAGE_ICON,
                                                     GetSystemMetricsForDpi(SM_CXSMICON, dpi),
                                                     GetSystemMetricsForDpi(SM_CYSMICON, dpi), LR_LOADFROMFILE));
      if (appIconBig_) SendMessageW(hwnd_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(appIconBig_));
      if (appIconSmall_) SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(appIconSmall_));
    }
  }
  if (options_.preheatSeconds > 0) {
    SetTimer(hwnd_, kTimerPreheat, static_cast<UINT>(options_.preheatSeconds) * 1000u, nullptr);
  }
  liveWindows_.push_back(this);
  return true;
}

void BrowserWindow::ShowFirstFrame(int showCmd) {
  Metrics& metrics = Metrics::Get();
  warmup_.WaitReady();
  metrics.Mark("warmupJoined");
  if (profileThread_) {
    WaitForSingleObject(profileThread_, INFINITE);
  }
  profileLoaded_ = true;
  metrics.Mark("profileJoined");
  RestoreSession();  // 上次非正常退出：首帧就带上当时的标签
  // 上次关窗时的位置大小（窗口还没显示，不闪）；拔掉了那块显示器就用默认位置
  if (root_ == this) {
    showCmd = ApplyWindowPlacement(showCmd);
  }
  if (warmup_.d2d) {
    painter_.CreateTarget(warmup_, hwnd_, options_.renderTarget, dpi_);
  }
  metrics.Mark("renderTargetCreated");
  Relayout();  // 书签读完、排版对象就绪后再排一次：首帧就带书签栏

  ShowWindow(hwnd_, options_.noActivate ? SW_SHOWNOACTIVATE : showCmd);
  metrics.Mark("windowShown");
  // 同步画出标签条、工具栏和地址栏；后台线程随后 DwmFlush，记下首帧真正上屏的时刻
  RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW | RDW_ALLCHILDREN);
  metrics.Mark("firstPaint");
  warmup_.NotifyFirstPaint();
  firstFrameShown_ = true;
  // Register after the first frame, never in the latency-sensitive startup path.
  if (ownsPrimary_ && !options_.appMode && options_.metricsPath.empty() && !options_.secondary) {
    PostMessageW(hwnd_, kMsgInstallTray, 0, 0);
  }
  if (IsZoomed(hwnd_) && !options_.appMode) {
    // 最大化启动：首帧之后再算一次非客户区，自动隐藏的任务栏留出 1 像素（首帧路径上不加载 shell32）
    PostTask([this] {
      SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    });
  }

  if (!options_.noActivate) {
    // 新标签页给地址栏；带着网址启动或恢复了会话，焦点等网页建好后交给网页
    if (const Tab* tab = ActiveTab(); tab && !tab->pendingUrl.empty()) {
      tabs_[active_]->focusOnCreate = true;
    } else {
      SetFocus(edit_);
    }
  }
  if (!options_.metricsPath.empty()) {
    SetTimer(hwnd_, kTimerWatchdog, 30000, nullptr);
  }
}

void BrowserWindow::StartEnvironment() {
  Metrics& metrics = Metrics::Get();
  metrics.Mark("envBegin");
  auto environmentOptions = Make<CoreWebView2EnvironmentOptions>();
  environmentOptions->put_AreBrowserExtensionsEnabled(TRUE);
  if (!options_.browserArgs.empty()) {
    environmentOptions->put_AdditionalBrowserArguments(options_.browserArgs.c_str());
  }
  if (!options_.trackingPrevention) {
    environmentOptions->put_EnableTrackingPrevention(FALSE);
  }
  const wchar_t* userDataFolder = options_.userDataFolder.empty() ? nullptr : options_.userDataFolder.c_str();
  const uint32_t generation = ++envGeneration_;
  if (options_.appMode) {
    SetTimer(hwnd_, kTimerAppRecover, kAppWatchdogMs, nullptr);
  }
  const HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
      nullptr, userDataFolder, environmentOptions.Get(),
      Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
          [this, generation](HRESULT result, ICoreWebView2Environment* environment) -> HRESULT {
            if (generation == envGeneration_) {
              OnEnvironmentCreated(result, environment);
            }
            return S_OK;
          })
          .Get());
  metrics.Mark("envCallReturned");
  if (FAILED(hr)) {
    OnEnvironmentCreated(hr, nullptr);
  }
}

// ---------------------------------------------------------------------------
// WebView2

void BrowserWindow::OnEnvironmentCreated(HRESULT result, ICoreWebView2Environment* environment) {
  if (destroyed_) {
    return;
  }
  Metrics& metrics = Metrics::Get();
  if (FAILED(result) || !environment) {
    metrics.Mark("envFailed");
    metrics.SetNumber("envError", static_cast<double>(static_cast<uint32_t>(result)));
    wchar_t text[200];
    swprintf_s(text, L"无法启动 WebView2 内核（0x%08X）。请确认已安装 Microsoft Edge WebView2 运行时。",
               static_cast<unsigned>(result));
    message_ = text;
    InvalidateChrome();
    if (options_.appMode) {
      // 常见于同一资料目录的内核进程以另一完整性级别（管理员）运行：它退出后重试即可接上
      swprintf_s(text, L"网页内核连接失败（0x%08X），正在自动重试…", static_cast<unsigned>(result));
      ScheduleAppRecovery(0, text);
    } else if (options_.metricsPath.empty()) {
      // 日常窗口同样退避重试（0.5 秒起，封顶 30 秒），不停在空白页等用户重启；没装运行时仍显示安装提示
      if (result != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) {
        swprintf_s(text, L"网页内核连接失败（0x%08X），正在自动重试…", static_cast<unsigned>(result));
        message_ = text;
      }
      ScheduleEnvironmentRecovery();
    }
    // 延后写出：默认 early 模式下这里可能早于首帧，首帧相关时间点还没记
    if (!options_.metricsPath.empty()) {
      SetTimer(hwnd_, kTimerMetrics, 500, nullptr);
    }
    return;
  }
  metrics.Mark("envReady");
  environment_ = environment;
  if (!options_.appMode) {
    recoverDelay_ = 0;
    message_.clear();
  }
  CoTaskString version;
  if (SUCCEEDED(environment_->get_BrowserVersionString(version.Put()))) {
    metrics.SetText("runtimeVersion", ToUtf8(version.Str()));
  }
  // 同一资料的窗口共用一个环境（拆出的窗口、内核重建后）：各自把没有控件的标签建起来；
  // 恢复出来没看过的后台标签点到时再建
  for (auto* window : liveWindows_) {
    if (window->profile_ != profile_ || window->options_.appMode != options_.appMode || window->destroyed_) continue;
    window->environment_ = environment_;
    if (!window->options_.appMode) window->message_.clear();
    for (auto& tab : window->tabs_) {
      if (!tab->controller && !tab->creating && !tab->lazy) {
        window->CreateController(*tab);
      }
    }
    window->InvalidateChrome();
  }
  if (ownsPrimary_) {
    StartAgentBridge();  // 桥只在主窗口上开：拆出的窗口重建内核时不另开一个去抢 broker 邀请
  }
}

void BrowserWindow::StartAgentBridge() {
  if (options_.noAgent || bridge_ || options_.agentConfigPath.empty()) {
    return;
  }
  std::string error;
  AgentBridgeConfig config = LoadAgentBridgeConfig(options_.agentConfigPath, &error);
  Metrics::Get().SetText("agentBridge", config.enabled ? "enabled" : (error.empty() ? "off" : error));
  if (!config.enabled) {
    return;
  }
  agentConfig_ = config;
  agentConfig_.token.clear();  // 窗口只用省电设置，令牌只留在桥里
  bridge_ = std::make_unique<AgentBridge>(hwnd_, *this, std::move(config));
  bridge_->Start();
}

void BrowserWindow::CreateController(Tab& tab) {
  if (!environment_ || tab.creating || tab.controller) {
    return;
  }
  Metrics& metrics = Metrics::Get();
  tab.creating = true;
  if (tab.id == firstTabId_) {
    metrics.Mark("controllerBegin");
  }
  if (IsSpare(tab)) {
    metrics.Mark("spareBegin");
  }
  // 智能体标签直接建在屏幕外宿主里：从头到尾不在主窗口露面
  HWND parent = hwnd_;
  if (tab.agent) {
    if (HWND host = EnsureAgentHost()) {
      parent = host;
    }
  }
  tab.parent = parent;
  const uint32_t id = tab.id;
  const uint32_t generation = envGeneration_;
  auto handler = Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
      [this, id, generation](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
        if (generation != envGeneration_) {  // 环境已重建：旧环境的控件不要
          if (controller) controller->Close();
          return S_OK;
        }
        OnControllerCreated(id, result, controller);
        return S_OK;
      });
  HRESULT hr = E_NOINTERFACE;
  ComPtr<ICoreWebView2Environment10> environment10;
  ComPtr<ICoreWebView2ControllerOptions> controllerOptions;
  if (SUCCEEDED(environment_.As(&environment10)) &&
      SUCCEEDED(environment10->CreateCoreWebView2ControllerOptions(controllerOptions.GetAddressOf()))) {
    // 控件从创建那一刻起就用外壳底色，网页出来之前不白闪
    ComPtr<ICoreWebView2ControllerOptions3> options3;
    if (SUCCEEDED(controllerOptions.As(&options3))) {
      options3->put_DefaultBackgroundColor(ToWebViewColor(palette_.content));
    }
    hr = environment10->CreateCoreWebView2ControllerWithOptions(parent, controllerOptions.Get(), handler.Get());
  }
  if (FAILED(hr)) {
    hr = environment_->CreateCoreWebView2Controller(parent, handler.Get());
  }
  if (FAILED(hr)) {
    tab.creating = false;
    OnControllerFailed(tab, hr);  // 可能销毁 tab（备用控件、智能体标签），之后不能再用
  }
}

void BrowserWindow::OnControllerCreated(uint32_t id, HRESULT result, ICoreWebView2Controller* controller) {
  Metrics& metrics = Metrics::Get();
  Tab* tab = destroyed_ ? nullptr : FindTab(id);
  if (!tab) {
    if (controller) {
      controller->Close();  // 标签在控件建好之前已经关掉
    }
    return;
  }
  tab->creating = false;
  if (FAILED(result) || !controller) {
    OnControllerFailed(*tab, FAILED(result) ? result : E_UNEXPECTED);
    return;
  }

  tab->controller = controller;
  tab->controller->get_CoreWebView2(tab->webview.ReleaseAndGetAddressOf());
  const bool spare = IsSpare(*tab);
  const bool show = !spare && options_.preheatSeconds <= 0 && IsActive(*tab) && !IsIconic(hwnd_);
  // 先藏再定位：备用控件、后台标签、智能体标签从头到尾不露面
  if (!show) {
    tab->controller->put_IsVisible(FALSE);
    tab->visible = false;
  }
  ComPtr<ICoreWebView2Controller2> controller2;
  if (SUCCEEDED(tab->controller.As(&controller2))) {
    controller2->put_DefaultBackgroundColor(ToWebViewColor(palette_.content));
  }
  tab->controller->put_Bounds(tab->parent == agentHost_ && agentHost_ ? AgentHostRect() : ContentRect());
  if (show || tab->agent) {
    PlaceTab(*tab);  // 智能体标签：套上宿主里的光栅化比例，开始挂起计时
  }
  if (!tab->webview) {
    AbandonNewWindow(*tab);
    return;
  }
  tab->error.clear();
  ConfigureWebView(*tab);
  UpdateMemoryTarget(*tab);
  if (options_.appMode) {
    KillTimer(hwnd_, kTimerAppRecover);
    appRecoverDelay_ = 0;
    message_.clear();
  }
  if (id == firstTabId_) {
    metrics.Mark("controllerReady");
  }
  if (spare) {
    metrics.Mark("spareReady");
  }

  if (tab->newWindowArgs) {
    if (FAILED(tab->newWindowArgs->put_NewWindow(tab->webview.Get()))) {
      AbandonNewWindow(*tab);
      tab->webview->Navigate(tab->url.empty() ? L"about:blank" : tab->url.c_str());
    } else {
      tab->newWindowDeferral->Complete();
      tab->newWindowArgs.Reset();
      tab->newWindowDeferral.Reset();
    }
  } else if (!tab->pendingUrl.empty()) {
    tab->allowInternal = IsInternalPage(tab->pendingUrl);  // 外壳发起的打开：可以是书签、历史等内部页面
    tab->webview->Navigate(tab->pendingUrl.c_str());
    tab->pendingUrl.clear();
  } else {
    NavigateNewTabPage(*tab);
    if (id == firstTabId_) {
      metrics.Mark("ntpNavigate");
    }
  }
  if (IsActive(*tab)) {
    UpdateAddressBar();
    InvalidateChrome();
    if (tab->focusOnCreate) {
      tab->focusOnCreate = false;
      if (tab->visible && !omniboxFocused_ && GetForegroundWindow() == hwnd_) {
        tab->controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
      }
    }
  }
  if (spare) {
    MaybeWriteMetrics();
  }
}

void BrowserWindow::OnControllerFailed(Tab& tab, HRESULT result) {
  const bool first = tab.id == firstTabId_;
  const bool popup = tab.newWindowArgs != nullptr;  // window.open 的目标：请求一旦放弃就不再重试
  Metrics::Get().Mark(first ? "controllerFailed" : "tabControllerFailed");
  AbandonNewWindow(tab);
  if (IsSpare(tab)) {
    spare_.reset();  // 备用控件建不出来就不备了，新标签页现建
    return;
  }
  // 官方建议失败后重试；ERROR_INVALID_STATE（同一数据目录的环境选项冲突）与 E_ABORT（父窗口已销毁）除外
  if (!popup && !tab.retried && result != HRESULT_FROM_WIN32(ERROR_INVALID_STATE) && result != E_ABORT) {
    tab.retried = true;
    CreateController(tab);
    return;
  }
  if (tab.agent) {
    // 智能体标签建不出来：告诉桥，然后把这个空标签拿掉
    if (tab.leaseDone) {
      LeaseCallback done = std::move(tab.leaseDone);
      tab.leaseDone = nullptr;
      wchar_t text[96];
      swprintf_s(text, L"WebView2 controller creation failed (0x%08X)", static_cast<unsigned>(result));
      done(0, nullptr, ToUtf8(text));
    }
    const size_t index = IndexOf(tab);
    if (index < tabs_.size()) {
      CloseTab(index);
    }
    return;
  }
  wchar_t text[160];
  if (options_.appMode) {
    swprintf_s(text, L"网页控件创建失败（0x%08X），正在自动重试…", static_cast<unsigned>(result));
    ScheduleAppRecovery(0, text);
  } else {
    // 原因只挂在这个标签上：切到别的标签不再显示，这个标签按 F5 / 刷新钮重新创建
    swprintf_s(text, L"无法创建网页控件（0x%08X）。按 F5 或点刷新重试。", static_cast<unsigned>(result));
    tab.error = text;
    InvalidateChrome();
  }
  if (first && !options_.metricsPath.empty()) {
    SetTimer(hwnd_, kTimerMetrics, 500, nullptr);
  }
}

bool BrowserWindow::AppWebViewLive() const {
  return environment_ && !tabs_.empty() && tabs_.front()->webview;
}

void BrowserWindow::ScheduleAppRecovery(UINT delayMs, const wchar_t* message) {
  if (!options_.appMode || destroyed_) return;
  if (message) message_ = message;
  InvalidateRect(hwnd_, nullptr, FALSE);
  if (delayMs == 0) {  // 3 秒起，每次翻倍，封顶 30 秒
    appRecoverDelay_ = appRecoverDelay_ ? std::min<UINT>(appRecoverDelay_ * 2, 30000) : 3000;
    delayMs = appRecoverDelay_;
  }
  SetTimer(hwnd_, kTimerAppRecover, delayMs, nullptr);
}

// 丢掉当前环境和控件，按原网址重建；页面本身的状态（草稿、登录）在资料目录里，不受影响
void BrowserWindow::RecoverAppWebView() {
  KillTimer(hwnd_, kTimerAppRecover);
  if (!options_.appMode || destroyed_ || AppWebViewLive()) return;
  for (auto& tab : tabs_) {
    if (tab->controller) tab->controller->Close();
    tab->controller.Reset();
    tab->webview.Reset();
    tab->creating = false;
    tab->retried = false;
    if (tab->pendingUrl.empty()) tab->pendingUrl = tab->url;
    if (tab->pendingUrl.empty()) tab->pendingUrl = ToNavigableUrl(options_.startUrl);
  }
  environment_.Reset();
  if (message_.empty()) message_ = L"正在重新连接网页内核…";  // 看门狗到点：上一次是卡住而不是报错
  InvalidateRect(hwnd_, nullptr, FALSE);
  StartEnvironment();
}

// 放弃 window.open 请求：标记已处理，内核就不会自己再弹一个外壳管不到的窗口
void BrowserWindow::AbandonNewWindow(Tab& tab) {
  if (tab.newWindowArgs) {
    tab.newWindowArgs->put_Handled(TRUE);
  }
  if (tab.newWindowDeferral) {
    tab.newWindowDeferral->Complete();
  }
  tab.newWindowArgs.Reset();
  tab.newWindowDeferral.Reset();
}

void BrowserWindow::ConfigureWebView(Tab& tab) {
  auto* self = this;
  const auto owner = tab.owner;
  *owner = this;
  // 新标签页与书签管理页：虚拟主机映射到随包的 ntp\ 目录（内置新标签页模式下书签管理页也要用）。
  // 图标缓存不再对所有网页映射：只在书签管理页期间映射（见 NavigationStarting），网站探测不到本机访问过哪些站点
  ComPtr<ICoreWebView2_3> webview3;
  if (SUCCEEDED(tab.webview.As(&webview3))) {
    const std::wstring folder = ExeDirectory() + L"\\ntp";
    webview3->SetVirtualHostNameToFolderMapping(kNtpHost, folder.c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY);
  }
  tab.faviconHost = false;
  self->ConfigureProfile(tab);
  if (self->extensionProfile_ && !self->extensionsStarted_) {
    // 扩展等首个页面加载完再装（NavigationCompleted 里开始）；页面迟迟不落地也最多等 2 秒
    SetTimer(self->hwnd_, kTimerExtensions, 2000, nullptr);
  }
  const uint32_t id = tab.id;
  EventRegistrationToken token{};
  ComPtr<ICoreWebView2_15> faviconView;
  if (SUCCEEDED(tab.webview.As(&faviconView))) {
    faviconView->add_FaviconChanged(Callback<ICoreWebView2FaviconChangedEventHandler>(
      [owner, id](ICoreWebView2* sender, IUnknown*) -> HRESULT {
        auto* self = *owner;
        if (!self || self->destroyed_) return S_OK;
        const Tab* current = self->FindTab(id);
        if (!current || current->agent) return S_OK;
        const std::wstring url = current->url;
        ComPtr<ICoreWebView2_15> view;
        if (SUCCEEDED(sender->QueryInterface(IID_PPV_ARGS(view.GetAddressOf())))) {
          view->GetFavicon(COREWEBVIEW2_FAVICON_IMAGE_FORMAT_PNG, Callback<ICoreWebView2GetFaviconCompletedHandler>(
            [owner, url](HRESULT hr, IStream* image) -> HRESULT {
              auto* self = *owner;
              if (!self || self->destroyed_) return S_OK;
              if (SUCCEEDED(hr)) self->favicons_.Store(url, image);  // 内容没变不写；写盘在后台线程
              return S_OK;
            }).Get());
        }
        return S_OK;
      }).Get(), &token);
  }
  if (tab.agent) {
    // 智能体标签在屏幕外：内核自带的弹框和权限气泡会落在看不见的地方，改由外壳处理
    ComPtr<ICoreWebView2Settings> settings;
    if (SUCCEEDED(tab.webview->get_Settings(settings.GetAddressOf()))) {
      settings->put_AreDefaultScriptDialogsEnabled(FALSE);
    }
    if (self->agentConfig_.blockAutoplay) {
      // 记下脚本 id：用户接管这个标签时移除，之后打开的网页照常播放
      tab.webview->AddScriptToExecuteOnDocumentCreated(
          kAgentMediaScript,
          Callback<ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler>(
              [owner, id](HRESULT hr, LPCWSTR scriptId) -> HRESULT {
                auto* self = *owner;
                if (!self || self->destroyed_ || FAILED(hr) || !scriptId) return S_OK;
                if (Tab* t = self->FindTab(id)) {
                  if (t->agent) {
                    t->agentScript = scriptId;
                  } else if (t->webview) {
                    t->webview->RemoveScriptToExecuteOnDocumentCreated(scriptId);  // 脚本装好前已被用户接管
                  }
                }
                return S_OK;
              })
              .Get());
    }
    if (self->agentConfig_.blockImages) {
      // 图片请求（含 iframe、worker 发起的）交给外壳：用户正看着这个标签时放行，否则回 404
      ComPtr<ICoreWebView2_22> webview22;
      if (SUCCEEDED(tab.webview.As(&webview22))) {
        webview22->AddWebResourceRequestedFilterWithRequestSourceKinds(
            L"*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_IMAGE, COREWEBVIEW2_WEB_RESOURCE_REQUEST_SOURCE_KINDS_ALL);
      } else {
        tab.webview->AddWebResourceRequestedFilter(L"*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_IMAGE);
      }
      tab.webview->add_WebResourceRequested(
          Callback<ICoreWebView2WebResourceRequestedEventHandler>(
              [owner, id](ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT {
                auto* self = *owner;
                if (!self || self->destroyed_) return S_OK;
                const Tab* t = self->FindTab(id);
                if (!t || !t->agent || self->IsActive(*t) || !self->environment_) {
                  return S_OK;
                }
                ComPtr<ICoreWebView2WebResourceResponse> response;
                if (SUCCEEDED(self->environment_->CreateWebResourceResponse(nullptr, 404, L"Blocked", L"",
                                                                      response.GetAddressOf()))) {
                  args->put_Response(response.Get());
                }
                return S_OK;
              })
              .Get(),
          &token);
    }
  }

  tab.webview->add_WebMessageReceived(
      Callback<ICoreWebView2WebMessageReceivedEventHandler>(
          [owner, id](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
            auto* self = *owner;
            if (!self || self->destroyed_) return S_OK;
            self->OnWebMessage(id, args);
            return S_OK;
          })
          .Get(),
      &token);
  tab.webview->add_DocumentTitleChanged(
      Callback<ICoreWebView2DocumentTitleChangedEventHandler>([owner, id](ICoreWebView2* sender, IUnknown*) -> HRESULT {
        auto* self = *owner;
        if (!self || self->destroyed_) return S_OK;
        if (Tab* t = self->FindTab(id)) {
          CoTaskString title;
          if (SUCCEEDED(sender->get_DocumentTitle(title.Put()))) {
            t->title = title.Str();
          }
          if (t->title.empty() && !IsNewTabPageUrl(t->url)) {
            t->title = t->url;
          }
          if (!t->agent && !HistoryStore::SafeUrl(t->url).empty()) self->history_.Title(t->url, t->title);
          if (self->IsActive(*t)) {
            self->UpdateWindowTitle();
          }
          self->InvalidateChrome();
          if (!t->agent) self->ScheduleSessionSave();
        }
        return S_OK;
      }).Get(),
      &token);
  tab.webview->add_SourceChanged(
      Callback<ICoreWebView2SourceChangedEventHandler>(
          [owner, id](ICoreWebView2* sender, ICoreWebView2SourceChangedEventArgs* args) -> HRESULT {
            auto* self = *owner;
            if (!self || self->destroyed_) return S_OK;
            if (Tab* t = self->FindTab(id)) {
              CoTaskString source;
              if (SUCCEEDED(sender->get_Source(source.Put()))) {
                t->url = source.Str();
              }
              BOOL newDocument = TRUE; args->get_IsNewDocument(&newDocument);
              if (!newDocument) self->RecordVisit(*t);
              if (self->IsActive(*t)) {
                // 地址栏输了一半就点回网页：网页一跳转就显示新网址，不再一直停在没提交的文字上
                if (self->editing_ && !self->omniboxFocused_) self->editing_ = false;
                self->UpdateAddressBar();
                self->InvalidateChrome();  // 星标跟着网址变
              }
              if (!t->agent) self->ScheduleSessionSave();
            }
            return S_OK;
          })
          .Get(),
      &token);
  tab.webview->add_ContainsFullScreenElementChanged(
      Callback<ICoreWebView2ContainsFullScreenElementChangedEventHandler>(
          [owner, id](ICoreWebView2*, IUnknown*) -> HRESULT {
            auto* self = *owner;
            if (!self || self->destroyed_) return S_OK;
            PostMessageW(self->hwnd_, kMsgFullscreenChanged, id, 0);
            return S_OK;
          }).Get(), &token);
  tab.webview->add_HistoryChanged(
      Callback<ICoreWebView2HistoryChangedEventHandler>([owner, id](ICoreWebView2* sender, IUnknown*) -> HRESULT {
        auto* self = *owner;
        if (!self || self->destroyed_) return S_OK;
        if (Tab* t = self->FindTab(id)) {
          BOOL back = FALSE;
          BOOL forward = FALSE;
          sender->get_CanGoBack(&back);
          sender->get_CanGoForward(&forward);
          t->canBack = back != FALSE;
          t->canForward = forward != FALSE;
          if (self->IsActive(*t)) {
            self->InvalidateChrome();
          }
        }
        return S_OK;
      }).Get(),
      &token);
  tab.webview->add_NavigationStarting(
      Callback<ICoreWebView2NavigationStartingEventHandler>(
          [owner, id](ICoreWebView2* sender, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
            auto* self = *owner;
            if (!self || self->destroyed_) return S_OK;
            if (Tab* t = self->FindTab(id)) {
              CoTaskString uri; args->get_Uri(uri.Put());
              const std::wstring target = uri.Str();
              // 内部页面（书签、历史、扩展管理……）只能由外壳打开：网页里的链接、脚本跳转一律拦下；
              // 前进后退、刷新、内部页面之间互跳、智能体（用户自己的自动化）照常
              if (IsInternalPage(target) && !t->agent && !t->allowInternal && !IsInternalPage(t->url)) {
                COREWEBVIEW2_NAVIGATION_KIND kind = COREWEBVIEW2_NAVIGATION_KIND_NEW_DOCUMENT;
                ComPtr<ICoreWebView2NavigationStartingEventArgs3> args3;
                if (SUCCEEDED(args->QueryInterface(IID_PPV_ARGS(args3.GetAddressOf())))) args3->get_NavigationKind(&kind);
                if (kind == COREWEBVIEW2_NAVIGATION_KIND_NEW_DOCUMENT) {
                  args->put_Cancel(TRUE);
                  return S_OK;
                }
              }
              t->allowInternal = false;
              // 图标缓存只给书签管理页（它显示书签图标），别的页面拿不到
              const bool wantFavicons = IsBookmarksPageUrl(target);
              if (wantFavicons != t->faviconHost) {
                ComPtr<ICoreWebView2_3> view3;
                if (SUCCEEDED(sender->QueryInterface(IID_PPV_ARGS(view3.GetAddressOf())))) {
                  if (wantFavicons) {
                    const std::wstring cache = self->profileDir_ + L"\\Favicons";
                    view3->SetVirtualHostNameToFolderMapping(L"favicons.jisu.example", cache.c_str(),
                                                             COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS);
                  } else {
                    view3->ClearVirtualHostNameToFolderMapping(L"favicons.jisu.example");
                  }
                  t->faviconHost = wantFavicons;
                }
              }
              if (!t->blockedPopups.empty()) {
                t->blockedPopups.clear();  // 换了页面：上一页被拦的弹窗不再提示
                if (self->IsActive(*t)) self->Relayout();
              }
              if (t->media && target != L"https://newtab.jisu.example/player.html") self->StopMedia(*t);
              if (t->mediaTarget && target != t->url) { if (auto* mediaTab=self->FindTab(t->mediaTarget)) mediaTab->mediaSource=0; t->mediaTarget=0; }
              if (self->fullscreenTab_ == id) self->ExitContentFullscreen();
              t->loading = true;
              self->InvalidateChrome();
            }
            return S_OK;
          })
          .Get(),
      &token);
  tab.webview->add_NavigationCompleted(
      Callback<ICoreWebView2NavigationCompletedEventHandler>(
          [owner, id](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
            auto* self = *owner;
            if (!self || self->destroyed_) return S_OK;
            if (Tab* t = self->FindTab(id)) {
              BOOL success=FALSE; args->get_IsSuccess(&success); if(success) self->RecordVisit(*t);
              t->loading = false;
              if (t->openerId) {
                t->openerId = 0;  // 弹出页已经落地：打开者不再需要保护，按标签上限再收一次
                PostMessageW(self->hwnd_, kMsgTabLimit, 0, 0);
              }
              Metrics& metrics = Metrics::Get();
              if (id == self->firstTabId_ && !metrics.Has("ntpNavCompleted")) {
                metrics.Mark("ntpNavCompleted");
              }
              self->InvalidateChrome();
              self->StartExtensions();  // 首个页面落地后再装扩展：不和首屏抢
              if (t->leaseDone) {
                // 新建的智能体标签首次导航（about:blank）完成：交给桥
                LeaseCallback done = std::move(t->leaseDone);
                t->leaseDone = nullptr;
                done(t->id, t->webview.Get(), std::string());
              }
            }
            return S_OK;
          })
          .Get(),
      &token);
  tab.webview->add_NewWindowRequested(
      Callback<ICoreWebView2NewWindowRequestedEventHandler>(
          [owner, id](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
            auto* self = *owner;
            if (!self || self->destroyed_) return S_OK;
            Tab* source = self->FindTab(id);
            if (self->options_.appMode) {
              self->OpenNewWindowAsTab(args, true, id);
              return S_OK;
            }
            CoTaskString uri;
            args->get_Uri(uri.Put());
            const std::wstring target = uri.Str();
            const bool agentSource = source && source->agent;
            // 网页不能弹出内部页面
            if (!agentSource && IsInternalPage(target) && !(source && IsInternalPage(source->url))) {
              args->put_Handled(TRUE);
              return S_OK;
            }
            // WebView2 关掉了 Edge 的弹窗拦截：没经用户点击的 window.open（弹窗广告、跳转劫持）先拦下，
            // 地址栏右侧提示，用户点开再放行；允许过的站点照常
            BOOL user = TRUE;
            args->get_IsUserInitiated(&user);
            if (!user && source && !agentSource && !self->PopupAllowed(source->url)) {
              args->put_Handled(TRUE);
              self->OnPopupBlocked(*source, target);
              return S_OK;
            }
            // 智能体页面弹出的窗口作为普通后台标签打开，不抢用户的视线；Ctrl+点击同主流浏览器开在后台
            const bool background = agentSource || (user && (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0);
            self->OpenNewWindowAsTab(args, !background, id);
            return S_OK;
          })
          .Get(),
      &token);
  tab.webview->add_WindowCloseRequested(
      Callback<ICoreWebView2WindowCloseRequestedEventHandler>([owner, id](ICoreWebView2*, IUnknown*) -> HRESULT {
        auto* self = *owner;
        if (!self || self->destroyed_) return S_OK;
        // 脚本打开的窗口（登录、支付弹窗）完成后 window.close()：关掉这个标签；用户自己开的标签不让网页关
        const Tab* t = self->FindTab(id);
        if (!t || t->agent || !t->scriptOpened || self->options_.appMode) return S_OK;
        self->PostTask([self, id] {
          if (Tab* tab = self->FindTab(id)) {
            const size_t index = self->IndexOf(*tab);
            if (index < self->tabs_.size()) self->CloseTab(index);
          }
        });
        return S_OK;
      }).Get(),
      &token);
  ComPtr<ICoreWebView2_8> audio;
  if (SUCCEEDED(tab.webview.As(&audio))) {
    // 标签出声 / 静音：画扬声器，点一下静音
    auto onAudio = [owner, id](ICoreWebView2* sender, IUnknown*) -> HRESULT {
      auto* self = *owner;
      if (!self || self->destroyed_) return S_OK;
      Tab* t = self->FindTab(id);
      ComPtr<ICoreWebView2_8> view;
      if (!t || FAILED(sender->QueryInterface(IID_PPV_ARGS(view.GetAddressOf())))) return S_OK;
      BOOL playing = FALSE, muted = FALSE;
      view->get_IsDocumentPlayingAudio(&playing);
      view->get_IsMuted(&muted);
      if (t->audible != (playing != FALSE) || t->muted != (muted != FALSE)) {
        t->audible = playing != FALSE;
        t->muted = muted != FALSE;
        self->UpdateMemoryTarget(*t);  // 后台出声的标签不降内存档
        self->Relayout();
        self->InvalidateChrome();
      }
      return S_OK;
    };
    audio->add_IsDocumentPlayingAudioChanged(Callback<ICoreWebView2IsDocumentPlayingAudioChangedEventHandler>(onAudio).Get(),
                                             &token);
    audio->add_IsMutedChanged(Callback<ICoreWebView2IsMutedChangedEventHandler>(onAudio).Get(), &token);
  }
  tab.webview->add_ScriptDialogOpening(
      Callback<ICoreWebView2ScriptDialogOpeningEventHandler>(
          [owner, id](ICoreWebView2*, ICoreWebView2ScriptDialogOpeningEventArgs* args) -> HRESULT {
            auto* self = *owner;
            if (!self || self->destroyed_) return S_OK;
            // 只有关掉了默认弹框的智能体标签会走到这里：离开页面的确认放行，其余按 Playwright 默认当作取消
            COREWEBVIEW2_SCRIPT_DIALOG_KIND kind = COREWEBVIEW2_SCRIPT_DIALOG_KIND_ALERT;
            args->get_Kind(&kind);
            if (kind == COREWEBVIEW2_SCRIPT_DIALOG_KIND_BEFOREUNLOAD) {
              args->Accept();
            }
            (void)id;
            return S_OK;
          })
          .Get(),
      &token);
  tab.webview->add_PermissionRequested(
      Callback<ICoreWebView2PermissionRequestedEventHandler>(
          [owner, id](ICoreWebView2*, ICoreWebView2PermissionRequestedEventArgs* args) -> HRESULT {
            auto* self = *owner;
            if (!self || self->destroyed_) return S_OK;
            const Tab* t = self->FindTab(id);
            if (t && t->agent && !self->IsActive(*t)) {
              args->put_State(COREWEBVIEW2_PERMISSION_STATE_DENY);  // 屏幕外的权限气泡没人能点
            }
            return S_OK;
          })
          .Get(),
      &token);
  tab.webview->add_ProcessFailed(
      Callback<ICoreWebView2ProcessFailedEventHandler>(
          [owner, id](ICoreWebView2* sender, ICoreWebView2ProcessFailedEventArgs* args) -> HRESULT {
            auto* self = *owner;
            if (!self || self->destroyed_) return S_OK;
            COREWEBVIEW2_PROCESS_FAILED_KIND kind = COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED;
            args->get_ProcessFailedKind(&kind);
            self->OnProcessFailed(id, kind, sender);
            return S_OK;
          })
          .Get(),
      &token);
  tab.controller->add_AcceleratorKeyPressed(
      Callback<ICoreWebView2AcceleratorKeyPressedEventHandler>(
          [owner, id](ICoreWebView2Controller*, ICoreWebView2AcceleratorKeyPressedEventArgs* args) -> HRESULT {
            auto* self = *owner;
            if (!self || self->destroyed_) return S_OK;
            const Tab* source = self->FindTab(id);
            if (!source || !self->IsActive(*source)) return S_OK;  // Background input cannot control native chrome.
            COREWEBVIEW2_KEY_EVENT_KIND kind = COREWEBVIEW2_KEY_EVENT_KIND_KEY_UP;
            args->get_KeyEventKind(&kind);
            if (kind != COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN && kind != COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_DOWN) {
              return S_OK;
            }
            UINT key = 0;
            args->get_VirtualKey(&key);
            COREWEBVIEW2_PHYSICAL_KEY_STATUS status{};
            args->get_PhysicalKeyStatus(&status);
            if (self->DispatchShortcut(key, status.WasKeyDown != FALSE)) {
              args->put_Handled(TRUE);
            }
            return S_OK;
          })
          .Get(),
      &token);
  tab.controller->add_GotFocus(Callback<ICoreWebView2FocusChangedEventHandler>(
                                   [owner](ICoreWebView2Controller*, IUnknown*) -> HRESULT {
                                     auto* self = *owner;
                                     if (!self || self->destroyed_) return S_OK;
                                     if(self->extensionPopupWindow_) PostMessageW(self->hwnd_,kMsgCloseExtensionPopup,static_cast<WPARAM>(self->extensionPopupGeneration_),0);
                                     self->focusInWebView_ = true;
                                     return S_OK;
                                   })
                                   .Get(),
                               &token);
}

void BrowserWindow::NavigateNewTabPage(Tab& tab) {
  if (options_.ntp == NtpSource::kInline) {
    if (inlineNtp_.empty()) {
      inlineNtp_ = FromUtf8(kNtpHtml);
    }
    tab.webview->NavigateToString(inlineNtp_.c_str());
  } else {
    tab.allowInternal = true;
    tab.webview->Navigate(kNtpUrl);
  }
}

// 新标签页回报：「ntp-frame <首帧 Unix 毫秒> <timeOrigin>」与「ntp-fcp <FCP Unix 毫秒>」；
// 书签管理页与新标签页发 JSON 字符串（见 OnPageMessage）
void BrowserWindow::OnWebMessage(uint32_t id, ICoreWebView2WebMessageReceivedEventArgs* args) {
  const double hostMs = ProcessClock::NowMs();
  CoTaskString text;
  if (FAILED(args->TryGetWebMessageAsString(text.Put()))) {
    return;
  }
  CoTaskString source;
  args->get_Source(source.Put());
  const std::wstring origin = source.Str();
  if (Tab* tab = FindTab(id); tab && (HandleMediaMessage(*tab, origin, ToUtf8(text.Str())) || HandleTabLimitMessage(*tab, origin, ToUtf8(text.Str())))) return;
  const bool ours = origin.starts_with(kNtpOrigin);
  if (options_.ntp == NtpSource::kFolder && !ours) {
    return;  // 只认新标签页自己的回报
  }
  const std::wstring message = text.Str();
  if (!message.empty() && message.front() == L'{') {
    Tab* tab = FindTab(id);
    if (tab && ours) {
      OnPageMessage(*tab, ToUtf8(message));
    }
    return;
  }
  const double zero = ProcessClock::ZeroEpochMs();
  Metrics& metrics = Metrics::Get();
  if (message.starts_with(L"ntp-frame ")) {
    double frameEpoch = 0;
    double originEpoch = 0;
    if (swscanf_s(message.c_str() + 10, L"%lf %lf", &frameEpoch, &originEpoch) != 2) {
      return;
    }
    if (id == firstTabId_ && !metrics.Has("ntpFirstFrameHost")) {
      metrics.MarkAt("ntpFirstFrameHost", hostMs);
      metrics.MarkAt("ntpFirstFramePage", frameEpoch - zero);
      metrics.MarkAt("ntpTimeOrigin", originEpoch - zero);
      // 首屏画出来之后再建备用控件，不和首屏抢 CPU
      if (options_.spare) {
        CreateSpare();
      }
      if (!options_.metricsPath.empty()) {
        SetTimer(hwnd_, kTimerMetrics, 3000, nullptr);
      }
      MaybeWriteMetrics();
    } else if (id == newTabMeasureId_ && newTabRequestMs_ > 0 && !metrics.Has("newTabFirstFrameHost")) {
      metrics.MarkAt("newTabRequest", newTabRequestMs_);
      metrics.MarkAt("newTabFirstFrameHost", hostMs);
    }
  } else if (message.starts_with(L"ntp-fcp ")) {
    double fcpEpoch = 0;
    if (swscanf_s(message.c_str() + 8, L"%lf", &fcpEpoch) != 1) {
      return;
    }
    if (id == firstTabId_ && !metrics.Has("ntpFcp")) {
      metrics.MarkAt("ntpFcp", fcpEpoch - zero);
      MaybeWriteMetrics();
    }
  }
}

void BrowserWindow::PostToPage(Tab& tab, const std::string& json) {
  if (tab.webview) {
    tab.webview->PostWebMessageAsJson(FromUtf8(json).c_str());
  }
}

// 外壳自带页面（新标签页、书签管理页）的请求
void BrowserWindow::OnPageMessage(Tab& tab, const std::string& text) {
  if (HandleHistoryMessage(tab, text)) return;
  json::Value message;
  if (!json::Parse(text, message)) {
    return;
  }
  const std::string& type = message["type"].AsString();
  if (type.starts_with("ext.") && tab.url.starts_with(L"https://newtab.jisu.example/extensions.html")) {
    if (type == "ext.get") SaveExtensionStatus();
    else if (type == "ext.open" || type == "ext.toggle" || type == "ext.pin") {
      const std::wstring id = FromUtf8(message["id"].AsString());
      for (size_t i = 0; i < extensions_.size(); ++i) {
        auto& e = extensions_[i];
        if (e.id != id || !e.handle) continue;
        if (type == "ext.pin") {
          SetExtensionPinned(i,message["pinned"].AsBool());
        } else if (type == "ext.open") {
          std::wstring page = message["options"].AsBool() ? e.options : e.popup;
          while (!page.empty() && page.front() == L'/') page.erase(page.begin());
          if (!page.empty() && page.find(L":") == std::wstring::npos && page.find(L"..") == std::wstring::npos)
            NewTab(L"chrome-extension://" + e.id + L"/" + page);
        } else {
          const bool enabled = message["enabled"].AsBool();
          if (!enabled && popupExtensionIndex_ == i) CloseExtensionPopup();
          std::weak_ptr<int> alive = alive_;
          const std::wstring extensionId = e.id;
          e.handle->Enable(enabled ? TRUE : FALSE, Callback<ICoreWebView2BrowserExtensionEnableCompletedHandler>(
            [this, extensionId, enabled, alive](HRESULT hr) -> HRESULT {
              if (alive.expired()) return S_OK;
              // 回调到达前扩展列表可能变了（内核重建、移除）：按 id 重新找，不用旧下标
              const auto entry = std::find_if(extensions_.begin(), extensions_.end(),
                                              [&extensionId](const ExtensionEntry& item) { return item.id == extensionId; });
              if (entry == extensions_.end()) return S_OK;
              if (SUCCEEDED(hr)) {
                entry->enabled = enabled;
                std::string text; json::Value c;
                if (ReadFileBytes(profileDir_ + L"\\extensions.json", text) && json::Parse(text,c)) {
                  auto entries = c["extensions"];
                  for (auto& item : entries.MutableItems()) {
                    if (FromUtf8(item["path"].AsString()) == entry->path) item.Set("enabled", json::Value::Bool(enabled));
                  }
                  c.Set("extensions", entries);
                  WriteFileAtomic(profileDir_ + L"\\extensions.json", json::Serialize(c));
                }
              }
              entry->result = hr; SaveExtensionStatus(); return S_OK;
            }).Get());
        }
        break;
      }
    }
    return;
  }
  if (type == "open") {
    const std::wstring url = FromUtf8(message["url"].AsString());
    if (IsWebUrl(url)) {
      OpenUrl(url, message["newTab"].AsBool());
    }
    return;
  }
  if (type.rfind("bm.", 0) != 0 || !IsBookmarksPageUrl(tab.url)) {
    return;  // 书签读写只给书签管理页
  }
  if (!profileLoaded_) {
    return;
  }
  bool changed = false;
  if (type == "bm.save") {
    const auto id = static_cast<int64_t>(message["id"].AsNumber(0));
    const auto parentId = static_cast<int64_t>(message["parent"].AsNumber(1));
    auto* parent = bookmarks_.FindById(parentId);
    auto* node = id ? bookmarks_.FindById(id) : nullptr;
    const bool folder = message["folder"].AsBool();
    const auto title = Trim(FromUtf8(message["title"].AsString()));
    const auto url = Trim(FromUtf8(message["url"].AsString()));
    const auto newFolder = Trim(FromUtf8(message["newFolder"].AsString()));
    bool valid = parent && parent->folder && !title.empty() && title.size() <= 4096 && newFolder.size() <= 200 &&
                 (folder || IsWebUrl(url)) && (!id || (node && !bookmarks_.IsRoot(*node) && node->folder == folder));
    for (const auto* p = parent; valid && p; p = p->parent) if (p == node) valid = false;
    if (valid) {
      if (!newFolder.empty()) parent = bookmarks_.AddFolder(*parent,parent->children.size(),newFolder);
      if (node) {
        bookmarks_.Rename(id,title); if (!folder) bookmarks_.SetUrl(id,url);
        if (node->parent != parent) bookmarks_.Move(id,parent->id,parent->children.size());
      } else if (folder) bookmarks_.AddFolder(*parent,parent->children.size(),title);
      else bookmarks_.AddUrl(*parent,parent->children.size(),title,url);
      settings_.bookmarkFolder = parent->id; SaveSettings();
      OnBookmarksChanged();
    }
    PostToPage(tab, valid ? "{\"type\":\"bm.saved\",\"ok\":true}" :
                          "{\"type\":\"bm.saved\",\"ok\":false,\"error\":\"目标文件夹或输入无效，未修改书签\"}");
    return;
  }
  if (type == "bm.get") {
    PostToPage(tab, "{\"type\":\"bm.tree\",\"tree\":" + bookmarks_.TreeJson() + "}");
  } else if (type == "bm.icons") {
    std::string out = "{\"type\":\"bm.icons\",\"icons\":[";
    bool first = true; size_t count = 0;
    for (const auto& item : message["ids"].Items()) {
      if (++count > 500) break;
      const auto id = static_cast<int64_t>(item.AsNumber());
      const auto* node = bookmarks_.FindById(id);
      if (!node || node->folder) continue;
      const std::wstring file = favicons_.Request(node->url);
      if (file.empty()) continue;
      if (!first) out += ','; first = false;
      out += "{\"id\":" + std::to_string(id) + ",\"src\":";
      json::AppendQuoted(out, "https://favicons.jisu.example/" + ToUtf8(FaviconKey(node->url)) + ".png");
      out += '}';
    }
    PostToPage(tab, out + "]}");
  } else if (type == "bm.add" || type == "bm.addFolder") {
    BookmarkNode* parent = bookmarks_.FindById(static_cast<int64_t>(message["parent"].AsNumber(1)));
    if (parent && parent->folder) {
      const size_t index = static_cast<size_t>(message["index"].AsNumber(static_cast<double>(parent->children.size())));
      const std::wstring title = FromUtf8(message["title"].AsString());
      if (type == "bm.add") {
        // 与 bm.save 同一口径：只收网页地址（不收 data:、file:、搜索词）
        const std::wstring url = ToNavigableUrl(Trim(FromUtf8(message["url"].AsString())));
        changed = IsWebUrl(url) && !url.starts_with(kSearchPrefix) && bookmarks_.AddUrl(*parent, index, title, url) != nullptr;
      } else {
        changed = bookmarks_.AddFolder(*parent, index, title.empty() ? L"新建文件夹" : title) != nullptr;
      }
    }
  } else if (type == "bm.update") {
    const int64_t id = static_cast<int64_t>(message["id"].AsNumber(0));
    if (message["title"].IsString()) {
      changed = bookmarks_.Rename(id, FromUtf8(message["title"].AsString())) || changed;
    }
    if (message["url"].IsString()) {
      const std::wstring url = ToNavigableUrl(Trim(FromUtf8(message["url"].AsString())));
      if (IsWebUrl(url) && !url.starts_with(kSearchPrefix)) changed = bookmarks_.SetUrl(id, url) || changed;
    }
  } else if (type == "bm.remove") {
    changed = bookmarks_.Remove(static_cast<int64_t>(message["id"].AsNumber(0)));
  } else if (type == "bm.move") {
    changed = bookmarks_.Move(static_cast<int64_t>(message["id"].AsNumber(0)),
                              static_cast<int64_t>(message["parent"].AsNumber(0)),
                              static_cast<size_t>(message["index"].AsNumber(1e9)));
  } else if (type == "bm.sources") {
    // 逐个解析各浏览器的书签文件统计条数：放到后台线程，打开导入框不再卡界面
    const uint32_t tabId = tab.id;
    const HWND target = hwnd_;
    std::weak_ptr<int> alive = alive_;
    std::thread([this, target, tabId, alive]() {
      std::string out = "{\"type\":\"bm.sources\",\"sources\":[";
      bool first = true;
      for (const BookmarkImportSource& source : BookmarkStore::DetectImportSources()) {
        out += first ? "" : ",";
        first = false;
        out += "{\"name\":";
        json::AppendQuoted(out, ToUtf8(source.name));
        out += ",\"file\":";
        json::AppendQuoted(out, ToUtf8(source.file));
        out += ",\"urls\":" + std::to_string(source.urls) + "}";
      }
      out += "]}";
      auto* task = new std::function<void()>([this, alive, tabId, out]() {
        if (alive.expired()) return;
        if (Tab* t = FindTab(tabId); t && IsBookmarksPageUrl(t->url)) PostToPage(*t, out);
      });
      if (!PostMessageW(target, kMsgTask, 0, reinterpret_cast<LPARAM>(task))) delete task;
    }).detach();
  } else if (type == "bm.import") {
    // 只接受已知浏览器的书签文件，页面不能指定任意文件；按路径比对，不再把所有浏览器的书签重新解析一遍
    const std::wstring file = FromUtf8(message["file"].AsString());
    std::wstring name = TruncateUtf16(Trim(FromUtf8(message["name"].AsString())), 40);
    if (name.empty()) name = L"其他浏览器";
    std::string error = "unknown import source";
    int count = -1;
    if (BookmarkStore::IsImportSourceFile(file)) {
      error.clear();
      count = bookmarks_.Import(file, name, &error);
    }
    std::string out = "{\"type\":\"bm.imported\",\"count\":" + std::to_string(count) + ",\"error\":";
    json::AppendQuoted(out, error);
    out += "}";
    PostToPage(tab, out);
    if (count >= 0) {
      OnBookmarksChanged();
    }
  } else if (type == "bm.export") {
    std::string out = "{\"type\":\"bm.exported\",\"html\":";
    json::AppendQuoted(out, bookmarks_.ExportHtml());
    out += "}";
    PostToPage(tab, out);
  }
  if (changed) {
    OnBookmarksChanged();
  }
}

void BrowserWindow::OpenNewWindowAsTab(ICoreWebView2NewWindowRequestedEventArgs* args, bool activate, uint32_t openerId) {
  if (options_.appMode) {
    // 应用窗口不开标签：交给同一资料的日常窗口
    args->put_Handled(TRUE);
    BOOL user = FALSE;
    args->get_IsUserInitiated(&user);
    CoTaskString uri;
    if (SUCCEEDED(args->get_Uri(uri.Put()))) {
      OpenFromAppWindow(uri.Str(), user != FALSE);
    }
    return;
  }
  const Tab* opener = FindTab(openerId);
  auto tab = std::make_unique<Tab>();
  tab->id = nextTabId_++;
  tab->openerId = openerId;
  tab->openedBy = openerId;
  tab->scriptOpened = true;
  tab->fromAgent = opener && opener->agent;
  args->GetDeferral(tab->newWindowDeferral.GetAddressOf());
  tab->newWindowArgs = args;
  CoTaskString uri;
  if (SUCCEEDED(args->get_Uri(uri.Put()))) {
    tab->url = uri.Str();
  }
  // 排在打开者右侧（同一打开者连开的几个依次往后），不扔到最末尾
  const size_t index = InsertTab(std::move(tab), InsertIndexFor(openerId));
  Relayout();
  if (activate) {
    Activate(index);
  } else {
    InvalidateChrome();
  }
  CreateController(*tabs_[index]);
  ScheduleSessionSave();
}

// 应用窗口（Pi Web）里的链接：直接交给同一资料已开着的日常窗口；用户点的就把日常窗口带到前面，
// 脚本自己弹的只在后台开标签。日常窗口没开时按同一资料目录启动它
void BrowserWindow::OpenFromAppWindow(const std::wstring& url, bool userInitiated) {
  if (!IsWebUrl(url) || url.size() > 16384) return;
  if (const HWND target = FindRunningWindow(options_.userDataFolder)) {
    DWORD pid = 0;
    GetWindowThreadProcessId(target, &pid);
    if (userInitiated) AllowSetForegroundWindow(pid);
    COPYDATASTRUCT data{};
    data.dwData = userInitiated ? kCopyDataOpenUrl : kCopyDataPassive;
    data.cbData = static_cast<DWORD>((url.size() + 1) * sizeof(wchar_t));
    data.lpData = const_cast<wchar_t*>(url.c_str());
    DWORD_PTR result = 0;
    if (SendMessageTimeoutW(target, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data), SMTO_ABORTIFHUNG, 3000, &result)) {
      if (!userInitiated && result != kCopyDataPassiveAck) {
        data.dwData = kCopyDataOpenUrl;  // 升级前的日常窗口不认后台转发：按普通方式打开，网址不丢
        SendMessageTimeoutW(target, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data), SMTO_ABORTIFHUNG, 3000, &result);
      }
      return;
    }
  }
  std::wstring command;
  if (!userInitiated) command += L"--no-activate ";
  if (!options_.defaultUserData) {
    std::wstring folder = options_.userDataFolder;
    while (!folder.empty() && (folder.back() == L'\\' || folder.back() == L'/')) folder.pop_back();  // 结尾 \" 会吞掉引号
    command += L"--udf \"" + folder + L"\" ";
  }
  command += L"--single-argument " + url;  // 网址原样作为一个参数，夹带不了别的启动参数
  if (userInitiated) AllowSetForegroundWindow(ASFW_ANY);
  ShellExecuteW(hwnd_, L"open", ExePath().c_str(), command.c_str(), nullptr, userInitiated ? SW_SHOWNORMAL : SW_SHOWNOACTIVATE);
}

void BrowserWindow::CreateSpare() {
  if (!environment_ || spare_ || destroyed_) {
    return;
  }
  spare_ = std::make_unique<Tab>();
  spare_->id = nextTabId_++;
  CreateController(*spare_);
}

// ---------------------------------------------------------------------------
// 智能体标签池（AgentHost）

void BrowserWindow::LeaseAgentTab(LeaseCallback done) {
  if (!environment_ || destroyed_) {
    done(0, nullptr, "Browser engine is not ready");
    return;
  }
  // 复用空闲的智能体标签：不在租用中、用户没在看、控件已就绪
  for (auto& tab : tabs_) {
    if (tab->agent && !tab->leased && !IsActive(*tab) && tab->webview && !tab->leaseDone) {
      tab->leased = true;
      UpdateMemoryTarget(*tab);  // 租出去干活：恢复正常内存档
      InvalidateChrome();
      done(tab->id, tab->webview.Get(), std::string());
      return;
    }
  }
  // 没有就新建一个后台标签（不激活），首次导航完成后交给桥
  auto tab = std::make_unique<Tab>();
  tab->id = nextTabId_++;
  tab->agent = true;
  tab->leased = true;
  tab->pendingUrl = L"about:blank";
  tab->url = L"about:blank";
  tab->title = kAgentTabTitle;
  tab->leaseDone = std::move(done);
  Tab& created = *tab;
  tabs_.push_back(std::move(tab));
  Relayout();
  InvalidateChrome();
  CreateController(created);
}

void BrowserWindow::ReturnAgentTab(uint32_t tabId) {
  if (Tab* tab = FindTab(tabId)) {
    tab->leased = false;
    tab->agentAwake = false;
    PlaceTab(*tab);
    UpdateMemoryTarget(*tab);  // 回池里闲着：降到低内存档
    InvalidateChrome();
  }
}

bool BrowserWindow::IsAgentTabViewed(uint32_t tabId) {
  const Tab* tab = FindTab(tabId);
  return tab && IsActive(*tab) && !IsIconic(hwnd_);
}

void BrowserWindow::ResumeAgentTab(uint32_t tabId) {
  Tab* tab = FindTab(tabId);
  if (!tab || !tab->agent) {
    return;
  }
  ResumeTab(*tab);
  if (!tab->visible) {
    tab->hiddenSince = GetTickCount64();  // 刚有过活动：挂起计时从现在重新算
    if (agentConfig_.freezeMs > 0 && !freezeTimerRunning_) {
      freezeTimerRunning_ = SetTimer(hwnd_, kTimerAgentFreeze, 1000, nullptr) != 0;
    }
  }
}

// 智能体标签在宿主里按配置的光栅化比例出图（CSS 视口不变，像素更少）；回到主窗口时跟随显示器
void BrowserWindow::ApplyAgentScale(Tab& tab, bool inHost) {
  ComPtr<ICoreWebView2Controller3> controller3;
  if (!tab.controller || FAILED(tab.controller.As(&controller3))) {
    return;
  }
  const bool fixed = inHost && agentConfig_.rasterScale > 0;
  const double target = fixed ? agentConfig_.rasterScale : static_cast<double>(dpi_) / 96.0;
  double current = 0;
  controller3->get_RasterizationScale(&current);
  controller3->put_ShouldDetectMonitorScaleChanges(fixed ? FALSE : TRUE);
  if (std::fabs(current - target) > 0.001) {
    controller3->put_RasterizationScale(target);
  }
}

void BrowserWindow::ResumeTab(Tab& tab) {
  if (!tab.suspended && !tab.suspending) {
    return;
  }
  tab.suspended = false;
  tab.suspending = false;
  ComPtr<ICoreWebView2_3> webview3;
  if (tab.webview && SUCCEEDED(tab.webview.As(&webview3))) {
    webview3->Resume();
  }
}

// 隐藏够 freezeMs 的智能体标签挂起：定时器、脚本全停，下次有命令时先恢复
void BrowserWindow::SuspendIdleAgentTabs() {
  bool waiting = false;
  const ULONGLONG now = GetTickCount64();
  for (auto& item : tabs_) {
    Tab& tab = *item;
    if (!tab.agent || tab.agentAwake || IsActive(tab) || tab.visible || tab.suspended || tab.suspending ||
        !tab.webview) {
      continue;
    }
    if (now - tab.hiddenSince < agentConfig_.freezeMs) {
      waiting = true;
      continue;
    }
    ComPtr<ICoreWebView2_3> webview3;
    if (FAILED(tab.webview.As(&webview3))) {
      continue;
    }
    tab.suspending = true;
    const uint32_t id = tab.id;
    std::weak_ptr<int> alive = alive_;  // 回调到达前窗口可能已关：先看它还在不在
    webview3->TrySuspend(Callback<ICoreWebView2TrySuspendCompletedHandler>(
                             [this, id, alive](HRESULT result, BOOL ok) -> HRESULT {
                               if (alive.expired()) {
                                 return S_OK;
                               }
                               Tab* t = FindTab(id);
                               if (!t) {
                                 return S_OK;
                               }
                               if (t->suspending) {
                                 t->suspending = false;
                                 t->suspended = SUCCEEDED(result) && ok;
                               } else if (SUCCEEDED(result) && ok) {
                                 // 挂起途中已经被唤醒：补一次恢复，免得页面停在挂起状态
                                 ComPtr<ICoreWebView2_3> again;
                                 if (t->webview && SUCCEEDED(t->webview.As(&again))) {
                                   again->Resume();
                                 }
                               }
                               return S_OK;
                             })
                             .Get());
  }
  if (!waiting && freezeTimerRunning_) {
    KillTimer(hwnd_, kTimerAgentFreeze);
    freezeTimerRunning_ = false;
  }
}

void BrowserWindow::SetAgentTabAwake(uint32_t tabId, bool awake) {
  Tab* tab = FindTab(tabId);
  if (!tab || tab->agentAwake == awake) {
    return;
  }
  tab->agentAwake = awake;
  PlaceTab(*tab);
  InvalidateChrome();
}

std::string BrowserWindow::AgentTabInfo(uint32_t tabId) {
  const Tab* tab = FindTab(tabId);
  const size_t index = tab ? IndexOf(*tab) : 0;
  const SIZE viewport = AgentViewport();
  const bool audible = tab && tab->audible;
  const bool muted = tab && tab->muted;
  std::string out = "{\"id\":" + std::to_string(tabId) + ",\"index\":" + std::to_string(index) +
                    ",\"windowId\":1,\"groupId\":-1,\"active\":false,\"highlighted\":false,\"pinned\":false,"
                    "\"audible\":" + (audible ? "true" : "false") +
                    ",\"discarded\":false,\"autoDiscardable\":true,\"frozen\":false,"
                    "\"incognito\":false,\"mutedInfo\":{\"muted\":" + (muted ? "true" : "false") +
                    "},\"status\":\"" + (tab && tab->loading ? "loading" : "complete") + "\",\"width\":" +
                    std::to_string(viewport.cx) + ",\"height\":" + std::to_string(viewport.cy) + ",\"url\":";
  json::AppendQuoted(out, tab ? ToUtf8(tab->url) : std::string("about:blank"));
  out += ",\"title\":";
  json::AppendQuoted(out, tab ? ToUtf8(tab->title) : std::string());
  out += "}";
  return out;
}

LRESULT CALLBACK BrowserWindow::AgentHostProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  switch (msg) {
    case WM_MOUSEACTIVATE:
      return MA_NOACTIVATE;
    case WM_ERASEBKGND:
      return 1;
    default:
      return DefWindowProcW(hwnd, msg, wParam, lParam);
  }
}

// 屏幕外宿主：可见但不在任何显示器上、不激活、不进任务栏和 Alt+Tab。WebView2 的隐藏控件不出帧，
// 截图、rAF 都会卡住，所以智能体标签执行命令时要在这里「可见」
HWND BrowserWindow::EnsureAgentHost() {
  if (agentHost_) {
    return agentHost_;
  }
  static bool registered = false;
  if (!registered) {
    WNDCLASSEXW hostClass{};
    hostClass.cbSize = static_cast<UINT>(sizeof(hostClass));
    hostClass.lpfnWndProc = &BrowserWindow::AgentHostProc;
    hostClass.hInstance = instance_;
    hostClass.lpszClassName = kAgentHostClass;
    registered = RegisterClassExW(&hostClass) != 0;
  }
  const SIZE size = AgentHostSize();
  const int x = GetSystemMetrics(SM_XVIRTUALSCREEN) - size.cx - 4000;
  const int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
  agentHost_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kAgentHostClass, L"极速浏览器 · 智能体",
                               WS_POPUP | WS_CLIPCHILDREN, x, y, size.cx, size.cy, nullptr, nullptr, instance_, nullptr);
  if (agentHost_) {
    ShowWindow(agentHost_, SW_SHOWNOACTIVATE);
  }
  return agentHost_;
}

// 智能体标签的 CSS 视口：与主窗口网页区相同（主窗口太小或最小化时用 1280×800）
SIZE BrowserWindow::AgentViewport() const {
  const RECT content = ContentRect();
  const double scale = static_cast<double>(dpi_) / 96.0;
  SIZE viewport{static_cast<LONG>(std::lround((content.right - content.left) / scale)),
                static_cast<LONG>(std::lround((content.bottom - content.top) / scale))};
  if (viewport.cx < 320 || viewport.cy < 240) {
    viewport = SIZE{1280, 800};
  }
  return viewport;
}

// 宿主窗口的物理像素：CSS 视口 × 光栅化比例
SIZE BrowserWindow::AgentHostSize() const {
  const SIZE viewport = AgentViewport();
  const double scale = agentConfig_.rasterScale > 0 ? agentConfig_.rasterScale : static_cast<double>(dpi_) / 96.0;
  return SIZE{static_cast<LONG>(std::lround(viewport.cx * scale)), static_cast<LONG>(std::lround(viewport.cy * scale))};
}

RECT BrowserWindow::AgentHostRect() const {
  RECT rect{0, 0, 0, 0};
  if (agentHost_) {
    GetClientRect(agentHost_, &rect);
  } else {
    const SIZE size = AgentHostSize();
    rect.right = size.cx;
    rect.bottom = size.cy;
  }
  return rect;
}

// ---------------------------------------------------------------------------
// 标签

void BrowserWindow::NewTab(const std::wstring& url, bool activate, size_t at) {
  std::unique_ptr<Tab> tab;
  if (url.empty() && spare_ && spare_->controller) {
    tab = std::move(spare_);  // 备用控件已建好并加载了新标签页，直接换上
    Metrics::Get().Mark("spareAdopted");
  } else {
    tab = std::make_unique<Tab>();
    tab->id = nextTabId_++;
    tab->pendingUrl = url;
    tab->url = url;
  }
  if (newTabMeasureId_ == 0 && activate) {
    newTabRequestMs_ = ProcessClock::NowMs();
    newTabMeasureId_ = tab->id;
  }
  const size_t index = InsertTab(std::move(tab), at);
  Relayout();
  if (activate) {
    Activate(index);
  } else {
    InvalidateChrome();
  }
  CreateController(*tabs_[index]);  // 已有控件或正在创建时什么也不做
  if (options_.spare && !spare_) {
    SetTimer(hwnd_, kTimerSpare, 500, nullptr);
  }
  if (url.empty() && activate) {
    FocusAddressBar();
  }
  ScheduleSessionSave();
}

size_t BrowserWindow::InsertTab(std::unique_ptr<Tab> tab, size_t at) {
  size_t pinned = 0;
  while (pinned < tabs_.size() && tabs_[pinned]->pinned) ++pinned;
  at = std::min(at, tabs_.size());
  at = tab->pinned ? std::min(at, pinned) : std::max(at, pinned);  // 固定的一组在最左边，两组不互相插
  const bool shiftsActive = !tabs_.empty() && at <= active_;
  tabs_.insert(tabs_.begin() + static_cast<std::ptrdiff_t>(at), std::move(tab));
  if (shiftsActive) ++active_;
  return at;
}

void BrowserWindow::CloseTab(size_t index) {
  if (index >= tabs_.size()) {
    return;
  }
  CancelTabDrag(false);
  if (fullscreenTab_ == tabs_[index]->id) ExitContentFullscreen();
  StopMedia(*tabs_[index]);
  if(auto* target=FindTab(tabs_[index]->mediaTarget))target->mediaSource=0;
  RememberClosedTab(*tabs_[index]);  // Ctrl+Shift+T 重新打开
  const bool wasActive = index == active_;
  std::unique_ptr<Tab> closing = std::move(tabs_[index]);
  tabs_.erase(tabs_.begin() + static_cast<std::ptrdiff_t>(index));
  if (closing->leaseDone) {
    LeaseCallback done = std::move(closing->leaseDone);
    closing->leaseDone = nullptr;
    done(0, nullptr, "Background tab was closed before it was ready");
  }
  if (closing->agent && bridge_) {
    bridge_->OnAgentTabGone(closing->id);
  }
  *closing->owner = nullptr;
  // 打开者关掉之前，先把它还没交接的弹出请求放弃掉、改成普通导航：
  // 打开者没了再 put_NewWindow，WebView2 内核会空指针崩溃（EmbeddedBrowserWebView.dll 0xc0000005）
  for (auto& tab : tabs_) {
    if (tab->newWindowArgs && tab->openerId == closing->id) {
      AbandonNewWindow(*tab);
      if (tab->pendingUrl.empty()) tab->pendingUrl = tab->url;
    }
  }
  AbandonNewWindow(*closing);
  if (closing->controller) {
    closing->controller->Close();
  }
  if (tabs_.empty()) {
    DestroyWindow(hwnd_);
    return;
  }
  hover_ = {};
  if (wasActive) {
    Relayout();
    Activate(std::min(index, tabs_.size() - 1));
  } else {
    // 关的是后台标签（自动关旧标签、智能体标签、网页 window.close）：当前标签不变，
    // 不重新激活——地址栏里正在输入的内容和打开着的扩展弹层都留着
    if (index < active_) --active_;
    Relayout();
    InvalidateChrome();
  }
  ScheduleSessionSave();
}

void BrowserWindow::Activate(size_t index) {
  if (index >= tabs_.size()) {
    return;
  }
  CloseExtensionPopup();
  if (fullscreenTab_ && fullscreenTab_ != tabs_[index]->id) ExitContentFullscreen();
  if (auto* old=ActiveTab();old&&old->media&&index!=active_&&fullscreen_.Active())ExitFullscreen();
  active_ = index;
  Tab& tab = *tabs_[index];
  if (tab.lazy) {
    tab.lazy = false;  // 恢复出来的后台标签第一次看：这时才建网页
    CreateController(tab);
  }
  if (tab.crashed && tab.webview) {
    tab.crashed = false;  // 在后台时渲染进程崩了：切过来重新加载
    tab.webview->Reload();
  }
  PostMessageW(hwnd_, kMsgTabLimit, 0, 0);
  // 先显示新标签再隐藏旧的，中间不露底色
  PlaceTab(tab);
  for (auto& other : tabs_) {
    if (other.get() != &tab) {
      PlaceTab(*other);
    }
  }
  editing_ = false;
  Relayout();  // 地址框里的「已拦截弹出窗口」跟着当前标签走
  UpdateAddressBar();
  UpdateWindowTitle();
  InvalidateChrome();
  FocusActiveContent();
  ScheduleSessionSave();
}

// 控件该在哪、显不显示：当前标签在主窗口里显示；醒着的智能体标签在屏幕外宿主里显示；其余隐藏
void BrowserWindow::PlaceTab(Tab& tab) {
  if (!tab.controller) {
    return;
  }
  const bool shown = IsActive(tab) && options_.preheatSeconds <= 0 && !IsIconic(hwnd_);
  if (shown) {
    if (tab.parent != hwnd_) {
      tab.controller->put_ParentWindow(hwnd_);
      tab.parent = hwnd_;
    }
    if (tab.agent) {
      ApplyAgentScale(tab, false);
      if (bridge_) {
        bridge_->OnAgentTabShown(tab.id);  // 用户点开看：冻结的页面先解冻
      }
    }
    auto bounds = ContentRect();
    if (tab.mediaSurface) {
      auto video = bounds;
      const LONG controls = static_cast<LONG>((bounds.right < MulDiv(600,dpi_,96) ? 280 : 222) * (dpi_/96.0));
      video.bottom = fullscreen_.Active() ? bounds.bottom : (std::max)(bounds.top+40L, bounds.bottom-controls);
      SetWindowPos(tab.mediaSurface,nullptr,video.left,video.top,video.right-video.left,video.bottom-video.top,SWP_NOACTIVATE|SWP_NOZORDER|SWP_SHOWWINDOW);
      if(HWND surface=FindWindowExW(tab.mediaSurface,nullptr,L"JisuMediaSurface",nullptr))SetWindowPos(surface,nullptr,0,0,video.right-video.left,video.bottom-video.top,SWP_NOACTIVATE|SWP_NOZORDER|SWP_ASYNCWINDOWPOS);
      bounds.top=video.bottom;
      tab.controller->put_Bounds(bounds);
      SetTabVisible(tab,!fullscreen_.Active());
    } else {
      tab.controller->put_Bounds(bounds);
      SetTabVisible(tab, true);
    }
    tab.suspended = false;  // 控件可见时内核自动恢复
    tab.suspending = false;
    return;
  }
  if (tab.mediaSurface) ShowWindow(tab.mediaSurface,SW_HIDE);
  if (tab.agent) {
    HWND host = EnsureAgentHost();
    if (host && tab.parent != host) {
      SetTabVisible(tab, false);  // 先藏再换父窗口，不在主窗口里闪一下
      tab.controller->put_ParentWindow(host);
      tab.parent = host;
    }
    if (host) {
      ApplyAgentScale(tab, true);
      tab.controller->put_Bounds(AgentHostRect());
    }
    if (tab.agentAwake) {
      ResumeTab(tab);  // 先恢复再显示：命令到达时页面已经在跑
    }
    const bool wasVisible = tab.visible;
    SetTabVisible(tab, tab.agentAwake);
    if (!tab.agentAwake && (wasVisible || tab.hiddenSince == 0)) {
      tab.hiddenSince = GetTickCount64();
      if (agentConfig_.freezeMs > 0 && !freezeTimerRunning_) {
        freezeTimerRunning_ = SetTimer(hwnd_, kTimerAgentFreeze, 1000, nullptr) != 0;
      }
    }
    return;
  }
  SetTabVisible(tab, false);
}

void BrowserWindow::PlaceAllTabs() {
  if (Tab* active = ActiveTab()) {
    PlaceTab(*active);
  }
  for (auto& tab : tabs_) {
    if (!IsActive(*tab)) {
      PlaceTab(*tab);
    }
  }
}

void BrowserWindow::SetTabVisible(Tab& tab, bool visible) {
  if (!tab.controller || tab.visible == visible) {
    return;
  }
  tab.controller->put_IsVisible(visible ? TRUE : FALSE);
  tab.visible = visible;
  UpdateMemoryTarget(tab);  // 看不见的标签降到低内存档，显示时恢复
}

BrowserWindow::Tab* BrowserWindow::FindTab(uint32_t id) {
  for (auto& tab : tabs_) {
    if (tab->id == id) {
      return tab.get();
    }
  }
  if (spare_ && spare_->id == id) {
    return spare_.get();
  }
  // Native media/source links survive moving either tab into a sibling window.
  for (auto* window : liveWindows_) {
    if (window == this || window->profile_ != profile_) continue;
    for (auto& tab : window->tabs_) if (tab->id == id) return tab.get();
  }
  return nullptr;
}

size_t BrowserWindow::IndexOf(const Tab& tab) const {
  for (size_t i = 0; i < tabs_.size(); ++i) {
    if (tabs_[i].get() == &tab) {
      return i;
    }
  }
  return tabs_.size();
}

BrowserWindow::Tab* BrowserWindow::ActiveTab() { return active_ < tabs_.size() ? tabs_[active_].get() : nullptr; }

const BrowserWindow::Tab* BrowserWindow::ActiveTab() const {
  return active_ < tabs_.size() ? tabs_[active_].get() : nullptr;
}

bool BrowserWindow::IsActive(const Tab& tab) const { return ActiveTab() == &tab; }

bool BrowserWindow::IsSpare(const Tab& tab) const { return spare_.get() == &tab; }

// ---------------------------------------------------------------------------
// 地址栏与快捷键

bool BrowserWindow::PreTranslate(const MSG& msg) {
  if (root_ == this) {
    for (auto* window : liveWindows_) {
      if (window != this && (msg.hwnd == window->hwnd_ || msg.hwnd == window->edit_ || IsChild(window->hwnd_, msg.hwnd)))
        return window->PreTranslate(msg);
    }
  }
  if (msg.message != WM_KEYDOWN && msg.message != WM_SYSKEYDOWN) {
    return false;
  }
  // 地址栏、扩展按钮等本窗口的子窗口拿着焦点时，外壳快捷键照样生效
  if (msg.hwnd != hwnd_ && msg.hwnd != edit_ && !IsChild(hwnd_, msg.hwnd)) {
    return false;
  }
  const UINT key = static_cast<UINT>(msg.wParam);
  if (key == VK_ESCAPE && dragTabId_) { CancelTabDrag(); return true; }
  if (key == VK_ESCAPE && extensionPopupWindow_) { CloseExtensionPopup(); return true; }
  if (key == VK_ESCAPE && dragBookmarkId_) { CancelBookmarkDrag(); return true; }
  const bool ctrl = GetKeyState(VK_CONTROL) < 0;
  const bool alt = GetKeyState(VK_MENU) < 0;
  if (msg.hwnd == edit_ && key == VK_RETURN) {
    if (msg.message == WM_SYSKEYDOWN && alt && !ctrl) {
      NavigateFromAddressBar(true);  // Alt+Enter：在新标签页打开
      return true;
    }
    if (msg.message == WM_KEYDOWN && ctrl && !alt) {
      // Ctrl+Enter：只输了一个词（如 taobao）就补成 www.taobao.com
      const int length = GetWindowTextLengthW(edit_);
      std::wstring text(static_cast<size_t>(length) + 1, L'\0');
      GetWindowTextW(edit_, text.data(), length + 1);
      text.resize(static_cast<size_t>(length));
      text = Trim(text);
      if (!text.empty() && text.find_first_of(L"./:@ \t") == std::wstring::npos) {
        SetWindowTextW(edit_, (L"www." + text + L".com").c_str());
      }
      NavigateFromAddressBar();
      return true;
    }
  }
  const bool repeat = (msg.lParam & (1 << 30)) != 0;
  if (DispatchShortcut(key, repeat)) {
    return true;
  }
  if (msg.hwnd == edit_ && msg.message == WM_KEYDOWN) {
    if (key == VK_RETURN) {
      NavigateFromAddressBar();
      return true;  // 吞掉回车：单行 EDIT 收到 WM_CHAR '\r' 会响一声
    }
    if (key == VK_ESCAPE) {
      editing_ = false;
      UpdateAddressBar();
      SendMessageW(edit_, EM_SETSEL, 0, -1);
      return true;
    }
    if (key == 'A' && ctrl) {
      SendMessageW(edit_, EM_SETSEL, 0, -1);
      return true;
    }
    if (key == VK_BACK && ctrl && !alt) {
      DeletePreviousWord();  // 系统 EDIT 的 Ctrl+Backspace 会插入一个方框字符
      return true;
    }
  }
  return false;
}

bool BrowserWindow::DispatchShortcut(UINT key, bool repeat) {
  // WebView-focused keyboard events arrive through AcceleratorKeyPressed too.
  if (key == VK_ESCAPE && dragTabId_) { CancelTabDrag(); return true; }
  const bool ctrl = GetKeyState(VK_CONTROL) < 0;
  const bool shift = GetKeyState(VK_SHIFT) < 0;
  const bool alt = GetKeyState(VK_MENU) < 0;
  // App windows have one page: let Pi handle its own shortcuts, without browser tab/omnibox commands.
  if (options_.appMode && ((ctrl && key != 'R' && key != 'W' && key != VK_F4) || (alt && key == 'D') || key == VK_F6)) return false;
  WPARAM command = kCmdNone;
  LPARAM arg = 0;
  bool once = false;  // 按住不连发
  if (ctrl && !alt) {
    switch (key) {
      case 'T':
        command = shift ? kCmdReopenClosed : kCmdNewTab;  // Ctrl+Shift+T：重新打开关闭的标签
        once = true;
        break;
      case 'W':
      case VK_F4:
        command = kCmdCloseTab;
        once = true;
        break;
      case VK_TAB:
        command = shift ? kCmdPrevTab : kCmdNextTab;
        break;
      case VK_NEXT:  // Ctrl+PageDown / PageUp：下一个 / 上一个标签
        command = kCmdNextTab;
        break;
      case VK_PRIOR:
        command = kCmdPrevTab;
        break;
      case 'H':
        command = kCmdHistory;
        once = true;
        break;
      case 'M':
        if (shift) { command = kCmdMedia; once = true; }
        break;
      case 'L':
        command = kCmdFocusAddress;
        break;
      case 'R':
        command = shift ? kCmdHardReload : kCmdReload;  // Ctrl+Shift+R：跳过缓存
        break;
      case VK_F5:
        command = kCmdHardReload;
        break;
      case 'D':
        command = kCmdToggleBookmark;
        once = true;
        break;
      case 'B':
        if (shift) {
          command = kCmdToggleBookmarkBar;
          once = true;
        }
        break;
      case 'O':
        if (shift) {
          command = kCmdBookmarkManager;
          once = true;
        }
        break;
      default:
        // Ctrl+1…8 第几个标签，Ctrl+9 最后一个
        if (!shift && ((key >= '1' && key <= '9') || (key >= VK_NUMPAD1 && key <= VK_NUMPAD9))) {
          command = kCmdSelectTab;
          arg = static_cast<LPARAM>(key >= VK_NUMPAD1 ? key - VK_NUMPAD0 : key - '0');
          once = true;
        }
        break;
    }
  } else if (alt && !ctrl) {
    switch (key) {
      case VK_LEFT:
        command = kCmdBack;
        break;
      case VK_RIGHT:
        command = kCmdForward;
        break;
      case 'D':
        command = kCmdFocusAddress;
        break;
      default:
        break;
    }
  } else if (!ctrl && !alt) {
    switch (key) {
      case VK_F11:
        command = kCmdToggleFullscreen;
        once = true;
        break;
      case VK_ESCAPE:
        if (fullscreen_.Active()) {
          command = kCmdExitFullscreen;
          once = true;
        }
        break;
      case VK_F5:
        command = shift ? kCmdHardReload : kCmdReload;  // Shift+F5：跳过缓存
        break;
      case VK_F6:
        command = kCmdFocusAddress;
        break;
      default:
        break;
    }
  }
  if (command == kCmdNone) {
    return false;
  }
  // 统一投递到消息队列再执行：不在 WebView2 的事件回调里直接关掉触发事件的控件
  if (!(once && repeat)) {
    PostMessageW(hwnd_, kMsgCommand, command, arg);
  }
  return true;
}

void BrowserWindow::PostMediaShortcut(const std::string& shortcut) {
  WPARAM command=kCmdNone;
  if(shortcut=="history")command=kCmdHistory;else if(shortcut=="address")command=kCmdFocusAddress;
  else if(shortcut=="new-tab")command=kCmdNewTab;else if(shortcut=="close-tab")command=kCmdCloseTab;
  else if(shortcut=="next-tab")command=kCmdNextTab;else if(shortcut=="prev-tab")command=kCmdPrevTab;
  else if(shortcut=="reload")command=kCmdReload;else if(shortcut=="bookmark")command=kCmdToggleBookmark;
  else if(shortcut=="media")command=kCmdMedia;
  if(command!=kCmdNone)PostMessageW(hwnd_,kMsgCommand,command,0);
}

void BrowserWindow::ExecuteCommand(WPARAM command, LPARAM arg) {
  Tab* tab = ActiveTab();
  // 网页控件没建起来的标签：刷新就重新创建（内核整个没了就重建内核）
  auto recreate = [this](Tab& target) {
    if (target.controller || target.creating) return false;
    target.retried = false;
    target.error.clear();
    target.lazy = false;
    if (environment_) {
      CreateController(target);
    } else if (!options_.appMode) {
      // 内核整个没了：用户主动刷新就由主窗口马上重试一次，不等退避（也不把退避越推越长）
      BrowserWindow& primary = PrimaryWindow();
      KillTimer(primary.hwnd_, kTimerRecover);
      primary.RecoverEnvironment();
    }
    InvalidateChrome();
    return true;
  };
  switch (command) {
    case kCmdHistory:
      OpenHistory();
      break;
    case kCmdMedia:
      OpenMedia();
      break;
    case kCmdToggleFullscreen:
      if (fullscreen_.Active()) ExitFullscreen();
      else { browserFullscreen_ = true; UpdateFullscreen(); }
      break;
    case kCmdExitFullscreen:
      ExitFullscreen();
      break;
    case kCmdNewTab:
      NewTab(L"");
      break;
    case kCmdReopenClosed:
      ReopenClosedTab();
      break;
    case kCmdCloseTab:
      CloseTab(active_);
      break;
    case kCmdNextTab:
      if (!tabs_.empty()) {
        Activate((active_ + 1) % tabs_.size());
      }
      break;
    case kCmdPrevTab:
      if (!tabs_.empty()) {
        Activate((active_ + tabs_.size() - 1) % tabs_.size());
      }
      break;
    case kCmdSelectTab: {
      const size_t number = static_cast<size_t>(arg);
      if (number == 9 && !tabs_.empty()) {
        Activate(tabs_.size() - 1);
      } else if (number >= 1 && number - 1 < tabs_.size()) {
        Activate(number - 1);
      }
      break;
    }
    case kCmdFocusAddress:
      FocusAddressBar();
      break;
    case kCmdReload:
      if (tab && !recreate(*tab) && tab->webview) {
        tab->webview->Reload();
      }
      break;
    case kCmdHardReload:
      if (tab && !recreate(*tab)) {
        HardReload();
      }
      break;
    case kCmdStopOrReload:
      if (tab && !recreate(*tab) && tab->webview) {
        if (tab->loading) {
          tab->webview->Stop();
        } else {
          tab->webview->Reload();
        }
      }
      break;
    case kCmdBack:
      if (tab && tab->webview) {
        tab->webview->GoBack();
      }
      break;
    case kCmdForward:
      if (tab && tab->webview) {
        tab->webview->GoForward();
      }
      break;
    case kCmdToggleBookmark:
      ToggleBookmark(true);
      break;
    case kCmdToggleBookmarkBar:
      settings_.bookmarkBar = !settings_.bookmarkBar;
      SaveSettings();
      OnBookmarksChanged();
      break;
    case kCmdBookmarkManager:
      OpenBookmarkManager(std::wstring());
      break;
    default:
      break;
  }
}

void BrowserWindow::FocusAddressBar() {
  if (fullscreen_.Active()) ExitFullscreen();
  SetFocus(edit_);
  SendMessageW(edit_, EM_SETSEL, 0, -1);
}

void BrowserWindow::UpdateAddressBar() {
  if (editing_ || !edit_) {
    return;
  }
  const Tab* tab = ActiveTab();
  const std::wstring text = tab ? DisplayUrl(tab->url) : std::wstring();
  const int length = GetWindowTextLengthW(edit_);
  std::wstring current(static_cast<size_t>(length) + 1, L'\0');
  GetWindowTextW(edit_, current.data(), length + 1);
  current.resize(static_cast<size_t>(length));
  if (current == text) {
    return;  // 没变就不重设：光标、选区不被打回开头
  }
  // 用户刚按 Ctrl+L 全选、页面这时跳转：换上新网址后保持全选，接着输入是替换而不是追加
  DWORD start = 0, end = 0;
  SendMessageW(edit_, EM_GETSEL, reinterpret_cast<WPARAM>(&start), reinterpret_cast<LPARAM>(&end));
  const bool allSelected = GetFocus() == edit_ && length > 0 && start == 0 && end == static_cast<DWORD>(length);
  settingAddress_ = true;
  SetWindowTextW(edit_, text.c_str());
  settingAddress_ = false;
  if (allSelected) {
    SendMessageW(edit_, EM_SETSEL, 0, -1);
  }
}

void BrowserWindow::UpdateWindowTitle() {
  if (options_.appMode) { SetWindowTextW(hwnd_, options_.appTitle.c_str()); return; }
  const Tab* tab = ActiveTab();
  const std::wstring title =
      (tab && !tab->title.empty() ? tab->title : std::wstring(kNewTabTitle)) + L" - " + kAppName;
  SetWindowTextW(hwnd_, title.c_str());
}

void BrowserWindow::NavigateFromAddressBar(bool newTab) {
  const int length = GetWindowTextLengthW(edit_);
  std::wstring text(static_cast<size_t>(length) + 1, L'\0');
  GetWindowTextW(edit_, text.data(), length + 1);
  text.resize(static_cast<size_t>(length));
  text = Trim(text);
  Tab* tab = ActiveTab();
  if (text.empty() || !tab) {
    return;
  }
  const std::wstring url = ToNavigableUrl(text);
  editing_ = false;
  // Alt+Enter，或当前是智能体正在用的标签：开新标签，不抢它的页面
  if (newTab || !AdoptAgentTab(*tab)) {
    NewTab(url, true);
    UpdateAddressBar();
    return;
  }
  NavigateTab(*tab, url);
  if (tab->controller) {
    tab->controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
  } else {
    tab->focusOnCreate = true;
  }
  UpdateAddressBar();
  InvalidateChrome();
}

void BrowserWindow::OpenUrl(const std::wstring& url, bool newTab) {
  Tab* tab = ActiveTab();
  // 智能体正在用的标签不动；空闲的智能体标签被用户拿来打开网页，就归用户（不再被 Pi 租走导航掉）
  if (newTab || !tab || !AdoptAgentTab(*tab)) {
    NewTab(url, true);
    return;
  }
  NavigateTab(*tab, url);
  UpdateAddressBar();
  InvalidateChrome();
}

// ---------------------------------------------------------------------------
// 书签

void BrowserWindow::ToggleBookmark(bool fromKeyboard) {
  Tab* tab = ActiveTab();
  if (!tab || !profileLoaded_ || !IsStarrable(tab->url)) {
    return;
  }
  (void)fromKeyboard;
  EditBookmark(bookmarks_.FindByUrl(tab->url), tab->title.empty() ? tab->url : tab->title, tab->url);
}

void BrowserWindow::EditBookmark(BookmarkNode* node, const std::wstring& title, const std::wstring& url) {
  const int64_t id = node ? node->id : 0;
  const int64_t parent = node && node->parent ? node->parent->id : settings_.bookmarkFolder;
  const auto edit = ShowBookmarkEditor(hwnd_, instance_, bookmarks_, node ? node->title : title,
                                        node ? node->url : url, parent, node != nullptr);
  if (!edit) return;
  const int64_t applied = ApplyBookmarkEdit(bookmarks_, *edit, id);
  if (!applied) {
    MessageBoxW(hwnd_, L"目标文件夹已变化，书签未保存，请重试。", L"保存书签", MB_OK | MB_ICONWARNING);
    return;
  }
  if (!edit->remove) {
    if (const auto* saved = bookmarks_.FindById(applied); saved && saved->parent) settings_.bookmarkFolder = saved->parent->id;
    SaveSettings();
  }
  OnBookmarksChanged();
}

// 书签菜单：网址项编号 kMenuItemBase + 序号；有两个以上网址的文件夹末尾加「全部打开」
void BrowserWindow::ShowBookmarkFolderMenu(BookmarkNode& folder, POINT screen) {
  std::vector<BookmarkNode*> nodes;
  for (auto& child : folder.children) {
    nodes.push_back(child.get());
  }
  ShowBookmarkMenu(nodes, &folder, screen, false);
}

void BrowserWindow::ShowOverflowMenu(POINT screen) { ShowBookmarkMenu(overflowItems_, nullptr, screen, true); }

// 往菜单里填一层：子文件夹先挂空的子菜单，展开时（WM_INITMENUPOPUP）才填它的内容、解码它的图标。
// 命令号对应书签 id，不存节点指针：菜单开着时书签可能被别的页面改掉
void BrowserWindow::FillBookmarkMenu(HMENU menu, const std::vector<BookmarkNode*>& nodes, BookmarkNode* owner) {
  BookmarkMenuState& state = *bookmarkMenu_;
  if (nodes.empty()) {
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"（空）");
    return;
  }
  size_t added = 0;
  for (BookmarkNode* node : nodes) {
    if (added++ >= kMenuItemLimit || state.urls.size() >= kMenuOpenAllBase - kMenuItemBase) {
      break;
    }
    if (node->folder) {
      HMENU sub = CreatePopupMenu();
      state.folders[sub] = node->id;
      state.icons->Append(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(sub), MenuText(node->title), L"", true);
    } else {
      state.urls.push_back(node->id);
      state.icons->Append(menu, MF_STRING, kMenuItemBase + state.urls.size() - 1, MenuText(BookmarkLabel(*node)),
                          favicons_.Request(node->url), false);
    }
  }
  const int count = owner ? CountUrls(*owner) : 0;
  if (count >= 2) {
    state.openAll.push_back(owner->id);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    const std::wstring label = L"全部在新标签页中打开（" + std::to_wstring(count) + L"）";
    AppendMenuW(menu, MF_STRING, kMenuOpenAllBase + state.openAll.size() - 1, label.c_str());
  }
}

void BrowserWindow::ShowBookmarkMenu(const std::vector<BookmarkNode*>& nodes, BookmarkNode* openAllFolder, POINT screen,
                                     bool alignRight) {
  BookmarkMenuIcons icons(dpi_);  // includes submenu images; handles outlive the entire native modal menu
  BookmarkMenuState state;
  state.icons = &icons;
  BookmarkMenuState* previous = bookmarkMenu_;
  bookmarkMenu_ = &state;
  HMENU menu = CreatePopupMenu();
  FillBookmarkMenu(menu, nodes, openAllFolder);
  // 不加 TPM_NONOTIFY：子菜单要靠 WM_INITMENUPOPUP 展开时现填
  const UINT flags = TPM_RETURNCMD | TPM_TOPALIGN | (alignRight ? TPM_RIGHTALIGN : TPM_LEFTALIGN);
  const UINT command = static_cast<UINT>(TrackPopupMenuEx(menu, flags, screen.x, screen.y, hwnd_, nullptr));
  bookmarkMenu_ = previous;
  DestroyMenu(menu);
  if (command >= kMenuOpenAllBase && command - kMenuOpenAllBase < state.openAll.size()) {
    if (BookmarkNode* folder = bookmarks_.FindById(state.openAll[command - kMenuOpenAllBase]); folder && folder->folder) {
      std::vector<std::wstring> urls;
      for (const auto& child : folder->children) {
        if (!child->folder) urls.push_back(child->url);
      }
      for (const auto& url : urls) NewTab(url, false);
    }
  } else if (command >= kMenuItemBase && command - kMenuItemBase < state.urls.size()) {
    if (const BookmarkNode* node = bookmarks_.FindById(state.urls[command - kMenuItemBase]); node && !node->folder) {
      OpenUrl(node->url, GetKeyState(VK_CONTROL) < 0);
    }
  }
}

namespace {
void CopyTextToClipboard(HWND owner, const std::wstring& text) {
  if (!OpenClipboard(owner)) return;
  EmptyClipboard();
  const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
  if (HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
    if (void* target = GlobalLock(memory)) {
      memcpy(target, text.c_str(), bytes);
      GlobalUnlock(memory);
      if (!SetClipboardData(CF_UNICODETEXT, memory)) GlobalFree(memory);
    } else {
      GlobalFree(memory);
    }
  }
  CloseClipboard();
}
}  // namespace

void BrowserWindow::ShowBookmarkContextMenu(BookmarkNode& node, POINT screen) {
  // 菜单开着时这个书签可能被书签管理页删改：先记下 id，菜单返回后再按 id 取
  const int64_t id = node.id;
  const bool folder = node.folder;
  HMENU menu = CreatePopupMenu();
  if (folder) {
    const int urls = CountUrls(node);
    AppendMenuW(menu, MF_STRING | (urls ? 0 : MF_GRAYED), kMenuOpenAll,
                (L"全部在新标签页中打开（" + std::to_wstring(urls) + L"）").c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuEdit, L"重命名…");
  } else {
    AppendMenuW(menu, MF_STRING, kMenuOpen, L"打开");
    AppendMenuW(menu, MF_STRING, kMenuOpenNewTab, L"在新标签页中打开");
    AppendMenuW(menu, MF_STRING, kMenuCopyUrl, L"复制网址");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuEdit, L"编辑…");
  }
  AppendMenuW(menu, MF_STRING, kMenuDelete, L"删除");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, kMenuBookmarkManager, L"书签管理器");
  const UINT command = static_cast<UINT>(TrackPopupMenuEx(
      menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN | TPM_TOPALIGN, screen.x, screen.y, hwnd_, nullptr));
  DestroyMenu(menu);
  BookmarkNode* current = bookmarks_.FindById(id);
  if (!current && command != kMenuBookmarkManager) {
    return;  // 已经被删掉了
  }
  switch (command) {
    case kMenuOpen:
      OpenUrl(current->url, false);
      break;
    case kMenuOpenNewTab:
      NewTab(current->url, false);
      break;
    case kMenuCopyUrl:
      CopyTextToClipboard(hwnd_, current->url);
      break;
    case kMenuOpenAll: {
      std::vector<std::wstring> urls;
      for (const auto& child : current->children) {
        if (!child->folder) urls.push_back(child->url);
      }
      for (const auto& url : urls) NewTab(url, false);
      break;
    }
    case kMenuEdit:
      if (current->folder) OpenBookmarkManager(L"#edit=" + std::to_wstring(id));
      else EditBookmark(current, current->title, current->url);
      break;
    case kMenuDelete:
      if (bookmarks_.Remove(id)) {
        OnBookmarksChanged();
      }
      break;
    case kMenuBookmarkManager:
      OpenBookmarkManager(std::wstring());
      break;
    default:
      break;
  }
}

void BrowserWindow::ShowMainMenu(POINT screen) {
  HMENU menu = CreatePopupMenu();
  AppendMenuW(menu, MF_STRING, kMenuNewTab, L"新建标签页\tCtrl+T");
  AppendMenuW(menu, MF_STRING | (profile_->closedTabs.empty() ? MF_GRAYED : 0), kMenuReopenClosed,
              L"重新打开关闭的标签页\tCtrl+Shift+T");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, kMenuBookmarkManager, L"书签管理器\tCtrl+Shift+O");
  AppendMenuW(menu, MF_STRING | (settings_.bookmarkBar ? MF_CHECKED : 0), kMenuToggleBar, L"显示书签栏\tCtrl+Shift+B");
  AppendMenuW(menu, MF_STRING, kMenuImport, L"导入书签…");
  AppendMenuW(menu, MF_STRING, kMenuExtensions, L"扩展管理");
  AppendMenuW(menu, MF_STRING, kMenuHistory, L"历史记录\tCtrl+H");
  AppendMenuW(menu, MF_STRING, kMenuMedia, L"极速播放器\tCtrl+Shift+M");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, kMenuDefaultBrowser, L"设为默认浏览器…");
  AppendMenuW(menu, MF_STRING, kMenuAbout, L"关于极速浏览器");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, TrayIcon::kExit, L"彻底退出");
  const UINT command = static_cast<UINT>(TrackPopupMenuEx(
      menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTALIGN | TPM_TOPALIGN, screen.x, screen.y, hwnd_, nullptr));
  DestroyMenu(menu);
  switch (command) {
    case kMenuNewTab:
      NewTab(L"");
      break;
    case kMenuReopenClosed:
      ReopenClosedTab();
      break;
    case kMenuBookmarkManager:
      OpenBookmarkManager(std::wstring());
      break;
    case kMenuToggleBar:
      ExecuteCommand(kCmdToggleBookmarkBar);
      break;
    case kMenuImport:
      OpenBookmarkManager(L"#import");
      break;
    case kMenuExtensions:
      OpenExtensions();
      break;
    case kMenuHistory:
      OpenHistory();
      break;
    case kMenuMedia:
      OpenMedia();
      break;
    case kMenuDefaultBrowser:
      RegisterAsDefaultBrowser();
      break;
    case kMenuAbout:
      ShowAbout();
      break;
    case TrayIcon::kExit:
      ExecuteTrayCommand(TrayIcon::kExit);
      break;
    default:
      break;
  }
}

void BrowserWindow::OpenBookmarkManager(const std::wstring& fragment) {
  const std::wstring url = kBookmarksUrl + fragment;
  for (size_t i = 0; i < tabs_.size(); ++i) {
    Tab& tab = *tabs_[i];
    if (IsBookmarksPageUrl(tab.url) && !tab.agent) {
      Activate(i);
      if (!fragment.empty() && tab.webview) {
        NavigateTab(tab, url);
      }
      return;
    }
  }
  NewTab(url, true);
}

void BrowserWindow::OnBookmarksChanged() {
  if (!profileLoaded_) {
    return;
  }
  ScheduleBookmarkSave();  // 合并到 400ms 后在后台写盘：拖动、连续改名不再每次在界面线程上 fsync
  for (auto* window : liveWindows_) {
    if (window->profile_ != profile_) continue;
    window->Relayout();
    window->PlaceAllTabs();
    window->InvalidateChrome();
    window->BroadcastBookmarks();
  }
}

void BrowserWindow::BroadcastBookmarks() {
  std::string tree;
  for (auto& tab : tabs_) {
    if (tab->webview && IsBookmarksPageUrl(tab->url)) {
      if (tree.empty()) {
        tree = "{\"type\":\"bm.tree\",\"tree\":" + bookmarks_.TreeJson() + "}";
      }
      PostToPage(*tab, tree);
    }
  }
}

void BrowserWindow::LoadSettings() {
  if (profileDir_.empty()) {
    return;
  }
  std::string text;
  json::Value root;
  if (ReadFileBytes(profileDir_ + L"\\jisu-settings.json", text) && json::Parse(text, root)) {
    settings_.bookmarkFolder = static_cast<int64_t>(root["bookmarkFolder"].AsNumber(1));
    settings_.maxRegularTabs = static_cast<int>(std::clamp(root["maxRegularTabs"].AsNumber(10), 1.0, 99.0));
    settings_.bookmarkBar = root["bookmarkBar"].AsBool(true);
    settings_.importedDefaultBrowser = root["importedDefaultBrowser"].AsBool(false);
    for (const json::Value& origin : root["popupAllow"].Items()) {
      if (origin.IsString() && settings_.popupAllow.size() < 500) settings_.popupAllow.push_back(FromUtf8(origin.AsString()));
    }
    const json::Value& window = root["window"];
    if (window.IsObject()) {
      settings_.hasWindow = true;
      settings_.window = RECT{static_cast<LONG>(window["left"].AsNumber()), static_cast<LONG>(window["top"].AsNumber()),
                              static_cast<LONG>(window["right"].AsNumber()), static_cast<LONG>(window["bottom"].AsNumber())};
      settings_.windowMaximized = window["max"].AsBool();
    }
  }
}

namespace {
std::string WindowJson(const RECT& rect, bool maximized) {
  return "{\"left\":" + std::to_string(rect.left) + ",\"top\":" + std::to_string(rect.top) +
         ",\"right\":" + std::to_string(rect.right) + ",\"bottom\":" + std::to_string(rect.bottom) +
         ",\"max\":" + (maximized ? "true" : "false") + "}";
}
}  // namespace

void BrowserWindow::SaveSettings() {
  if (profileDir_.empty() || options_.appMode) {
    return;
  }
  std::string out = "{\"bookmarkBar\":";
  out += settings_.bookmarkBar ? "true" : "false";
  out += ",\"importedDefaultBrowser\":";
  out += settings_.importedDefaultBrowser ? "true" : "false";
  out += ",\"maxRegularTabs\":" + std::to_string(settings_.maxRegularTabs);
  out += ",\"bookmarkFolder\":" + std::to_string(settings_.bookmarkFolder);
  out += ",\"popupAllow\":[";
  for (size_t i = 0; i < settings_.popupAllow.size(); ++i) {
    if (i) out += ',';
    json::AppendQuoted(out, ToUtf8(settings_.popupAllow[i]));
  }
  out += ']';
  if (settings_.hasWindow) out += ",\"window\":" + WindowJson(settings_.window, settings_.windowMaximized);
  out += '}';
  profile_->writer.Write(profileDir_ + L"\\jisu-settings.json", std::move(out), true);
}

std::wstring BrowserWindow::AppPlacementFile() const {
  std::wstring id;
  for (const wchar_t c : options_.appId) {
    id.push_back((std::iswalnum(c) && c < 0x80) || c == L'-' || c == L'_' || c == L'.' ? c : L'_');
  }
  return profileDir_ + L"\\app-window-" + (id.empty() ? std::wstring(L"web-app") : id) + L".json";
}

// 关窗时记下位置大小（工作区坐标）与是否最大化：日常窗口存进设置，应用窗口按 app-id 单独存
void BrowserWindow::SaveWindowPlacement() {
  if (!ownsPrimary_ || !hwnd_ || options_.hasBounds || options_.secondary || !options_.metricsPath.empty() ||
      options_.preheatSeconds > 0 || fullscreen_.Active() || profileDir_.empty()) {
    return;
  }
  WINDOWPLACEMENT placement{sizeof(placement)};
  if (!GetWindowPlacement(hwnd_, &placement)) return;
  settings_.hasWindow = true;
  settings_.window = placement.rcNormalPosition;
  settings_.windowMaximized =
      placement.showCmd == SW_SHOWMAXIMIZED || (placement.showCmd == SW_SHOWMINIMIZED && (placement.flags & WPF_RESTORETOMAXIMIZED));
  if (options_.appMode) {
    profile_->writer.Write(AppPlacementFile(), "{\"window\":" + WindowJson(settings_.window, settings_.windowMaximized) + "}",
                           true);
  } else {
    SaveSettings();
  }
}

// 首帧前（窗口还没显示）套用上次的位置大小；那块显示器不在了就用默认位置
int BrowserWindow::ApplyWindowPlacement(int showCmd) {
  if (options_.hasBounds || !settings_.hasWindow || options_.secondary || !options_.metricsPath.empty() ||
      options_.preheatSeconds > 0) {
    return showCmd;
  }
  const RECT rect = settings_.window;
  if (rect.right - rect.left < MulDiv(480, static_cast<int>(dpi_), 96) / 2 ||
      rect.bottom - rect.top < MulDiv(320, static_cast<int>(dpi_), 96) / 2 || !MonitorFromRect(&rect, MONITOR_DEFAULTTONULL)) {
    return showCmd;
  }
  WINDOWPLACEMENT placement{sizeof(placement)};
  if (!GetWindowPlacement(hwnd_, &placement)) return showCmd;
  placement.rcNormalPosition = rect;
  placement.showCmd = SW_HIDE;
  placement.flags = 0;
  SetWindowPlacement(hwnd_, &placement);
  const bool plainShow = showCmd == SW_SHOWNORMAL || showCmd == SW_SHOWDEFAULT || showCmd == SW_SHOW || showCmd == SW_RESTORE;
  return settings_.windowMaximized && plainShow ? SW_SHOWMAXIMIZED : showCmd;
}

// 在当前用户下登记为浏览器（不需要管理员），再打开系统「默认应用」页由用户自己选
void BrowserWindow::RegisterAsDefaultBrowser() {
  const std::wstring exe = ExePath();
  const std::wstring quoted = L"\"" + exe + L"\"";
  const std::wstring icon = exe + L",0";
  const std::wstring classes = L"Software\\Classes\\JisuHTML";
  const std::wstring client = L"Software\\Clients\\StartMenuInternet\\JisuBrowser";
  bool ok = true;
  ok &= SetRegString(HKEY_CURRENT_USER, classes, nullptr, L"极速浏览器 HTML 文档");
  ok &= SetRegString(HKEY_CURRENT_USER, classes + L"\\DefaultIcon", nullptr, icon);
  // --single-argument：%1 之后的全部原文就是一个网址/文件，构造的链接夹带不了 --browser-args 等启动参数
  ok &= SetRegString(HKEY_CURRENT_USER, classes + L"\\shell\\open\\command", nullptr, quoted + L" --single-argument %1");
  ok &= SetRegString(HKEY_CURRENT_USER, client, nullptr, kAppName);
  ok &= SetRegString(HKEY_CURRENT_USER, client + L"\\DefaultIcon", nullptr, icon);
  ok &= SetRegString(HKEY_CURRENT_USER, client + L"\\shell\\open\\command", nullptr, quoted);
  const std::wstring caps = client + L"\\Capabilities";
  ok &= SetRegString(HKEY_CURRENT_USER, caps, L"ApplicationName", kAppName);
  ok &= SetRegString(HKEY_CURRENT_USER, caps, L"ApplicationIcon", icon);
  ok &= SetRegString(HKEY_CURRENT_USER, caps, L"ApplicationDescription", L"原生外壳 + WebView2 的快速浏览器");
  for (const wchar_t* scheme : {L"http", L"https"}) {
    ok &= SetRegString(HKEY_CURRENT_USER, caps + L"\\URLAssociations", scheme, L"JisuHTML");
  }
  for (const wchar_t* extension : {L".htm", L".html", L".shtml", L".xht", L".xhtml"}) {
    ok &= SetRegString(HKEY_CURRENT_USER, caps + L"\\FileAssociations", extension, L"JisuHTML");
  }
  ok &= SetRegString(HKEY_CURRENT_USER, caps + L"\\StartMenu", L"StartMenuInternet", L"JisuBrowser");
  ok &= SetRegString(HKEY_CURRENT_USER, L"Software\\RegisteredApplications", L"JisuBrowser", caps);
  SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
  if (!ok) {
    MessageBoxW(hwnd_, L"写入当前用户的浏览器登记信息失败。", kAppName, MB_OK | MB_ICONWARNING);
    return;
  }
  // Windows 不允许程序自己改默认浏览器：打开设置页，由用户点选
  ShellExecuteW(hwnd_, L"open", L"ms-settings:defaultapps?registeredAppUser=JisuBrowser", nullptr, nullptr,
                SW_SHOWNORMAL);
}

void BrowserWindow::ShowAbout() {
  std::wstring runtime = L"未就绪";
  CoTaskString version;
  if (environment_ && SUCCEEDED(environment_->get_BrowserVersionString(version.Put()))) {
    runtime = version.Str();
  }
  std::wstring text = std::wstring(kAppName) + L" " + FromUtf8(JISU_VERSION) + L"\n提交 " + FromUtf8(JISU_COMMIT) +
                      L"\nWebView2 运行时 " + runtime + L"\nWebView2 SDK " + FromUtf8(JISU_WEBVIEW2_SDK) +
                      L"\n\n智能体桥：" +
                      (!bridge_            ? std::wstring(L"未启用")
                       : bridge_->Yielding() ? std::wstring(L"让行中（Thorium 等配置里的浏览器在运行，关掉后自动接手）")
                                             : L"已启用（" + std::to_wstring(bridge_->ConnectionCount()) + L" 个连接）");
  MessageBoxW(hwnd_, text.c_str(), kAppName, MB_OK | MB_ICONINFORMATION);
}

LRESULT BrowserWindow::OnCopyData(const COPYDATASTRUCT& data) {
  if (options_.appMode && (data.dwData == kCopyDataOpenUrl || data.dwData == kCopyDataOpenUrl + 1)) {
    // Reopening never resets the current conversation or joins the daily browser.
    const bool passive = data.dwData == kCopyDataOpenUrl + 1;
    ShowWindow(hwnd_, passive ? SW_SHOWNOACTIVATE : (IsIconic(hwnd_) ? SW_RESTORE : SW_SHOW));
    if (!passive) SetForegroundWindow(hwnd_);
    // 只有网页控件不在时才重建（不等退避）；控件好好的就不打断当前页面
    RecoverAppWebView();
    return TRUE;
  }
  const bool passive = data.dwData == kCopyDataPassive;
  if ((data.dwData != kCopyDataOpenUrl && !passive) || data.cbData % sizeof(wchar_t) != 0 || data.cbData > 64 * 1024) {
    return FALSE;
  }
  std::wstring url(static_cast<const wchar_t*>(data.lpData), data.cbData / sizeof(wchar_t));
  while (!url.empty() && url.back() == L'\0') {
    url.pop_back();
  }
  url = Trim(url);
  if (passive) {
    // 不激活的转发：有网址就开在后台标签；没网址只是确认浏览器在运行，不多开空白页、不抢焦点、不闪任务栏
    if (!url.empty()) NewTab(ToNavigableUrl(url), false);
    return kCopyDataPassiveAck;
  }
  NewTab(url.empty() ? std::wstring() : ToNavigableUrl(url), true);
  // 用户自己点了链接或图标：把窗口带到前面（发送方已调 AllowSetForegroundWindow）
  if (IsIconic(hwnd_)) {
    ShowWindow(hwnd_, SW_RESTORE);
  }
  SetForegroundWindow(hwnd_);
  return TRUE;
}

// ---------------------------------------------------------------------------
// 启动时间线

void BrowserWindow::MaybeWriteMetrics() {
  if (options_.metricsPath.empty() || metricsWritten_) {
    return;
  }
  const Metrics& metrics = Metrics::Get();
  const bool spareDone = !options_.spare || metrics.Has("spareReady");
  if (metrics.Has("ntpFirstFrameHost") && metrics.Has("ntpFcp") && spareDone) {
    WriteMetrics(false);
  }
}

void BrowserWindow::WriteMetrics(bool forced) {
  if (metricsWritten_ || options_.metricsPath.empty()) {
    return;
  }
  metricsWritten_ = true;
  KillTimer(hwnd_, kTimerMetrics);
  KillTimer(hwnd_, kTimerWatchdog);
  Metrics& metrics = Metrics::Get();
  metrics.SetText("app", "jisu-browser");
  metrics.SetText("version", JISU_VERSION);
  metrics.SetText("commit", JISU_COMMIT);
  metrics.SetText("sdkVersion", JISU_WEBVIEW2_SDK);
  metrics.SetText("exe", ToUtf8(ExePath()));
  metrics.SetText("commandLine", ToUtf8(options_.commandLine));
  metrics.SetText("renderTarget", options_.renderTarget == RenderTargetKind::kSoftware ? "sw" : "hw");
  metrics.SetText("envStart", options_.envStart == EnvStart::kLate ? "late" : "early");
  metrics.SetText("ntp", options_.ntp == NtpSource::kInline ? "inline" : "folder");
  metrics.SetText("theme", palette_.dark ? "dark" : "light");
  metrics.SetNumber("warmD3D", options_.warmD3D ? 1 : 0);
  metrics.SetNumber("spare", options_.spare ? 1 : 0);
  metrics.SetNumber("rtHardware", painter_.IsHardware() ? 1 : 0);
  metrics.SetNumber("dpi", static_cast<double>(dpi_));
  metrics.SetNumber("refreshHz", MonitorRefreshHz(hwnd_));
  metrics.SetNumber("forced", forced ? 1 : 0);
  metrics.SetNumber("zeroEpochMs", ProcessClock::ZeroEpochMs());
  metrics.SetNumber("bookmarkBarItems", static_cast<double>(barItems_.size()));
  if (!tabs_.empty() && tabs_.front()->webview) {
    UINT32 pid = 0;
    if (SUCCEEDED(tabs_.front()->webview->get_BrowserProcessId(&pid))) {
      metrics.SetNumber("browserPid", pid);
    }
  }
  metrics.Write(options_.metricsPath);
  if (options_.exitAfterMetrics) {
    PostMessageW(hwnd_, WM_CLOSE, 0, 0);
  }
}

// ---------------------------------------------------------------------------
// 窗口

LRESULT CALLBACK BrowserWindow::WndProcThunk(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  auto* self = reinterpret_cast<BrowserWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (msg == WM_NCCREATE) {
    const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
    self = static_cast<BrowserWindow*>(create->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    self->hwnd_ = hwnd;
  }
  return self ? self->WndProc(msg, wParam, lParam) : DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT BrowserWindow::WndProc(UINT msg, WPARAM wParam, LPARAM lParam) {
  if (options_.appMode && !fullscreen_.Active() && (msg == WM_NCCALCSIZE || msg == WM_NCHITTEST)) return DefWindowProcW(hwnd_, msg, wParam, lParam);
  if (tray_.HandleExplorerRestart(msg)) {
    trayInstallAttempts_ = 0;
    InstallTrayIcon();
    return 0;
  }
  if (bridge_ && bridge_->HandleMessage(msg, wParam, lParam)) {
    return 0;
  }
  switch (msg) {
    case WM_NCCALCSIZE:
      if (wParam) {
        auto* p = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);
        if (!fullscreen_.Active() && IsZoomed(hwnd_)) {
          MONITORINFO monitor{sizeof(MONITORINFO)};
          if (GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &monitor)) {
            p->rgrc[0] = monitor.rcWork;
            // 任务栏设成自动隐藏时工作区就是整个屏幕：在任务栏那条边留 1 像素，鼠标碰到屏幕边缘它才能弹出来。
            // 首帧之后才查（shell32 不进启动关键路径），最大化启动的窗口在首帧后补算一次
            if (firstFrameShown_ && EqualRect(&monitor.rcWork, &monitor.rcMonitor)) {
              APPBARDATA state{sizeof(state)};
              if (SHAppBarMessage(ABM_GETSTATE, &state) & ABS_AUTOHIDE) {
                for (const UINT edge : {ABE_BOTTOM, ABE_LEFT, ABE_TOP, ABE_RIGHT}) {
                  APPBARDATA bar{sizeof(bar)};
                  bar.uEdge = edge;
                  bar.rc = monitor.rcMonitor;
                  if (!SHAppBarMessage(ABM_GETAUTOHIDEBAREX, &bar)) continue;
                  if (edge == ABE_BOTTOM) p->rgrc[0].bottom -= 1;
                  else if (edge == ABE_TOP) p->rgrc[0].top += 1;
                  else if (edge == ABE_LEFT) p->rgrc[0].left += 1;
                  else p->rgrc[0].right -= 1;
                  break;
                }
              }
            }
          }
        }
        return 0;
      }
      break;
    case WM_NCHITTEST: {
      if (fullscreen_.Active()) return HTCLIENT;
      POINT p = PointFromLParam(lParam);
      RECT r{}; GetWindowRect(hwnd_, &r);
      if (!IsZoomed(hwnd_)) {
        const int edge = GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi_) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi_);
        const bool left = p.x < r.left + edge, right = p.x >= r.right - edge;
        const bool top = p.y < r.top + edge, bottom = p.y >= r.bottom - edge;
        if (top && left) return HTTOPLEFT; if (top && right) return HTTOPRIGHT;
        if (bottom && left) return HTBOTTOMLEFT; if (bottom && right) return HTBOTTOMRIGHT;
        if (left) return HTLEFT; if (right) return HTRIGHT;
        if (top) return HTTOP; if (bottom) return HTBOTTOM;
      }
      ScreenToClient(hwnd_, &p);
      if (p.y >= 0 && p.y < layout_.tabStripHeight * layout_.scale && HitTest(p).kind == ChromeHit::kNone) return HTCAPTION;
      return HTCLIENT;
    }
    case WM_SYSCOMMAND:
      // 单按 Alt / F10：没有菜单栏，进了看不见的系统菜单模式后接下来的按键都被吞掉；Alt+空格的系统菜单照常
      if ((wParam & 0xFFF0) == SC_KEYMENU && lParam == 0 && !options_.appMode) return 0;
      break;
    case kMsgInstallTray:
      InstallTrayIcon();
      return 0;
    case TrayIcon::kCallback:
      ExecuteTrayCommand(tray_.HandleCallback(wParam, lParam));
      return 0;
    case kMsgTabLimit:
      EnforceTabLimit();
      return 0;
    case kMsgFullscreenChanged:
      OnContentFullscreenChanged(static_cast<uint32_t>(wParam));
      return 0;
    case kMsgFavicon:
      OnFaviconUpdated(static_cast<uint64_t>(wParam));
      return 0;
    case kMsgTask: {
      std::unique_ptr<std::function<void()>> task(reinterpret_cast<std::function<void()>*>(lParam));
      if (task && *task && !destroyed_) (*task)();
      return 0;
    }
    case WM_CREATE:
      OnCreate();
      return 0;
    case WM_ERASEBKGND:
      return 1;  // 整个客户区由 Direct2D 画，不让 GDI 先擦一遍
    case WM_PAINT:
      if (options_.appMode || fullscreen_.Active()) {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(hwnd_, &paint);
        FillRect(dc, &paint.rcPaint, static_cast<HBRUSH>(GetStockObject(options_.appMode ? WHITE_BRUSH : BLACK_BRUSH)));
        if (options_.appMode && !message_.empty()) {  // 没有标签条：内核出错时把原因画在窗口中央，不留纯白
          RECT client{};
          GetClientRect(hwnd_, &client);
          HFONT font = CreateFontW(-MulDiv(15, static_cast<int>(dpi_), 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                   DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                   DEFAULT_PITCH, L"Microsoft YaHei UI");
          HGDIOBJ previous = SelectObject(dc, font ? font : GetStockObject(DEFAULT_GUI_FONT));
          SetBkMode(dc, TRANSPARENT);
          SetTextColor(dc, RGB(96, 96, 96));
          DrawTextW(dc, message_.c_str(), -1, &client, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
          SelectObject(dc, previous);
          if (font) DeleteObject(font);
        }
        EndPaint(hwnd_, &paint);
      } else {
        painter_.Paint(palette_, layout_, BuildChromeState());
        ValidateRect(hwnd_, nullptr);
      }
      return 0;
    case WM_SIZE:
      CloseExtensionPopup();
      if (wParam == SIZE_MINIMIZED) {
        PlaceAllTabs();  // 最小化时让内核降载；醒着的智能体标签移去屏幕外宿主
        return 0;
      }
      painter_.Resize(LOWORD(lParam), HIWORD(lParam));
      Relayout();
      if (options_.preheatSeconds <= 0) {
        PlaceAllTabs();
      }
      if (spare_ && spare_->controller) {
        spare_->controller->put_Bounds(ContentRect());
      }
      if (agentHost_) {
        // 智能体标签的 CSS 视口跟主窗口网页区一样大
        const SIZE size = AgentHostSize();
        SetWindowPos(agentHost_, nullptr, 0, 0, size.cx, size.cy, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        for (auto& tab : tabs_) {
          if (tab->agent && tab->controller && tab->parent == agentHost_) {
            tab->controller->put_Bounds(AgentHostRect());
          }
        }
      }
      return 0;
    case WM_DPICHANGED: {
      CloseExtensionPopup();
      for (auto& e : extensions_) if(e.toolbarIcon){DestroyIcon(e.toolbarIcon);e.toolbarIcon=nullptr;}
      dpi_ = HIWORD(wParam);
      if (extensionsButton_) RefreshExtensionToolbar();
      painter_.SetDpi(dpi_);
      UpdateEditFont();
      if (chromeTooltip_) SendMessageW(chromeTooltip_, TTM_SETMAXTIPWIDTH, 0, MulDiv(420, static_cast<int>(dpi_), 96));
      const auto* suggested = reinterpret_cast<const RECT*>(lParam);
      // 首帧前套用上次位置时移到了另一块屏：保存的就是那块屏上的像素尺寸，不再按 DPI 比例缩放一遍
      if (fullscreen_.Active()) fullscreen_.FitToMonitor(hwnd_);
      else if (firstFrameShown_) SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                                               suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
      Relayout();
      InvalidateRect(hwnd_, nullptr, FALSE);
      return 0;
    }
    case WM_DISPLAYCHANGE:
      fullscreen_.FitToMonitor(hwnd_);
      if (agentHost_) {
        // 加了、挪了显示器：屏幕外宿主重新放到所有显示器之外，不落进可见区域
        const SIZE size = AgentHostSize();
        SetWindowPos(agentHost_, nullptr, GetSystemMetrics(SM_XVIRTUALSCREEN) - size.cx - 4000, GetSystemMetrics(SM_YVIRTUALSCREEN),
                     0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
      }
      return 0;
    case WM_GETMINMAXINFO: {
      auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
      const int dpi = static_cast<int>(dpi_);
      info->ptMinTrackSize.x = MulDiv(480, dpi, 96);
      info->ptMinTrackSize.y = MulDiv(320, dpi, 96);
      return 0;
    }
    case WM_MOVE:
    case WM_MOVING:
      if (Tab* tab = ActiveTab(); tab && tab->controller) {
        tab->controller->NotifyParentWindowPositionChanged();
      }
      break;
    case WM_SETFOCUS:
      if (focusInWebView_) {
        if (Tab* tab = ActiveTab(); tab && tab->controller) {
          tab->controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
          return 0;
        }
      }
      if (edit_) {
        SetFocus(edit_);
      }
      return 0;
    case WM_CAPTURECHANGED:
    case WM_CANCELMODE:
      CancelTabDrag();
      CancelBookmarkDrag();
      break;
    case WM_KEYDOWN:
      if (wParam == VK_ESCAPE && dragTabId_) { CancelTabDrag(); return 0; }
      if (wParam == VK_ESCAPE && dragBookmarkId_) { CancelBookmarkDrag(); return 0; }
      break;
    case WM_MOUSEMOVE:
      OnMouseMove(PointFromLParam(lParam));
      return 0;
    case WM_MOUSELEAVE:
      trackingMouse_ = false;
      if (frozenTabWidth_ > 0) {
        frozenTabWidth_ = 0;  // 鼠标离开：连续关标签结束，剩下的标签展开
        Relayout();
        InvalidateChrome();
      }
      if (hover_.kind != ChromeHit::kNone) {
        hover_ = {};
        InvalidateChrome();
      }
      UpdateChromeTooltip();
      return 0;
    case WM_LBUTTONDOWN:
      OnLButtonDown(PointFromLParam(lParam));
      return 0;
    case WM_LBUTTONUP:
      OnLButtonUp(PointFromLParam(lParam));
      return 0;
    case WM_MBUTTONUP:
      OnMButtonUp(PointFromLParam(lParam));
      return 0;
    case WM_RBUTTONUP:
      OnRButtonUp(PointFromLParam(lParam));
      return 0;
    case WM_INITMENUPOPUP:
      // 书签子菜单展开时才填内容、解码图标
      if (bookmarkMenu_) {
        const HMENU menu = reinterpret_cast<HMENU>(wParam);
        const auto folder = bookmarkMenu_->folders.find(menu);
        if (folder != bookmarkMenu_->folders.end() && bookmarkMenu_->filled.insert(menu).second) {
          std::vector<BookmarkNode*> children;
          BookmarkNode* node = bookmarks_.FindById(folder->second);
          if (node && node->folder) {
            for (auto& child : node->children) children.push_back(child.get());
          }
          FillBookmarkMenu(menu, children, node && node->folder ? node : nullptr);
        }
        return 0;
      }
      break;
    case WM_CTLCOLOREDIT:
      if (reinterpret_cast<HWND>(lParam) == edit_) {
        const HDC dc = reinterpret_cast<HDC>(wParam);
        SetTextColor(dc, ToColorRef(palette_.text));
        SetBkColor(dc, ToColorRef(palette_.omnibox));
        return reinterpret_cast<LRESULT>(omniboxBrush_);
      }
      break;
    case WM_DRAWITEM:
      if (wParam == kExtensionsButton || (wParam >= kExtensionButtonBase && wParam - kExtensionButtonBase < extensions_.size())) {
        DrawExtensionButton(*reinterpret_cast<const DRAWITEMSTRUCT*>(lParam)); return TRUE;
      }
      break;
    case kMsgCloseExtensionPopup:
      if(static_cast<uint64_t>(wParam) == extensionPopupGeneration_ && MayProcessExtensionPopupClose(hwnd_)) CloseExtensionPopup();
      return 0;
    case kMsgOpenExtensionSettings: {
      std::unique_ptr<std::wstring> url(reinterpret_cast<std::wstring*>(lParam));
      CloseExtensionPopup(); if(url) NewTab(*url); return 0;
    }
    case WM_ACTIVATE:
      if (LOWORD(wParam) == WA_INACTIVE && ShouldCloseExtensionPopupOnDeactivate(hwnd_, extensionPopupWindow_, reinterpret_cast<HWND>(lParam))) CloseExtensionPopup();
      break;
    case WM_COMMAND:
      if (LOWORD(wParam) >= TrayIcon::kOpen && LOWORD(wParam) <= TrayIcon::kExit) {
        ExecuteTrayCommand(LOWORD(wParam));
        return 0;
      }
      if (HIWORD(wParam) == BN_CLICKED && HandleExtensionCommand(LOWORD(wParam))) return 0;
      if (LOWORD(wParam) == kAddressBarId) {
        switch (HIWORD(wParam)) {
          case EN_SETFOCUS:
            CloseExtensionPopup();
            omniboxFocused_ = true;
            focusInWebView_ = false;
            PostMessageW(edit_, EM_SETSEL, 0, -1);  // 点进地址栏即全选，和主流浏览器一致
            InvalidateChrome();
            break;
          case EN_KILLFOCUS:
            omniboxFocused_ = false;
            InvalidateChrome();
            break;
          case EN_CHANGE:
            if (!settingAddress_) {
              editing_ = true;
            }
            break;
          default:
            break;
        }
        return 0;
      }
      break;
    case WM_COPYDATA:
      if (const auto* data = reinterpret_cast<const COPYDATASTRUCT*>(lParam)) {
        return OnCopyData(*data);
      }
      return FALSE;
    case WM_TIMER:
      if (wParam == 7) { PollMedia(); return 0; }
      if (wParam == kTimerAppRecover) {
        RecoverAppWebView();
        return 0;
      }
      if (wParam == kTimerTrayRetry) {
        KillTimer(hwnd_, kTimerTrayRetry);
        InstallTrayIcon();
        return 0;
      }
      if (wParam == kTimerSpare) {
        KillTimer(hwnd_, kTimerSpare);
        CreateSpare();
        return 0;
      }
      if (wParam == kTimerMetrics || wParam == kTimerWatchdog) {
        WriteMetrics(true);
        return 0;
      }
      if (wParam == kTimerPreheat) {
        DestroyWindow(hwnd_);
        return 0;
      }
      if (wParam == kTimerAgentFreeze) {
        SuspendIdleAgentTabs();
        return 0;
      }
      if (wParam == kTimerSession) {
        KillTimer(hwnd_, kTimerSession);
        sessionTimerPending_ = false;
        SaveSession(false, false);
        return 0;
      }
      if (wParam == kTimerBookmarks) {
        FlushBookmarkSave();
        return 0;
      }
      if (wParam == kTimerRecover) {
        KillTimer(hwnd_, kTimerRecover);
        RecoverEnvironment();
        return 0;
      }
      if (wParam == kTimerExtensions) {
        KillTimer(hwnd_, kTimerExtensions);
        StartExtensions();
        return 0;
      }
      break;
    case WM_SETTINGCHANGE:
      if (lParam && wcscmp(reinterpret_cast<const wchar_t*>(lParam), L"ImmersiveColorSet") == 0) {
        OnThemeChanged();
      }
      break;
    case WM_QUERYENDSESSION:
      return TRUE;
    case WM_ENDSESSION:
      // 关机、注销、系统更新重启：进程随时会被结束，书签、历史、会话现在就写完，下次开机照样恢复标签
      if (wParam) FlushForEndSession();
      return 0;
    case kMsgCommand:
      ExecuteCommand(wParam, lParam);
      return 0;
    case WM_DESTROY:
      OnDestroy();
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd_, msg, wParam, lParam);
}

void BrowserWindow::OnCreate() {
  dpi_ = GetDpiForWindow(hwnd_);
  ApplyWindowTheme();
  omniboxBrush_ = CreateSolidBrush(ToColorRef(palette_.omnibox));
  // 地址栏用系统 EDIT：自带中文输入法、选择、剪贴板与无障碍
  edit_ = options_.appMode ? nullptr : CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | ES_LEFT | ES_AUTOHSCROLL, 0, 0,
                          0, 0, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAddressBarId)), instance_,
                          nullptr);
  if (edit_) {
    SendMessageW(edit_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"搜索或输入网址"));
    SendMessageW(edit_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
    // 右键菜单（粘贴并转到）、多行粘贴
    SetWindowSubclass(edit_, &BrowserWindow::AddressBarProc, 1, reinterpret_cast<DWORD_PTR>(this));
  }
  UpdateEditFont();
  Metrics::Get().Mark("addressBarCreated");

  if (root_ != this) { Relayout(); return; }  // A detached window adopts the existing controller.
  // 第一个标签先有，控件等环境就绪再建
  auto first = std::make_unique<Tab>();
  first->id = nextTabId_++;
  first->pendingUrl = options_.startUrl.empty() ? std::wstring() : ToNavigableUrl(options_.startUrl);
  first->url = first->pendingUrl;
  firstTabId_ = first->id;
  tabs_.push_back(std::move(first));
  active_ = 0;
  Relayout();
  UpdateWindowTitle();
}

void BrowserWindow::InstallTrayIcon() {
  if (destroyed_ || options_.appMode || options_.preheatSeconds > 0 || !options_.metricsPath.empty() || options_.secondary) return;
  ++trayInstallAttempts_;
  const std::wstring version = FromUtf8(JISU_VERSION) + L" · " + FromUtf8(std::string(JISU_COMMIT).substr(0, 7));
  if (!tray_.Install(hwnd_, instance_, version) && trayInstallAttempts_ < 6) {
    SetTimer(hwnd_, kTimerTrayRetry, 2000, nullptr);
  }
  SetPropW(hwnd_, L"JisuBrowser.TrayRegistered", reinterpret_cast<HANDLE>(static_cast<uintptr_t>(tray_.Registered())));
  WriteTrayState();
}

void BrowserWindow::WriteTrayState() {
  if (!ownsPrimary_ || options_.secondary) return;
  if (options_.appMode || !options_.defaultUserData || !options_.metricsPath.empty() || options_.preheatSeconds > 0) return;
  const std::wstring file = ParentDirectory(profileDir_) + L"\\tray-status.json";
  std::string state = "{\"pid\":" + std::to_string(GetCurrentProcessId()) +
      ",\"hwnd\":" + std::to_string(reinterpret_cast<uintptr_t>(hwnd_)) +
      ",\"registered\":" + (tray_.Registered() ? "true" : "false") +
      ",\"exited\":" + (destroyed_ ? "true" : "false") + ",\"version\":";
  json::AppendQuoted(state, JISU_VERSION);
  state += ",\"commit\":";
  json::AppendQuoted(state, JISU_COMMIT);
  state += "}";
  WriteFileAtomic(file, state, false);  // 状态文件：不等落盘
}

void BrowserWindow::ExecuteTrayCommand(UINT command) {
  if (command == TrayIcon::kExit) {
    // Same normal teardown as the window X: bridge, popups, all tabs and spare
    // WebViews are closed; no invisible shell is left behind.
    for (auto* window : liveWindows_) {
      window->tray_.SetExiting();
      PostMessageW(window->hwnd_, WM_CLOSE, 0, 0);
    }
  } else if (command == TrayIcon::kOpen || command == TrayIcon::kNewTab) {
    if (command == TrayIcon::kNewTab) NewTab(L"");
    ShowWindow(hwnd_, IsIconic(hwnd_) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(hwnd_);
  }
}

void BrowserWindow::OnDestroy() {
  CancelTabDrag(false);
  // 窗口还在、标签还在：先记下位置大小与会话。最后一个窗口正常关闭 = 干净退出（关机中除外）
  SaveWindowPlacement();
  const bool lastWindow = std::count_if(liveWindows_.begin(), liveWindows_.end(), [this](const BrowserWindow* window) {
                            return window->profile_ == profile_;
                          }) <= 1;
  if (lastWindow) {
    SaveSession(!profile_->endingSession, true);
  }
  FlushBookmarkSave();
  for (const UINT_PTR timer : {kTimerTrayRetry, kTimerSession, kTimerBookmarks, kTimerRecover, kTimerExtensions}) {
    KillTimer(hwnd_, timer);
  }
  tray_.SetExiting();
  WriteMetrics(true);  // 测量中被关掉也留下已有的时间线（已写过则不重复）
  destroyed_ = true;
  alive_.reset();
  if (lastWindow) favicons_.Stop();  // 不等进行中的网络请求，退出不被抓图标拖住
  CloseExtensionPopup();
  for (auto& e : extensions_) if(e.toolbarIcon){DestroyIcon(e.toolbarIcon);e.toolbarIcon=nullptr;}
  if(extensionTooltip_){DestroyWindow(extensionTooltip_);extensionTooltip_=nullptr;}
  if (chromeTooltip_) {
    DestroyWindow(chromeTooltip_);
    chromeTooltip_ = nullptr;
  }
  const bool hadBridge = bridge_ != nullptr;
  bridge_.reset();  // 先断开所有智能体连接，再关控件
  for (auto& tab : tabs_) {
    *tab->owner = nullptr;
    StopMedia(*tab);
    if (tab->leaseDone) {
      LeaseCallback done = std::move(tab->leaseDone);
      tab->leaseDone = nullptr;
      done(0, nullptr, "Browser is closing");
    }
    AbandonNewWindow(*tab);
    if (tab->controller) {
      tab->controller->Close();
    }
  }
  if (spare_ && spare_->controller) {
    *spare_->owner = nullptr;
    spare_->controller->Close();
  }
  tabs_.clear();
  spare_.reset();
  environment_.Reset();
  if (agentHost_) {
    DestroyWindow(agentHost_);
    agentHost_ = nullptr;
  }
  // 还没执行的界面任务：随窗口一起丢掉，不泄漏
  MSG pending{};
  while (PeekMessageW(&pending, hwnd_, kMsgTask, kMsgTask, PM_REMOVE)) {
    delete reinterpret_cast<std::function<void()>*>(pending.lParam);
  }
  tray_.Remove();
  RemovePropW(hwnd_, L"JisuBrowser.TrayRegistered");
  WriteTrayState();
  RemovePropW(hwnd_, kProfileProp);
  liveWindows_.erase(std::remove(liveWindows_.begin(), liveWindows_.end(), this), liveWindows_.end());
  if (liveWindows_.empty()) {
    profile_->writer.Flush(3000);  // 书签、会话、设置写完再退
    PostQuitMessage(0);
  } else {
    if (ownsPrimary_) {
      auto* primary = liveWindows_.front();
      primary->ownsPrimary_ = true;
      PostMessageW(primary->hwnd_, kMsgInstallTray, 0, 0);
      if (hadBridge) primary->StartAgentBridge();
      favicons_.SetWindow(primary->hwnd_);  // 图标更新通知交给还开着的窗口
      // 内核正在重建（重试计时器随本窗口一起停了）：交给接手的窗口接着重试
      if (!primary->environment_ && !primary->options_.appMode) primary->ScheduleEnvironmentRecovery();
    }
    liveWindows_.front()->ScheduleSessionSave();  // 剩下的窗口接着记会话
  }
}

void BrowserWindow::ApplyWindowTheme() {
  const BOOL dark = palette_.dark ? TRUE : FALSE;
  DwmSetWindowAttribute(hwnd_, kDwmUseImmersiveDarkMode, &dark, static_cast<DWORD>(sizeof(dark)));
  // Win11：标题栏与标签条同色；Win10 不认这两个属性，调用失败无副作用
  const COLORREF caption = ToColorRef(palette_.frame);
  DwmSetWindowAttribute(hwnd_, kDwmCaptionColor, &caption, static_cast<DWORD>(sizeof(caption)));
  const COLORREF text = ToColorRef(palette_.text);
  DwmSetWindowAttribute(hwnd_, kDwmTextColor, &text, static_cast<DWORD>(sizeof(text)));
}

void BrowserWindow::OnThemeChanged() {
  palette_ = LoadPalette();
  ApplyWindowTheme();
  if (omniboxBrush_) {
    DeleteObject(omniboxBrush_);
  }
  omniboxBrush_ = CreateSolidBrush(ToColorRef(palette_.omnibox));
  auto recolor = [this](Tab& tab) {
    ComPtr<ICoreWebView2Controller2> controller2;
    if (tab.controller && SUCCEEDED(tab.controller.As(&controller2))) {
      controller2->put_DefaultBackgroundColor(ToWebViewColor(palette_.content));
    }
  };
  for (auto& tab : tabs_) {
    recolor(*tab);
  }
  if (spare_) {
    recolor(*spare_);
  }
  InvalidateRect(hwnd_, nullptr, FALSE);
  InvalidateRect(edit_, nullptr, TRUE);
}

void BrowserWindow::UpdateEditFont() {
  const HFONT font =
      CreateFontW(-MulDiv(14, static_cast<int>(dpi_), 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                  L"Microsoft YaHei UI");
  SendMessageW(edit_, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
  if (editFont_) {
    DeleteObject(editFont_);
  }
  editFont_ = font;
  const HDC dc = GetDC(edit_);
  const HGDIOBJ previous = SelectObject(dc, font);
  TEXTMETRICW metrics{};
  GetTextMetricsW(dc, &metrics);
  SelectObject(dc, previous);
  ReleaseDC(edit_, dc);
  editHeightPx_ = metrics.tmHeight;
}

// HTML fullscreen changes the WebView surface, not the native host window.
// Only the selected, visible tab may expand the host; background leases never do.
void BrowserWindow::OnContentFullscreenChanged(uint32_t id) {
  Tab* tab = FindTab(id);
  if (!tab || !tab->webview) return;
  BOOL contains = FALSE;
  if (FAILED(tab->webview->get_ContainsFullScreenElement(&contains))) return;
  if (contains) {
    if (!IsActive(*tab) || !IsWindowVisible(hwnd_) || IsIconic(hwnd_)) return;
    if (GetForegroundWindow() != hwnd_ && !fullscreen_.Active()) return;
    fullscreenTab_ = id;
  } else if (fullscreenTab_ == id) {
    fullscreenTab_ = 0;
  } else return;
  UpdateFullscreen();
}

void BrowserWindow::UpdateFullscreen() {
  if (browserFullscreen_ || fullscreenTab_) {
    CloseExtensionPopup();
    CancelTabDrag();
    CancelBookmarkDrag();
    hover_ = {}; pressed_ = {};
    if (!fullscreen_.Enter(hwnd_)) {
      browserFullscreen_ = false;
      ExitContentFullscreen();
      return;
    }
  } else fullscreen_.Exit(hwnd_);
  Relayout();
  PlaceAllTabs();
  InvalidateChrome();
}

void BrowserWindow::ExitContentFullscreen() {
  if (!fullscreenTab_) return;
  Tab* tab = FindTab(fullscreenTab_);
  fullscreenTab_ = 0;
  if (tab && tab->webview)
    tab->webview->ExecuteScript(L"if(document.fullscreenElement)document.exitFullscreen().catch(()=>{});", nullptr);
  UpdateFullscreen();
}

void BrowserWindow::ExitFullscreen() {
  browserFullscreen_ = false;
  ExitContentFullscreen();
  UpdateFullscreen();
}

void BrowserWindow::Relayout() {
  RECT client{};
  GetClientRect(hwnd_, &client);
  ChromeLayout& l = layout_;
  l.scale = static_cast<float>(dpi_) / 96.0f;
  l.width = static_cast<float>(client.right - client.left) / l.scale;
  if (options_.appMode || fullscreen_.Active()) {
    l.contentTopPx = 0;
    if (edit_) ShowWindow(edit_, SW_HIDE);
    if (extensionsButton_) ShowWindow(extensionsButton_, SW_HIDE);
    for (auto& extension : extensions_) if (extension.button) ShowWindow(extension.button, SW_HIDE);
    return;
  }
  if (edit_) ShowWindow(edit_, SW_SHOWNOACTIVATE);

  l.minimize = D2D1::RectF(l.width - 138, 0, l.width - 92, l.tabStripHeight);
  l.maximize = D2D1::RectF(l.width - 92, 0, l.width - 46, l.tabStripHeight);
  l.windowClose = D2D1::RectF(l.width - 46, 0, l.width, l.tabStripHeight);
  auto strip = LayoutTabStrip(l.width, tabs_.size(), frozenTabWidth_ > 0 ? frozenTabWidth_ : kTabPreferredWidth);
  l.tabs = std::move(strip.tabs);
  l.tabCloses = std::move(strip.closes);
  l.newTab = strip.newTab;
  // 出声或已静音的标签：关闭钮左侧放扬声器（标签太窄就不放）
  l.tabAudio.assign(tabs_.size(), D2D1_RECT_F{});
  for (size_t i = 0; i < tabs_.size(); ++i) {
    const Tab& tab = *tabs_[i];
    const float width = l.tabs[i].right - l.tabs[i].left;
    if ((!tab.audible && !tab.muted) || width < 110.0f) continue;
    const D2D1_RECT_F& close = l.tabCloses[i];
    const float right = close.right > close.left ? close.left - 2.0f : l.tabs[i].right - 12.0f;
    l.tabAudio[i] = D2D1::RectF(right - 22.0f, kTabCenterY - 10.0f, right, kTabCenterY + 10.0f);
  }

  const float top = l.tabStripHeight;
  l.back = D2D1::RectF(8.0f, top + 4.0f, 40.0f, top + 36.0f);
  l.forward = D2D1::RectF(44.0f, top + 4.0f, 76.0f, top + 36.0f);
  l.reload = D2D1::RectF(80.0f, top + 4.0f, 112.0f, top + 36.0f);
  l.menu = D2D1::RectF(std::max(160.0f, l.width - 40.0f), top + 4.0f, std::max(192.0f, l.width - 8.0f), top + 36.0f);
  const float toolbarRight = LayoutExtensionToolbar(l.menu.left - 6.0f, top, l.scale);
  l.omnibox = D2D1::RectF(120.0f, top + 5.0f, std::max(160.0f, toolbarRight - 6.0f), top + 35.0f);
  l.star = D2D1::RectF(l.omnibox.right - 32.0f, l.omnibox.top + 2.0f, l.omnibox.right - 4.0f, l.omnibox.bottom - 2.0f);
  // 当前页拦下过弹出窗口：星标左侧放提示钮
  const Tab* active = ActiveTab();
  l.popupBlocked = active && !active->blockedPopups.empty()
                       ? D2D1::RectF(l.star.left - 30.0f, l.star.top, l.star.left - 2.0f, l.star.bottom)
                       : D2D1_RECT_F{};
  LayoutBookmarkBar();
  l.separatorPx = static_cast<int>(std::lround((l.tabStripHeight + l.toolbarHeight + l.bookmarkBarHeight) * l.scale));
  l.contentTopPx = l.separatorPx + 1;

  if (edit_) {
    const float editRight = l.popupBlocked.right > l.popupBlocked.left ? l.popupBlocked.left : l.star.left;
    const int left = static_cast<int>(std::lround((l.omnibox.left + 12.0f) * l.scale));
    const int right = static_cast<int>(std::lround((editRight - 2.0f) * l.scale));
    const int centerY = static_cast<int>(std::lround((l.omnibox.top + l.omnibox.bottom) * 0.5f * l.scale));
    const RECT rect{left, centerY - editHeightPx_ / 2, std::max(left, right), centerY - editHeightPx_ / 2 + editHeightPx_};
    if (!EqualRect(&rect, &editRect_)) {  // 位置没变就不移动、不重画地址栏
      editRect_ = rect;
      MoveWindow(edit_, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, TRUE);
    }
  }
}

void BrowserWindow::LayoutBookmarkBar() {
  ChromeLayout& l = layout_;
  barItems_.clear();
  overflowItems_.clear();
  l.bookmarks.clear();
  l.hasOverflow = false;
  // 开着书签栏就显示，书签栏还没有书签时画一句提示（不再整条隐藏，像开关没反应）
  const bool show = profileLoaded_ && settings_.bookmarkBar;
  l.bookmarkBarHeight = show ? 30.0f : 0.0f;
  if (!show) {
    return;
  }
  constexpr float kOverflowWidth = 28.0f;
  const float top = l.tabStripHeight + l.toolbarHeight - 2.0f;
  const float itemTop = top + 2.0f;
  const float itemBottom = top + 28.0f;
  float x = 8.0f;
  auto& children = bookmarks_.Bar().children;
  for (size_t i = 0; i < children.size(); ++i) {
    BookmarkNode& node = *children[i];
    const float width = painter_.MeasureBookmark(node.title, node.folder);
    const bool last = i + 1 == children.size();
    const float limit = last ? l.width - 8.0f : l.width - 8.0f - kOverflowWidth - 4.0f;
    if (x + width > limit) {
      for (size_t j = i; j < children.size(); ++j) {
        overflowItems_.push_back(children[j].get());
      }
      break;
    }
    l.bookmarks.push_back(D2D1::RectF(x, itemTop, x + width, itemBottom));
    barItems_.push_back(&node);
    x += width + 2.0f;
  }
  if (!overflowItems_.empty()) {
    l.hasOverflow = true;
    l.bookmarkOverflow = D2D1::RectF(l.width - 8.0f - kOverflowWidth, itemTop, l.width - 8.0f, itemBottom);
  }
}

RECT BrowserWindow::ContentRect() const {
  RECT client{};
  GetClientRect(hwnd_, &client);
  return fullscreen_.ContentBounds(client, layout_.contentTopPx);
}

ChromeState BrowserWindow::BuildChromeState() const {
  ChromeState state;
  state.tabs.reserve(tabs_.size());
  for (size_t i = 0; i < tabs_.size(); ++i) {
    const Tab& tab = *tabs_[i];
    std::wstring title = tab.title.empty() ? std::wstring(tab.agent ? kAgentTabTitle : kNewTabTitle) : tab.title;
    ChromeTab item{std::move(title), i == active_, tab.loading, tab.agent, tab.agentAwake,
                   tab.agent ? L"" : favicons_.Request(tab.url.empty() ? tab.pendingUrl : tab.url)};
    item.audible = tab.audible;
    item.muted = tab.muted;
    item.pinned = tab.pinned;
    state.tabs.push_back(std::move(item));
  }
  state.hover = hover_;
  state.bookmarkDrag = draggingBookmark_;
  state.bookmarkDrop = dropBookmark_;
  state.bookmarkDropIntoFolder = dropIntoFolder_;
  const Tab* active = ActiveTab();
  if (active) {
    state.canBack = active->canBack;
    state.canForward = active->canForward;
    state.loading = active->loading;
    state.canStar = profileLoaded_ && IsStarrable(active->url);
    if (state.canStar) {
      // 按网址和书签版本缓存：悬停重绘不再每帧整树查找
      if (active->url != starredUrl_ || bookmarks_.Revision() != starredRevision_) {
        starredUrl_ = active->url;
        starredRevision_ = bookmarks_.Revision();
        starredValue_ = const_cast<BookmarkStore&>(bookmarks_).FindByUrl(active->url) != nullptr;
      }
      state.starred = starredValue_;
    }
  }
  state.bookmarks.reserve(barItems_.size());
  for (const BookmarkNode* node : barItems_) {
    state.bookmarks.push_back(ChromeBookmark{node->title, node->folder, node->folder ? L"" : favicons_.Request(node->url)});
  }
  state.bookmarkBarEmpty = layout_.bookmarkBarHeight > 0 && barItems_.empty() && overflowItems_.empty();
  state.maximized = IsZoomed(hwnd_) != FALSE;
  state.omniboxFocused = omniboxFocused_;
  // 提示只给当前标签：哪个标签的控件建不起来就在哪个标签显示，切走就不显示
  state.message = active && !active->error.empty() ? active->error : message_;
  // 网页控件正盖着内容区时只画顶部界面
  state.paintContent = !active || !active->controller || !active->visible || !state.message.empty();
  return state;
}

ChromeHit BrowserWindow::HitTest(POINT point) const {
  if (fullscreen_.Active()) return {};
  const float x = static_cast<float>(point.x) / layout_.scale;
  const float y = static_cast<float>(point.y) / layout_.scale;
  auto inside = [x, y](const D2D1_RECT_F& r) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; };
  if (inside(layout_.minimize)) return ChromeHit{ChromeHit::kMinimize, -1};
  if (inside(layout_.maximize)) return ChromeHit{ChromeHit::kMaximize, -1};
  if (inside(layout_.windowClose)) return ChromeHit{ChromeHit::kWindowClose, -1};
  for (size_t i = 0; i < layout_.tabs.size(); ++i) {
    if (inside(layout_.tabs[i])) {
      if (i < layout_.tabAudio.size() && inside(layout_.tabAudio[i])) return ChromeHit{ChromeHit::kTabAudio, static_cast<int>(i)};
      return ChromeHit{inside(layout_.tabCloses[i]) ? ChromeHit::kTabClose : ChromeHit::kTab, static_cast<int>(i)};
    }
  }
  if (inside(layout_.newTab)) {
    return ChromeHit{ChromeHit::kNewTab, -1};
  }
  if (inside(layout_.back)) {
    return ChromeHit{ChromeHit::kBack, -1};
  }
  if (inside(layout_.forward)) {
    return ChromeHit{ChromeHit::kForward, -1};
  }
  if (inside(layout_.reload)) {
    return ChromeHit{ChromeHit::kReload, -1};
  }
  if (inside(layout_.menu)) {
    return ChromeHit{ChromeHit::kMenu, -1};
  }
  if (layout_.popupBlocked.right > layout_.popupBlocked.left && inside(layout_.popupBlocked)) {
    return ChromeHit{ChromeHit::kPopupBlocked, -1};
  }
  if (inside(layout_.star)) {
    const Tab* tab = ActiveTab();
    if (tab && profileLoaded_ && IsStarrable(tab->url)) {
      return ChromeHit{ChromeHit::kStar, -1};
    }
  }
  for (size_t i = 0; i < layout_.bookmarks.size(); ++i) {
    if (inside(layout_.bookmarks[i])) {
      return ChromeHit{ChromeHit::kBookmark, static_cast<int>(i)};
    }
  }
  if (layout_.hasOverflow && inside(layout_.bookmarkOverflow)) {
    return ChromeHit{ChromeHit::kBookmarkOverflow, -1};
  }
  if (layout_.bookmarkBarHeight > 0) {
    const float barTop = layout_.tabStripHeight + layout_.toolbarHeight - 2.0f;
    if (y >= barTop && y < barTop + layout_.bookmarkBarHeight && x >= 0 && x < layout_.width) {
      return ChromeHit{ChromeHit::kBookmarkBar, -1};  // 书签栏空白处：右键菜单
    }
  }
  return {};
}

void BrowserWindow::InvalidateChrome() {
  if (hwnd_) {
    InvalidateRect(hwnd_, nullptr, FALSE);
  }
}

void BrowserWindow::OnMouseMove(POINT point) {
  if (dragTabId_ && GetCapture() == hwnd_) { UpdateTabDrag(point); return; }
  if (dragBookmarkId_ && GetCapture() == hwnd_) {
    if (!draggingBookmark_ && (std::abs(point.x - dragStart_.x) >= GetSystemMetricsForDpi(SM_CXDRAG, dpi_) ||
                              std::abs(point.y - dragStart_.y) >= GetSystemMetricsForDpi(SM_CYDRAG, dpi_))) draggingBookmark_ = true;
    if (draggingBookmark_) { UpdateBookmarkDrop(point); return; }
  }
  if (!trackingMouse_) {
    TRACKMOUSEEVENT track{sizeof(TRACKMOUSEEVENT), TME_LEAVE, hwnd_, 0};
    TrackMouseEvent(&track);
    trackingMouse_ = true;
  }
  if (frozenTabWidth_ > 0 && static_cast<float>(point.y) / layout_.scale >= layout_.tabStripHeight + 8.0f) {
    frozenTabWidth_ = 0;  // 鼠标离开标签条：剩下的标签展开
    Relayout();
    InvalidateChrome();
  }
  const ChromeHit hit = HitTest(point);
  if (hit != hover_) {
    hover_ = hit;
    InvalidateChrome();
  }
  UpdateChromeTooltip();
}

void BrowserWindow::OnLButtonDown(POINT point) {
  CloseExtensionPopup();
  pressed_ = HitTest(point);
  if (pressed_.kind == ChromeHit::kBookmark && static_cast<size_t>(pressed_.index) < barItems_.size()) {
    dragBookmarkId_ = barItems_[static_cast<size_t>(pressed_.index)]->id;
    dragStart_ = point;
    SetCapture(hwnd_);
  }
  if (pressed_.kind == ChromeHit::kTab) {
    const size_t index = static_cast<size_t>(pressed_.index);
    Activate(index);  // 标签按下即切换
    dragTabId_ = tabs_[index]->id;
    dragTabOriginalIndex_ = index;
    dragTabStart_ = point;
    dragTabGrabX_ = point.x / layout_.scale - layout_.tabs[index].left;
    SetCapture(hwnd_);
  }
}

void BrowserWindow::OnLButtonUp(POINT point) {
  if (dragTabId_) { FinishTabDrag(point); return; }
  if (draggingBookmark_) { FinishBookmarkDrag(point); return; }
  if (dragBookmarkId_) { dragBookmarkId_ = 0; if (GetCapture() == hwnd_) ReleaseCapture(); }
  const ChromeHit hit = HitTest(point);
  const ChromeHit pressed = pressed_;
  pressed_ = {};
  if (hit != pressed) {
    return;
  }
  // 菜单从按钮下沿弹出
  auto below = [this](const D2D1_RECT_F& r, bool right) {
    POINT p{static_cast<LONG>(std::lround((right ? r.right : r.left) * layout_.scale)),
            static_cast<LONG>(std::lround(r.bottom * layout_.scale))};
    ClientToScreen(hwnd_, &p);
    return p;
  };
  switch (hit.kind) {
    case ChromeHit::kMinimize:
      ShowWindow(hwnd_, SW_MINIMIZE); break;
    case ChromeHit::kMaximize:
      ShowWindow(hwnd_, IsZoomed(hwnd_) ? SW_RESTORE : SW_MAXIMIZE); break;
    case ChromeHit::kWindowClose:
      PostMessageW(hwnd_, WM_CLOSE, 0, 0); break;
    case ChromeHit::kTabClose:
      if (static_cast<size_t>(hit.index) < layout_.tabs.size()) {
        // 连续点关闭：剩下的标签先保持原宽，下一个关闭钮正好移到光标下
        frozenTabWidth_ = layout_.tabs[static_cast<size_t>(hit.index)].right - layout_.tabs[static_cast<size_t>(hit.index)].left;
      }
      CloseTab(static_cast<size_t>(hit.index));
      break;
    case ChromeHit::kTabAudio:
      ToggleMuted(static_cast<size_t>(hit.index));
      break;
    case ChromeHit::kNewTab:
      NewTab(L"");
      break;
    case ChromeHit::kBack:
      ExecuteCommand(kCmdBack);
      break;
    case ChromeHit::kForward:
      ExecuteCommand(kCmdForward);
      break;
    case ChromeHit::kReload:
      ExecuteCommand(kCmdStopOrReload);
      break;
    case ChromeHit::kStar:
      ToggleBookmark(false);
      break;
    case ChromeHit::kPopupBlocked:
      ShowPopupBlockedMenu(below(layout_.popupBlocked, false));
      break;
    case ChromeHit::kMenu:
      ShowMainMenu(below(layout_.menu, true));
      break;
    case ChromeHit::kBookmark:
      if (static_cast<size_t>(hit.index) < barItems_.size()) {
        BookmarkNode* node = barItems_[static_cast<size_t>(hit.index)];
        if (node->folder) {
          ShowBookmarkFolderMenu(*node, below(layout_.bookmarks[static_cast<size_t>(hit.index)], false));
        } else {
          OpenUrl(node->url, GetKeyState(VK_CONTROL) < 0);
        }
      }
      break;
    case ChromeHit::kBookmarkOverflow:
      ShowOverflowMenu(below(layout_.bookmarkOverflow, true));
      break;
    default:
      break;
  }
}

void BrowserWindow::OnMButtonUp(POINT point) {
  const ChromeHit hit = HitTest(point);
  if (hit.kind == ChromeHit::kTab || hit.kind == ChromeHit::kTabClose || hit.kind == ChromeHit::kTabAudio) {
    if (static_cast<size_t>(hit.index) < layout_.tabs.size()) {
      frozenTabWidth_ = layout_.tabs[static_cast<size_t>(hit.index)].right - layout_.tabs[static_cast<size_t>(hit.index)].left;
    }
    CloseTab(static_cast<size_t>(hit.index));
  } else if (hit.kind == ChromeHit::kBookmark && static_cast<size_t>(hit.index) < barItems_.size()) {
    BookmarkNode* node = barItems_[static_cast<size_t>(hit.index)];
    if (!node->folder) {
      NewTab(node->url, false);  // 中键：后台新标签
    }
  }
}

void BrowserWindow::OnRButtonUp(POINT point) {
  CloseExtensionPopup();
  const ChromeHit hit = HitTest(point);
  POINT screen = point;
  ClientToScreen(hwnd_, &screen);
  if (hit.kind == ChromeHit::kBookmark && static_cast<size_t>(hit.index) < barItems_.size()) {
    ShowBookmarkContextMenu(*barItems_[static_cast<size_t>(hit.index)], screen);
  } else if ((hit.kind == ChromeHit::kTab || hit.kind == ChromeHit::kTabClose || hit.kind == ChromeHit::kTabAudio) &&
             static_cast<size_t>(hit.index) < tabs_.size()) {
    ShowTabContextMenu(static_cast<size_t>(hit.index), screen);
  } else if (hit.kind == ChromeHit::kBookmarkBar || hit.kind == ChromeHit::kBookmarkOverflow) {
    ShowBookmarkBarMenu(screen);
  }
}

// ---------------------------------------------------------------------------
// 合并写盘、延后执行

// 在界面线程上、WebView2 回调之外执行（提示框、关标签这类不能在内核回调里做的事）
void BrowserWindow::PostTask(std::function<void()> task) {
  if (destroyed_ || !hwnd_) return;
  auto* payload = new std::function<void()>(std::move(task));
  if (!PostMessageW(hwnd_, kMsgTask, 0, reinterpret_cast<LPARAM>(payload))) delete payload;
}

// 标签有变化：1.5 秒后写一次会话（期间的变化合并），崩溃、被结束后下次启动照此恢复
void BrowserWindow::ScheduleSessionSave() {
  if (!profile_->sessionWritable || destroyed_ || !hwnd_ || sessionTimerPending_) return;
  sessionTimerPending_ = SetTimer(hwnd_, kTimerSession, 1500, nullptr) != 0;
}

void BrowserWindow::ScheduleBookmarkSave() {
  profile_->bookmarksDirty = true;
  if (!destroyed_ && hwnd_) SetTimer(hwnd_, kTimerBookmarks, 400, nullptr);
}

void BrowserWindow::FlushBookmarkSave() {
  if (hwnd_) KillTimer(hwnd_, kTimerBookmarks);
  if (!profile_->bookmarksDirty || bookmarks_.File().empty()) return;
  profile_->bookmarksDirty = false;
  profile_->writer.Write(bookmarks_.File(), bookmarks_.Serialized(), true);
  bookmarks_.MarkWritten();
}

// 日常窗口内核建不起来或中途退出：第一次 0.5 秒后重建，再失败按 3 秒起、封顶 30 秒退避
void BrowserWindow::ScheduleEnvironmentRecovery() {
  if (options_.appMode || destroyed_ || !hwnd_) return;
  recoverDelay_ = recoverDelay_ ? std::min<UINT>(std::max<UINT>(recoverDelay_ * 2, 3000), 30000) : 500;
  SetTimer(hwnd_, kTimerRecover, recoverDelay_, nullptr);
}

void BrowserWindow::StartExtensions() {
  if (extensionsStarted_ || !extensionProfile_ || destroyed_) return;
  extensionsStarted_ = true;
  if (hwnd_) KillTimer(hwnd_, kTimerExtensions);
  InstallNextExtension();
}

}  // namespace jisu
