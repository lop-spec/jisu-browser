#include "browser_window.h"

#include "json.h"
#include "metrics.h"
#include "menu_icons.h"
#include "bookmark_editor.h"
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
#include <cwchar>
#include <cwctype>

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
constexpr wchar_t kAppName[] = L"极速浏览器";
constexpr wchar_t kNewTabTitle[] = L"新标签页";
constexpr wchar_t kAgentTabTitle[] = L"智能体";
// 新标签页与书签管理页经虚拟主机映射到随包的 ntp\ 目录；.example 是保留域名，不会和真实网站冲突
constexpr wchar_t kNtpHost[] = L"newtab.jisu.example";
constexpr wchar_t kNtpOrigin[] = L"https://newtab.jisu.example/";
constexpr wchar_t kNtpUrl[] = L"https://newtab.jisu.example/index.html";
constexpr wchar_t kBookmarksUrl[] = L"https://newtab.jisu.example/bookmarks.html";
constexpr wchar_t kBookmarksDisplay[] = L"jisu://bookmarks";
constexpr wchar_t kSearchPrefix[] = L"https://www.bing.com/search?q=";
constexpr wchar_t kProfileProp[] = L"JisuBrowser.Profile";

constexpr UINT kMsgCommand = WM_APP + 1;
constexpr UINT kMsgFavicon = WM_APP + 2;
constexpr UINT kMsgTabLimit = WM_APP + 3;
constexpr UINT_PTR kTimerSpare = 1;
constexpr UINT_PTR kTimerMetrics = 2;
constexpr UINT_PTR kTimerWatchdog = 3;
constexpr UINT_PTR kTimerPreheat = 4;
constexpr UINT_PTR kTimerAgentFreeze = 5;

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
constexpr UINT kMenuOpen = 2001;
constexpr UINT kMenuOpenNewTab = 2002;
constexpr UINT kMenuEdit = 2003;
constexpr UINT kMenuDelete = 2004;
constexpr UINT kMenuOpenAll = 2005;
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

bool IsNewTabPageUrl(const std::wstring& url) {
  return url.empty() || url == L"about:blank" || url == kNtpOrigin || url.starts_with(kNtpUrl);
}

bool IsBookmarksPageUrl(const std::wstring& url) { return url.starts_with(kBookmarksUrl); }

bool IsWebUrl(const std::wstring& url) { return url.starts_with(L"http://") || url.starts_with(L"https://"); }

// 能收藏的页面：网页，不含外壳自己的新标签页与书签管理页
bool IsStarrable(const std::wstring& url) { return IsWebUrl(url) && !url.starts_with(kNtpOrigin); }

std::wstring DisplayUrl(const std::wstring& url) {
  if (IsBookmarksPageUrl(url)) {
    return kBookmarksDisplay;
  }
  return IsNewTabPageUrl(url) ? std::wstring() : url;
}

std::wstring Trim(const std::wstring& text) {
  const size_t first = text.find_first_not_of(L" \t\r\n");
  if (first == std::wstring::npos) {
    return {};
  }
  const size_t last = text.find_last_not_of(L" \t\r\n");
  return text.substr(first, last - first + 1);
}

std::wstring UrlEncode(const std::wstring& text) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::wstring out;
  for (const char ch : ToUtf8(text)) {
    const unsigned char c = static_cast<unsigned char>(ch);
    const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
                            c == '_' || c == '.' || c == '~';
    if (unreserved) {
      out.push_back(static_cast<wchar_t>(c));
    } else if (c == ' ') {
      out.push_back(L'+');
    } else {
      out.push_back(L'%');
      out.push_back(static_cast<wchar_t>(kHex[c >> 4]));
      out.push_back(static_cast<wchar_t>(kHex[c & 0x0F]));
    }
  }
  return out;
}

// 地址栏输入 → 网址：带协议的原样打开；像域名的补 https://；localhost 与 IPv4 补 http://；其余交给搜索
std::wstring ToNavigableUrl(const std::wstring& input) {
  std::wstring lower = input;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
  if (lower == kBookmarksDisplay || lower.starts_with(std::wstring(kBookmarksDisplay) + L"#")) {
    return kBookmarksUrl + input.substr(wcslen(kBookmarksDisplay));
  }
  if (lower == L"jisu://newtab") {
    return kNtpUrl;
  }
  static constexpr const wchar_t* kSchemes[] = {L"http://", L"https://", L"file://",     L"about:",
                                                L"data:",   L"edge://",  L"view-source:"};
  for (const wchar_t* scheme : kSchemes) {
    if (lower.starts_with(scheme)) {
      return input;
    }
  }
  if (input.find_first_of(L" \t") == std::wstring::npos) {
    const std::wstring host = lower.substr(0, lower.find_first_of(L":/"));
    const bool ipv4 = !host.empty() && host.find_first_not_of(L"0123456789.") == std::wstring::npos &&
                      host.find(L'.') != std::wstring::npos;
    if (host == L"localhost" || ipv4) {
      return L"http://" + input;
    }
    if (host.find(L'.') != std::wstring::npos && host.front() != L'.' && host.back() != L'.') {
      return L"https://" + input;
    }
  }
  return kSearchPrefix + UrlEncode(input);
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

// 菜单项文字：& 要写成 &&，太长的截断
std::wstring MenuText(const std::wstring& text) {
  std::wstring shown = text.size() > 60 ? text.substr(0, 59) + L"…" : text;
  std::wstring out;
  for (const wchar_t c : shown) {
    if (c == L'&') {
      out += L"&&";
    } else if (c == L'\t' || c == L'\r' || c == L'\n') {
      out.push_back(L' ');
    } else {
      out.push_back(c);
    }
  }
  return out.empty() ? std::wstring(L"（无标题）") : out;
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

BrowserWindow::BrowserWindow(const Options& options, GraphicsWarmup& warmup)
    : options_(options), warmup_(warmup), palette_(LoadPalette()) {}

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
}

// ---------------------------------------------------------------------------
// 启动

DWORD WINAPI BrowserWindow::ProfileThreadMain(void* param) {
  auto* self = static_cast<BrowserWindow*>(param);
  Metrics::Get().Mark("profileBegin");
  self->LoadSettings();
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
  // 书签与设置在后台读，和建窗、预热并行；首帧前汇合
  profileThread_ = CreateThread(nullptr, 0, &BrowserWindow::ProfileThreadMain, this, 0, nullptr);
  if (!profileThread_) {
    ProfileThreadMain(this);
  }

  WNDCLASSEXW windowClass{};
  windowClass.cbSize = static_cast<UINT>(sizeof(windowClass));
  windowClass.style = CS_HREDRAW | CS_VREDRAW;
  windowClass.lpfnWndProc = &BrowserWindow::WndProcThunk;
  windowClass.hInstance = instance;
  windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  windowClass.hbrBackground = CreateSolidBrush(ToColorRef(palette_.content));
  windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
  windowClass.lpszClassName = kWindowClass;
  if (!RegisterClassExW(&windowClass)) {
    return false;
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
  const HWND created = CreateWindowExW(0, kWindowClass, kAppName, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, x, y, width,
                                       height, nullptr, nullptr, instance, this);
  if (!created) {
    return false;
  }
  SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
  if (options_.metricsPath.empty() && options_.preheatSeconds <= 0) favicons_.Start(profileDir_, hwnd_, kMsgFavicon);
  SetPropW(hwnd_, kProfileProp, reinterpret_cast<HANDLE>(static_cast<uintptr_t>(ProfileHash(profileDir_))));
  if (options_.preheatSeconds > 0) {
    SetTimer(hwnd_, kTimerPreheat, static_cast<UINT>(options_.preheatSeconds) * 1000u, nullptr);
  }
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

  if (!options_.noActivate) {
    SetFocus(edit_);
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
  const HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
      nullptr, userDataFolder, environmentOptions.Get(),
      Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
          [this](HRESULT result, ICoreWebView2Environment* environment) -> HRESULT {
            OnEnvironmentCreated(result, environment);
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
    // 延后写出：默认 early 模式下这里可能早于首帧，首帧相关时间点还没记
    if (!options_.metricsPath.empty()) {
      SetTimer(hwnd_, kTimerMetrics, 500, nullptr);
    }
    return;
  }
  metrics.Mark("envReady");
  environment_ = environment;
  CoTaskString version;
  if (SUCCEEDED(environment_->get_BrowserVersionString(version.Put()))) {
    metrics.SetText("runtimeVersion", ToUtf8(version.Str()));
  }
  for (auto& tab : tabs_) {
    if (!tab->controller && !tab->creating) {
      CreateController(*tab);
    }
  }
  StartAgentBridge();
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
  auto handler = Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
      [this, id](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
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
  ConfigureWebView(*tab);
  if (id == firstTabId_) {
    metrics.Mark("controllerReady");
  }
  if (spare) {
    metrics.Mark("spareReady");
  }

  if (tab->newWindowArgs) {
    if (FAILED(tab->newWindowArgs->put_NewWindow(tab->webview.Get()))) {
      AbandonNewWindow(*tab);
    } else {
      tab->newWindowDeferral->Complete();
      tab->newWindowArgs.Reset();
      tab->newWindowDeferral.Reset();
    }
  } else if (!tab->pendingUrl.empty()) {
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
  swprintf_s(text, L"无法创建网页控件（0x%08X）。", static_cast<unsigned>(result));
  message_ = text;
  InvalidateChrome();
  if (first && !options_.metricsPath.empty()) {
    SetTimer(hwnd_, kTimerMetrics, 500, nullptr);
  }
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
  // 新标签页与书签管理页：虚拟主机映射到随包的 ntp\ 目录（内置新标签页模式下书签管理页也要用）
  ComPtr<ICoreWebView2_3> webview3;
  if (SUCCEEDED(tab.webview.As(&webview3))) {
    const std::wstring folder = ExeDirectory() + L"\\ntp";
    webview3->SetVirtualHostNameToFolderMapping(kNtpHost, folder.c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY);
  }
  ComPtr<ICoreWebView2_3> faviconMapping;
  if (SUCCEEDED(tab.webview.As(&faviconMapping))) {
    const std::wstring cache = profileDir_ + L"\\Favicons";
    faviconMapping->SetVirtualHostNameToFolderMapping(L"favicons.jisu.example", cache.c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
  }
  ConfigureProfile(tab);
  const uint32_t id = tab.id;
  EventRegistrationToken token{};
  ComPtr<ICoreWebView2_15> faviconView;
  if (SUCCEEDED(tab.webview.As(&faviconView))) {
    std::weak_ptr<int> alive = alive_;
    faviconView->add_FaviconChanged(Callback<ICoreWebView2FaviconChangedEventHandler>(
      [this, id, alive](ICoreWebView2* sender, IUnknown*) -> HRESULT {
        if (alive.expired()) return S_OK;
        const Tab* current = FindTab(id);
        if (!current || current->agent) return S_OK;
        const std::wstring url = current->url;
        ComPtr<ICoreWebView2_15> view;
        if (SUCCEEDED(sender->QueryInterface(IID_PPV_ARGS(view.GetAddressOf())))) {
          view->GetFavicon(COREWEBVIEW2_FAVICON_IMAGE_FORMAT_PNG, Callback<ICoreWebView2GetFaviconCompletedHandler>(
            [this, url, alive](HRESULT hr, IStream* image) -> HRESULT {
              if (!alive.expired() && SUCCEEDED(hr)) favicons_.Store(url, image);
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
    if (agentConfig_.blockAutoplay) {
      tab.webview->AddScriptToExecuteOnDocumentCreated(
          kAgentMediaScript,
          Callback<ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler>(
              [](HRESULT, LPCWSTR) -> HRESULT { return S_OK; })
              .Get());
    }
    if (agentConfig_.blockImages) {
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
              [this, id](ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT {
                const Tab* t = FindTab(id);
                if (!t || !t->agent || IsActive(*t) || !environment_) {
                  return S_OK;
                }
                ComPtr<ICoreWebView2WebResourceResponse> response;
                if (SUCCEEDED(environment_->CreateWebResourceResponse(nullptr, 404, L"Blocked", L"",
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
          [this, id](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
            OnWebMessage(id, args);
            return S_OK;
          })
          .Get(),
      &token);
  tab.webview->add_DocumentTitleChanged(
      Callback<ICoreWebView2DocumentTitleChangedEventHandler>([this, id](ICoreWebView2* sender, IUnknown*) -> HRESULT {
        if (Tab* t = FindTab(id)) {
          CoTaskString title;
          if (SUCCEEDED(sender->get_DocumentTitle(title.Put()))) {
            t->title = title.Str();
          }
          if (t->title.empty() && !IsNewTabPageUrl(t->url)) {
            t->title = t->url;
          }
          if (IsActive(*t)) {
            UpdateWindowTitle();
          }
          InvalidateChrome();
        }
        return S_OK;
      }).Get(),
      &token);
  tab.webview->add_SourceChanged(
      Callback<ICoreWebView2SourceChangedEventHandler>(
          [this, id](ICoreWebView2* sender, ICoreWebView2SourceChangedEventArgs*) -> HRESULT {
            if (Tab* t = FindTab(id)) {
              CoTaskString source;
              if (SUCCEEDED(sender->get_Source(source.Put()))) {
                t->url = source.Str();
              }
              if (IsActive(*t)) {
                UpdateAddressBar();
                InvalidateChrome();  // 星标跟着网址变
              }
            }
            return S_OK;
          })
          .Get(),
      &token);
  tab.webview->add_HistoryChanged(
      Callback<ICoreWebView2HistoryChangedEventHandler>([this, id](ICoreWebView2* sender, IUnknown*) -> HRESULT {
        if (Tab* t = FindTab(id)) {
          BOOL back = FALSE;
          BOOL forward = FALSE;
          sender->get_CanGoBack(&back);
          sender->get_CanGoForward(&forward);
          t->canBack = back != FALSE;
          t->canForward = forward != FALSE;
          if (IsActive(*t)) {
            InvalidateChrome();
          }
        }
        return S_OK;
      }).Get(),
      &token);
  tab.webview->add_NavigationStarting(
      Callback<ICoreWebView2NavigationStartingEventHandler>(
          [this, id](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs*) -> HRESULT {
            if (Tab* t = FindTab(id)) {
              t->loading = true;
              InvalidateChrome();
            }
            return S_OK;
          })
          .Get(),
      &token);
  tab.webview->add_NavigationCompleted(
      Callback<ICoreWebView2NavigationCompletedEventHandler>(
          [this, id](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*) -> HRESULT {
            if (Tab* t = FindTab(id)) {
              t->loading = false;
              Metrics& metrics = Metrics::Get();
              if (id == firstTabId_ && !metrics.Has("ntpNavCompleted")) {
                metrics.Mark("ntpNavCompleted");
              }
              InvalidateChrome();
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
          [this, id](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
            // 智能体页面弹出的窗口作为普通后台标签打开，不抢用户的视线
            const Tab* source = FindTab(id);
            OpenNewWindowAsTab(args, !(source && source->agent));
            return S_OK;
          })
          .Get(),
      &token);
  tab.webview->add_ScriptDialogOpening(
      Callback<ICoreWebView2ScriptDialogOpeningEventHandler>(
          [this, id](ICoreWebView2*, ICoreWebView2ScriptDialogOpeningEventArgs* args) -> HRESULT {
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
          [this, id](ICoreWebView2*, ICoreWebView2PermissionRequestedEventArgs* args) -> HRESULT {
            const Tab* t = FindTab(id);
            if (t && t->agent && !IsActive(*t)) {
              args->put_State(COREWEBVIEW2_PERMISSION_STATE_DENY);  // 屏幕外的权限气泡没人能点
            }
            return S_OK;
          })
          .Get(),
      &token);
  tab.webview->add_ProcessFailed(
      Callback<ICoreWebView2ProcessFailedEventHandler>(
          [this, id](ICoreWebView2* sender, ICoreWebView2ProcessFailedEventArgs* args) -> HRESULT {
            COREWEBVIEW2_PROCESS_FAILED_KIND kind = COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED;
            args->get_ProcessFailedKind(&kind);
            Tab* t = FindTab(id);
            if (!t || !t->agent || kind != COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_EXITED) {
              return S_OK;
            }
            // 智能体页面崩溃：断开当前连接、标签回池，并重新加载
            if (bridge_) {
              bridge_->OnAgentTabGone(id);
            }
            t->leased = false;
            t->agentAwake = false;
            sender->Reload();
            PlaceTab(*t);
            InvalidateChrome();
            return S_OK;
          })
          .Get(),
      &token);
  tab.controller->add_AcceleratorKeyPressed(
      Callback<ICoreWebView2AcceleratorKeyPressedEventHandler>(
          [this](ICoreWebView2Controller*, ICoreWebView2AcceleratorKeyPressedEventArgs* args) -> HRESULT {
            COREWEBVIEW2_KEY_EVENT_KIND kind = COREWEBVIEW2_KEY_EVENT_KIND_KEY_UP;
            args->get_KeyEventKind(&kind);
            if (kind != COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN && kind != COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_DOWN) {
              return S_OK;
            }
            UINT key = 0;
            args->get_VirtualKey(&key);
            COREWEBVIEW2_PHYSICAL_KEY_STATUS status{};
            args->get_PhysicalKeyStatus(&status);
            if (DispatchShortcut(key, status.WasKeyDown != FALSE)) {
              args->put_Handled(TRUE);
            }
            return S_OK;
          })
          .Get(),
      &token);
  tab.controller->add_GotFocus(Callback<ICoreWebView2FocusChangedEventHandler>(
                                   [this](ICoreWebView2Controller*, IUnknown*) -> HRESULT {
                                     if(extensionPopupWindow_) PostMessageW(hwnd_,kMsgCloseExtensionPopup,static_cast<WPARAM>(extensionPopupGeneration_),0);
                                     focusInWebView_ = true;
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
  if (Tab* tab = FindTab(id); tab && HandleTabLimitMessage(*tab, origin, ToUtf8(text.Str()))) return;
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
          e.handle->Enable(enabled ? TRUE : FALSE, Callback<ICoreWebView2BrowserExtensionEnableCompletedHandler>(
            [this, i, enabled, alive](HRESULT hr) -> HRESULT {
              if (alive.expired()) return S_OK;
              if (SUCCEEDED(hr)) {
                extensions_[i].enabled = enabled;
                std::string text; json::Value c;
                if (ReadFileBytes(profileDir_ + L"\\extensions.json", text) && json::Parse(text,c)) {
                  auto entries = c["extensions"];
                  for (auto& item : entries.MutableItems()) {
                    if (FromUtf8(item["path"].AsString()) == extensions_[i].path) item.Set("enabled", json::Value::Bool(enabled));
                  }
                  c.Set("extensions", entries);
                  WriteFileAtomic(profileDir_ + L"\\extensions.json", json::Serialize(c));
                }
              }
              extensions_[i].result = hr; SaveExtensionStatus(); return S_OK;
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
        const std::wstring url = ToNavigableUrl(Trim(FromUtf8(message["url"].AsString())));
        changed = !url.empty() && bookmarks_.AddUrl(*parent, index, title, url) != nullptr;
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
      changed = bookmarks_.SetUrl(id, url) || changed;
    }
  } else if (type == "bm.remove") {
    changed = bookmarks_.Remove(static_cast<int64_t>(message["id"].AsNumber(0)));
  } else if (type == "bm.move") {
    changed = bookmarks_.Move(static_cast<int64_t>(message["id"].AsNumber(0)),
                              static_cast<int64_t>(message["parent"].AsNumber(0)),
                              static_cast<size_t>(message["index"].AsNumber(1e9)));
  } else if (type == "bm.sources") {
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
    PostToPage(tab, out);
  } else if (type == "bm.import") {
    // 只接受探测到的导入源，页面不能指定任意文件
    const std::wstring file = FromUtf8(message["file"].AsString());
    std::string error = "unknown import source";
    int count = -1;
    for (const BookmarkImportSource& source : BookmarkStore::DetectImportSources()) {
      if (_wcsicmp(source.file.c_str(), file.c_str()) == 0) {
        error.clear();
        count = bookmarks_.Import(source.file, source.name, &error);
        break;
      }
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

void BrowserWindow::OpenNewWindowAsTab(ICoreWebView2NewWindowRequestedEventArgs* args, bool activate) {
  auto tab = std::make_unique<Tab>();
  tab->id = nextTabId_++;
  args->GetDeferral(tab->newWindowDeferral.GetAddressOf());
  tab->newWindowArgs = args;
  CoTaskString uri;
  if (SUCCEEDED(args->get_Uri(uri.Put()))) {
    tab->url = uri.Str();
  }
  tabs_.push_back(std::move(tab));
  Relayout();
  if (activate) {
    Activate(tabs_.size() - 1);
  } else {
    InvalidateChrome();
  }
  CreateController(*tabs_.back());
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
    webview3->TrySuspend(Callback<ICoreWebView2TrySuspendCompletedHandler>(
                             [this, id](HRESULT result, BOOL ok) -> HRESULT {
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
  std::string out = "{\"id\":" + std::to_string(tabId) + ",\"index\":" + std::to_string(index) +
                    ",\"windowId\":1,\"groupId\":-1,\"active\":false,\"highlighted\":false,\"pinned\":false,"
                    "\"audible\":false,\"discarded\":false,\"autoDiscardable\":true,\"frozen\":false,"
                    "\"incognito\":false,\"mutedInfo\":{\"muted\":false},\"status\":\"complete\",\"width\":" +
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

void BrowserWindow::NewTab(const std::wstring& url, bool activate) {
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
  tabs_.push_back(std::move(tab));
  Relayout();
  if (activate) {
    Activate(tabs_.size() - 1);
  } else {
    InvalidateChrome();
  }
  CreateController(*tabs_.back());  // 已有控件或正在创建时什么也不做
  if (options_.spare && !spare_) {
    SetTimer(hwnd_, kTimerSpare, 500, nullptr);
  }
  if (url.empty() && activate) {
    FocusAddressBar();
  }
}

void BrowserWindow::CloseTab(size_t index) {
  if (index >= tabs_.size()) {
    return;
  }
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
  AbandonNewWindow(*closing);
  if (closing->controller) {
    closing->controller->Close();
  }
  if (tabs_.empty()) {
    DestroyWindow(hwnd_);
    return;
  }
  size_t next = active_;
  if (index < active_) {
    next = active_ - 1;
  } else if (index == active_) {
    next = std::min(index, tabs_.size() - 1);
  }
  hover_ = {};
  Relayout();
  Activate(next);
}

void BrowserWindow::Activate(size_t index) {
  CloseExtensionPopup();
  if (index >= tabs_.size()) {
    return;
  }
  active_ = index;
  PostMessageW(hwnd_, kMsgTabLimit, 0, 0);
  // 先显示新标签再隐藏旧的，中间不露底色
  PlaceTab(*tabs_[index]);
  for (auto& tab : tabs_) {
    if (tab.get() != tabs_[index].get()) {
      PlaceTab(*tab);
    }
  }
  editing_ = false;
  UpdateAddressBar();
  UpdateWindowTitle();
  InvalidateChrome();
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
    tab.controller->put_Bounds(ContentRect());
    SetTabVisible(tab, true);
    tab.suspended = false;  // 控件可见时内核自动恢复
    tab.suspending = false;
    return;
  }
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
  if (msg.message != WM_KEYDOWN && msg.message != WM_SYSKEYDOWN) {
    return false;
  }
  if (msg.hwnd != hwnd_ && msg.hwnd != edit_) {
    return false;
  }
  const UINT key = static_cast<UINT>(msg.wParam);
  if (key == VK_ESCAPE && extensionPopupWindow_) { CloseExtensionPopup(); return true; }
  if (key == VK_ESCAPE && dragBookmarkId_) { CancelBookmarkDrag(); return true; }
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
    if (key == 'A' && GetKeyState(VK_CONTROL) < 0) {
      SendMessageW(edit_, EM_SETSEL, 0, -1);
      return true;
    }
  }
  return false;
}

bool BrowserWindow::DispatchShortcut(UINT key, bool repeat) {
  const bool ctrl = GetKeyState(VK_CONTROL) < 0;
  const bool shift = GetKeyState(VK_SHIFT) < 0;
  const bool alt = GetKeyState(VK_MENU) < 0;
  WPARAM command = kCmdNone;
  bool once = false;  // 按住不连发
  if (ctrl && !alt) {
    switch (key) {
      case 'T':
        command = kCmdNewTab;
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
      case 'L':
        command = kCmdFocusAddress;
        break;
      case 'R':
        command = kCmdReload;
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
      case VK_F5:
        command = kCmdReload;
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
    PostMessageW(hwnd_, kMsgCommand, command, 0);
  }
  return true;
}

void BrowserWindow::ExecuteCommand(WPARAM command) {
  Tab* tab = ActiveTab();
  switch (command) {
    case kCmdNewTab:
      NewTab(L"");
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
    case kCmdFocusAddress:
      FocusAddressBar();
      break;
    case kCmdReload:
      if (tab && tab->webview) {
        tab->webview->Reload();
      }
      break;
    case kCmdStopOrReload:
      if (tab && tab->webview) {
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
  SetFocus(edit_);
  SendMessageW(edit_, EM_SETSEL, 0, -1);
}

void BrowserWindow::UpdateAddressBar() {
  if (editing_ || !edit_) {
    return;
  }
  const Tab* tab = ActiveTab();
  const std::wstring text = tab ? DisplayUrl(tab->url) : std::wstring();
  settingAddress_ = true;
  SetWindowTextW(edit_, text.c_str());
  settingAddress_ = false;
}

void BrowserWindow::UpdateWindowTitle() {
  const Tab* tab = ActiveTab();
  const std::wstring title =
      (tab && !tab->title.empty() ? tab->title : std::wstring(kNewTabTitle)) + L" - " + kAppName;
  SetWindowTextW(hwnd_, title.c_str());
}

void BrowserWindow::NavigateFromAddressBar() {
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
  if (tab->agent && !tab->leased) {
    // 用户在空闲的智能体标签里自己输网址：这个标签归用户了，不再放回池里
    tab->agent = false;
    ComPtr<ICoreWebView2Settings> settings;
    if (tab->webview && SUCCEEDED(tab->webview->get_Settings(settings.GetAddressOf()))) {
      settings->put_AreDefaultScriptDialogsEnabled(TRUE);
    }
  }
  tab->url = url;
  if (tab->webview) {
    tab->webview->Navigate(url.c_str());
    tab->controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
  } else {
    tab->pendingUrl = url;  // 控件还没建好：建好后打开
  }
  UpdateAddressBar();
  InvalidateChrome();
}

void BrowserWindow::OpenUrl(const std::wstring& url, bool newTab) {
  Tab* tab = ActiveTab();
  if (newTab || !tab || (tab->agent && tab->leased)) {
    NewTab(url, true);
    return;
  }
  tab->url = url;
  if (tab->webview) {
    tab->webview->Navigate(url.c_str());
  } else {
    tab->pendingUrl = url;
  }
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

void BrowserWindow::ShowBookmarkMenu(const std::vector<BookmarkNode*>& nodes, BookmarkNode* openAllFolder, POINT screen,
                                     bool alignRight) {
  std::vector<BookmarkNode*> urls;
  std::vector<BookmarkNode*> folders;
  BookmarkMenuIcons icons(dpi_);  // includes submenu images; handles outlive the entire native modal menu
  auto build = [&](auto&& self, HMENU menu, const std::vector<BookmarkNode*>& list, BookmarkNode* owner) -> void {
    if (list.empty()) {
      AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"（空）");
      return;
    }
    for (BookmarkNode* node : list) {
      if (urls.size() + folders.size() >= kMenuItemLimit) {
        break;
      }
      if (node->folder) {
        std::vector<BookmarkNode*> children;
        for (auto& child : node->children) {
          children.push_back(child.get());
        }
        HMENU sub = CreatePopupMenu();
        self(self, sub, children, node);
        icons.Append(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(sub), MenuText(node->title), L"", true);
      } else {
        urls.push_back(node);
        icons.Append(menu, MF_STRING, kMenuItemBase + urls.size() - 1, MenuText(BookmarkLabel(*node)),
                     favicons_.Request(node->url), false);
      }
    }
    const int count = owner ? CountUrls(*owner) : 0;
    if (count >= 2) {
      folders.push_back(owner);
      AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
      const std::wstring label = L"全部在新标签页中打开（" + std::to_wstring(count) + L"）";
      AppendMenuW(menu, MF_STRING, kMenuOpenAllBase + folders.size() - 1, label.c_str());
    }
  };
  HMENU menu = CreatePopupMenu();
  build(build, menu, nodes, openAllFolder);
  const UINT flags = TPM_RETURNCMD | TPM_NONOTIFY | TPM_TOPALIGN | (alignRight ? TPM_RIGHTALIGN : TPM_LEFTALIGN);
  const UINT command = static_cast<UINT>(TrackPopupMenuEx(menu, flags, screen.x, screen.y, hwnd_, nullptr));
  DestroyMenu(menu);
  if (command >= kMenuOpenAllBase && command - kMenuOpenAllBase < folders.size()) {
    for (const auto& child : folders[command - kMenuOpenAllBase]->children) {
      if (!child->folder) {
        NewTab(child->url, false);
      }
    }
  } else if (command >= kMenuItemBase && command - kMenuItemBase < urls.size()) {
    OpenUrl(urls[command - kMenuItemBase]->url, GetKeyState(VK_CONTROL) < 0);
  }
}

void BrowserWindow::ShowBookmarkContextMenu(BookmarkNode& node, POINT screen) {
  HMENU menu = CreatePopupMenu();
  if (node.folder) {
    const int urls = CountUrls(node);
    AppendMenuW(menu, MF_STRING | (urls ? 0 : MF_GRAYED), kMenuOpenAll,
                (L"全部在新标签页中打开（" + std::to_wstring(urls) + L"）").c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuEdit, L"重命名…");
  } else {
    AppendMenuW(menu, MF_STRING, kMenuOpen, L"打开");
    AppendMenuW(menu, MF_STRING, kMenuOpenNewTab, L"在新标签页中打开");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuEdit, L"编辑…");
  }
  AppendMenuW(menu, MF_STRING, kMenuDelete, L"删除");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, kMenuBookmarkManager, L"书签管理器");
  const UINT command = static_cast<UINT>(TrackPopupMenuEx(
      menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN | TPM_TOPALIGN, screen.x, screen.y, hwnd_, nullptr));
  DestroyMenu(menu);
  const int64_t id = node.id;
  switch (command) {
    case kMenuOpen:
      OpenUrl(node.url, false);
      break;
    case kMenuOpenNewTab:
      NewTab(node.url, false);
      break;
    case kMenuOpenAll:
      for (const auto& child : node.children) {
        if (!child->folder) {
          NewTab(child->url, false);
        }
      }
      break;
    case kMenuEdit:
      if (node.folder) OpenBookmarkManager(L"#edit=" + std::to_wstring(id));
      else EditBookmark(&node, node.title, node.url);
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
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, kMenuBookmarkManager, L"书签管理器\tCtrl+Shift+O");
  AppendMenuW(menu, MF_STRING | (settings_.bookmarkBar ? MF_CHECKED : 0), kMenuToggleBar, L"显示书签栏\tCtrl+Shift+B");
  AppendMenuW(menu, MF_STRING, kMenuImport, L"导入书签…");
  AppendMenuW(menu, MF_STRING, kMenuExtensions, L"扩展管理");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, kMenuDefaultBrowser, L"设为默认浏览器…");
  AppendMenuW(menu, MF_STRING, kMenuAbout, L"关于极速浏览器");
  const UINT command = static_cast<UINT>(TrackPopupMenuEx(
      menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTALIGN | TPM_TOPALIGN, screen.x, screen.y, hwnd_, nullptr));
  DestroyMenu(menu);
  switch (command) {
    case kMenuNewTab:
      NewTab(L"");
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
    case kMenuDefaultBrowser:
      RegisterAsDefaultBrowser();
      break;
    case kMenuAbout:
      ShowAbout();
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
        tab.url = url;
        tab.webview->Navigate(url.c_str());
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
  bookmarks_.Save();
  Relayout();
  PlaceAllTabs();  // 书签栏出现或消失时网页区上沿会变
  InvalidateChrome();
  BroadcastBookmarks();
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
  }
}

void BrowserWindow::SaveSettings() {
  if (profileDir_.empty()) {
    return;
  }
  std::string out = "{\"bookmarkBar\":";
  out += settings_.bookmarkBar ? "true" : "false";
  out += ",\"importedDefaultBrowser\":";
  out += settings_.importedDefaultBrowser ? "true" : "false";
  out += ",\"maxRegularTabs\":" + std::to_string(settings_.maxRegularTabs);
  out += ",\"bookmarkFolder\":" + std::to_string(settings_.bookmarkFolder) + "}";
  EnsureDirectory(profileDir_);
  WriteFileAtomic(profileDir_ + L"\\jisu-settings.json", out);
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
  ok &= SetRegString(HKEY_CURRENT_USER, classes + L"\\shell\\open\\command", nullptr, quoted + L" \"%1\"");
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

void BrowserWindow::OnCopyData(const COPYDATASTRUCT& data) {
  if (data.dwData != kCopyDataOpenUrl || data.cbData % sizeof(wchar_t) != 0 || data.cbData > 64 * 1024) {
    return;
  }
  std::wstring url(static_cast<const wchar_t*>(data.lpData), data.cbData / sizeof(wchar_t));
  while (!url.empty() && url.back() == L'\0') {
    url.pop_back();
  }
  url = Trim(url);
  NewTab(url.empty() ? std::wstring() : ToNavigableUrl(url), true);
  // 用户自己点了链接或图标：把窗口带到前面（发送方已调 AllowSetForegroundWindow）
  if (IsIconic(hwnd_)) {
    ShowWindow(hwnd_, SW_RESTORE);
  }
  SetForegroundWindow(hwnd_);
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
  if (bridge_ && bridge_->HandleMessage(msg, wParam, lParam)) {
    return 0;
  }
  switch (msg) {
    case WM_NCCALCSIZE:
      if (wParam) {
        auto* p = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);
        if (IsZoomed(hwnd_)) {
          MONITORINFO monitor{sizeof(MONITORINFO)};
          if (GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &monitor)) p->rgrc[0] = monitor.rcWork;
        }
        return 0;
      }
      break;
    case WM_NCHITTEST: {
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
    case kMsgTabLimit:
      EnforceTabLimit();
      return 0;
    case kMsgFavicon:
      painter_.ClearFaviconBitmaps();
      InvalidateChrome();
      BroadcastBookmarks();
      return 0;
    case WM_CREATE:
      OnCreate();
      return 0;
    case WM_ERASEBKGND:
      return 1;  // 整个客户区由 Direct2D 画，不让 GDI 先擦一遍
    case WM_PAINT:
      painter_.Paint(palette_, layout_, BuildChromeState());
      ValidateRect(hwnd_, nullptr);
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
      const auto* suggested = reinterpret_cast<const RECT*>(lParam);
      SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                   suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
      Relayout();
      InvalidateRect(hwnd_, nullptr, FALSE);
      return 0;
    }
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
      CancelBookmarkDrag();
      break;
    case WM_KEYDOWN:
      if (wParam == VK_ESCAPE && dragBookmarkId_) { CancelBookmarkDrag(); return 0; }
      break;
    case WM_MOUSEMOVE:
      OnMouseMove(PointFromLParam(lParam));
      return 0;
    case WM_MOUSELEAVE:
      trackingMouse_ = false;
      if (hover_.kind != ChromeHit::kNone) {
        hover_ = {};
        InvalidateChrome();
      }
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
      if(static_cast<uint64_t>(wParam) == extensionPopupGeneration_) CloseExtensionPopup();
      return 0;
    case kMsgOpenExtensionSettings: {
      std::unique_ptr<std::wstring> url(reinterpret_cast<std::wstring*>(lParam));
      CloseExtensionPopup(); if(url) NewTab(*url); return 0;
    }
    case WM_ACTIVATE:
      if (LOWORD(wParam) == WA_INACTIVE && reinterpret_cast<HWND>(lParam) != extensionPopupWindow_) CloseExtensionPopup();
      break;
    case WM_COMMAND:
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
        OnCopyData(*data);
      }
      return TRUE;
    case WM_TIMER:
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
      break;
    case WM_SETTINGCHANGE:
      if (lParam && wcscmp(reinterpret_cast<const wchar_t*>(lParam), L"ImmersiveColorSet") == 0) {
        OnThemeChanged();
      }
      break;
    case kMsgCommand:
      ExecuteCommand(wParam);
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
  edit_ = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | ES_LEFT | ES_AUTOHSCROLL, 0, 0,
                          0, 0, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAddressBarId)), instance_,
                          nullptr);
  SendMessageW(edit_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"搜索或输入网址"));
  SendMessageW(edit_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
  UpdateEditFont();
  Metrics::Get().Mark("addressBarCreated");

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

void BrowserWindow::OnDestroy() {
  WriteMetrics(true);  // 测量中被关掉也留下已有的时间线（已写过则不重复）
  destroyed_ = true;
  alive_.reset();
  favicons_.Stop();
  CloseExtensionPopup();
  for (auto& e : extensions_) if(e.toolbarIcon){DestroyIcon(e.toolbarIcon);e.toolbarIcon=nullptr;}
  if(extensionTooltip_){DestroyWindow(extensionTooltip_);extensionTooltip_=nullptr;}
  bridge_.reset();  // 先断开所有智能体连接，再关控件
  for (auto& tab : tabs_) {
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
    spare_->controller->Close();
  }
  tabs_.clear();
  spare_.reset();
  environment_.Reset();
  if (agentHost_) {
    DestroyWindow(agentHost_);
    agentHost_ = nullptr;
  }
  RemovePropW(hwnd_, kProfileProp);
  PostQuitMessage(0);
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

void BrowserWindow::Relayout() {
  RECT client{};
  GetClientRect(hwnd_, &client);
  ChromeLayout& l = layout_;
  l.scale = static_cast<float>(dpi_) / 96.0f;
  l.width = static_cast<float>(client.right - client.left) / l.scale;

  l.minimize = D2D1::RectF(l.width - 138, 0, l.width - 92, l.tabStripHeight);
  l.maximize = D2D1::RectF(l.width - 92, 0, l.width - 46, l.tabStripHeight);
  l.windowClose = D2D1::RectF(l.width - 46, 0, l.width, l.tabStripHeight);
  auto strip = LayoutTabStrip(l.width, tabs_.size());
  l.tabs = std::move(strip.tabs);
  l.tabCloses = std::move(strip.closes);
  l.newTab = strip.newTab;

  const float top = l.tabStripHeight;
  l.back = D2D1::RectF(8.0f, top + 4.0f, 40.0f, top + 36.0f);
  l.forward = D2D1::RectF(44.0f, top + 4.0f, 76.0f, top + 36.0f);
  l.reload = D2D1::RectF(80.0f, top + 4.0f, 112.0f, top + 36.0f);
  l.menu = D2D1::RectF(std::max(160.0f, l.width - 40.0f), top + 4.0f, std::max(192.0f, l.width - 8.0f), top + 36.0f);
  const float toolbarRight = LayoutExtensionToolbar(l.menu.left - 6.0f, top, l.scale);
  l.omnibox = D2D1::RectF(120.0f, top + 5.0f, std::max(160.0f, toolbarRight - 6.0f), top + 35.0f);
  l.star = D2D1::RectF(l.omnibox.right - 32.0f, l.omnibox.top + 2.0f, l.omnibox.right - 4.0f, l.omnibox.bottom - 2.0f);
  LayoutBookmarkBar();
  l.separatorPx = static_cast<int>(std::lround((l.tabStripHeight + l.toolbarHeight + l.bookmarkBarHeight) * l.scale));
  l.contentTopPx = l.separatorPx + 1;

  if (edit_) {
    const int left = static_cast<int>(std::lround((l.omnibox.left + 12.0f) * l.scale));
    const int right = static_cast<int>(std::lround((l.star.left - 2.0f) * l.scale));
    const int centerY = static_cast<int>(std::lround((l.omnibox.top + l.omnibox.bottom) * 0.5f * l.scale));
    MoveWindow(edit_, left, centerY - editHeightPx_ / 2, std::max(0, right - left), editHeightPx_, TRUE);
  }
}

void BrowserWindow::LayoutBookmarkBar() {
  ChromeLayout& l = layout_;
  barItems_.clear();
  overflowItems_.clear();
  l.bookmarks.clear();
  l.hasOverflow = false;
  const bool show = profileLoaded_ && settings_.bookmarkBar && !bookmarks_.Bar().children.empty();
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
  client.top = std::min<LONG>(client.bottom, layout_.contentTopPx);
  return client;
}

ChromeState BrowserWindow::BuildChromeState() const {
  ChromeState state;
  state.tabs.reserve(tabs_.size());
  for (size_t i = 0; i < tabs_.size(); ++i) {
    const Tab& tab = *tabs_[i];
    std::wstring title = tab.title.empty() ? std::wstring(tab.agent ? kAgentTabTitle : kNewTabTitle) : tab.title;
    state.tabs.push_back(ChromeTab{std::move(title), i == active_, tab.loading, tab.agent, tab.agentAwake, tab.agent ? L"" : favicons_.Request(tab.url)});
  }
  state.hover = hover_;
  state.bookmarkDrag = draggingBookmark_;
  state.bookmarkDrop = dropBookmark_;
  state.bookmarkDropIntoFolder = dropIntoFolder_;
  if (const Tab* tab = ActiveTab()) {
    state.canBack = tab->canBack;
    state.canForward = tab->canForward;
    state.loading = tab->loading;
    state.canStar = profileLoaded_ && IsStarrable(tab->url);
    if (state.canStar) {
      state.starred = const_cast<BookmarkStore&>(bookmarks_).FindByUrl(tab->url) != nullptr;
    }
  }
  state.bookmarks.reserve(barItems_.size());
  for (const BookmarkNode* node : barItems_) {
    state.bookmarks.push_back(ChromeBookmark{node->title, node->folder, node->folder ? L"" : favicons_.Request(node->url)});
  }
  state.maximized = IsZoomed(hwnd_) != FALSE;
  state.omniboxFocused = omniboxFocused_;
  state.message = message_;
  return state;
}

ChromeHit BrowserWindow::HitTest(POINT point) const {
  const float x = static_cast<float>(point.x) / layout_.scale;
  const float y = static_cast<float>(point.y) / layout_.scale;
  auto inside = [x, y](const D2D1_RECT_F& r) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; };
  if (inside(layout_.minimize)) return ChromeHit{ChromeHit::kMinimize, -1};
  if (inside(layout_.maximize)) return ChromeHit{ChromeHit::kMaximize, -1};
  if (inside(layout_.windowClose)) return ChromeHit{ChromeHit::kWindowClose, -1};
  for (size_t i = 0; i < layout_.tabs.size(); ++i) {
    if (inside(layout_.tabs[i])) {
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
  return {};
}

void BrowserWindow::InvalidateChrome() {
  if (hwnd_) {
    InvalidateRect(hwnd_, nullptr, FALSE);
  }
}

void BrowserWindow::OnMouseMove(POINT point) {
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
  const ChromeHit hit = HitTest(point);
  if (hit != hover_) {
    hover_ = hit;
    InvalidateChrome();
  }
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
    Activate(static_cast<size_t>(pressed_.index));  // 标签按下即切换
  }
}

void BrowserWindow::OnLButtonUp(POINT point) {
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
      CloseTab(static_cast<size_t>(hit.index));
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
  if (hit.kind == ChromeHit::kTab || hit.kind == ChromeHit::kTabClose) {
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
  if (hit.kind == ChromeHit::kBookmark && static_cast<size_t>(hit.index) < barItems_.size()) {
    POINT screen = point;
    ClientToScreen(hwnd_, &screen);
    ShowBookmarkContextMenu(*barItems_[static_cast<size_t>(hit.index)], screen);
  }
}

}  // namespace jisu
