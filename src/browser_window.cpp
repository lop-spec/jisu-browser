#include "browser_window.h"

#include "metrics.h"
#include "ntp_html.h"

#include <WebView2EnvironmentOptions.h>
#include <commctrl.h>
#include <dwmapi.h>

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

using Microsoft::WRL::Callback;
using Microsoft::WRL::Make;

namespace jisu {
namespace {

constexpr wchar_t kWindowClass[] = L"JisuBrowserWindow";
constexpr wchar_t kAppName[] = L"极速浏览器";
constexpr wchar_t kNewTabTitle[] = L"新标签页";
// 新标签页经虚拟主机映射到随包的 ntp\ 目录；.example 是保留域名，不会和真实网站冲突
constexpr wchar_t kNtpHost[] = L"newtab.jisu.example";
constexpr wchar_t kNtpOrigin[] = L"https://newtab.jisu.example/";
constexpr wchar_t kNtpUrl[] = L"https://newtab.jisu.example/index.html";
constexpr wchar_t kSearchPrefix[] = L"https://www.bing.com/search?q=";

constexpr UINT kMsgCommand = WM_APP + 1;
constexpr UINT_PTR kTimerSpare = 1;
constexpr UINT_PTR kTimerMetrics = 2;
constexpr UINT_PTR kTimerWatchdog = 3;
constexpr UINT_PTR kTimerPreheat = 4;
constexpr int kAddressBarId = 100;

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
  return url.empty() || url == L"about:blank" || url.starts_with(kNtpOrigin);
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

}  // namespace

BrowserWindow::BrowserWindow(const Options& options, GraphicsWarmup& warmup)
    : options_(options), warmup_(warmup), palette_(LoadPalette()) {}

BrowserWindow::~BrowserWindow() {
  if (editFont_) {
    DeleteObject(editFont_);
  }
  if (omniboxBrush_) {
    DeleteObject(omniboxBrush_);
  }
}

// ---------------------------------------------------------------------------
// 启动

bool BrowserWindow::Create(HINSTANCE instance) {
  instance_ = instance;
  WNDCLASSEXW windowClass{};
  windowClass.cbSize = static_cast<UINT>(sizeof(windowClass));
  windowClass.style = CS_HREDRAW | CS_VREDRAW;
  windowClass.lpfnWndProc = &BrowserWindow::WndProcThunk;
  windowClass.hInstance = instance;
  windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  windowClass.hbrBackground = CreateSolidBrush(ToColorRef(palette_.content));
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
  if (options_.preheatSeconds > 0) {
    SetTimer(hwnd_, kTimerPreheat, static_cast<UINT>(options_.preheatSeconds) * 1000u, nullptr);
  }
  return true;
}

void BrowserWindow::ShowFirstFrame(int showCmd) {
  Metrics& metrics = Metrics::Get();
  warmup_.WaitReady();
  metrics.Mark("warmupJoined");
  if (warmup_.d2d) {
    painter_.CreateTarget(warmup_, hwnd_, options_.renderTarget, dpi_);
  }
  warmup_.d3dWarm.Reset();  // 显卡驱动已由渲染目标的设备持有，预热设备用完即放
  metrics.Mark("renderTargetCreated");

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
    hr = environment10->CreateCoreWebView2ControllerWithOptions(hwnd_, controllerOptions.Get(), handler.Get());
  }
  if (FAILED(hr)) {
    hr = environment_->CreateCoreWebView2Controller(hwnd_, handler.Get());
  }
  if (FAILED(hr)) {
    tab.creating = false;
    OnControllerFailed(tab, hr);  // 可能销毁 tab（备用控件），之后不能再用
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
  // 先藏再定位：备用控件和后台标签从头到尾不露面
  if (!show) {
    tab->controller->put_IsVisible(FALSE);
    tab->visible = false;
  }
  ComPtr<ICoreWebView2Controller2> controller2;
  if (SUCCEEDED(tab->controller.As(&controller2))) {
    controller2->put_DefaultBackgroundColor(ToWebViewColor(palette_.content));
  }
  tab->controller->put_Bounds(ContentRect());
  if (show) {
    tab->controller->put_IsVisible(TRUE);
    tab->visible = true;
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
  if (options_.ntp == NtpSource::kFolder) {
    ComPtr<ICoreWebView2_3> webview3;
    if (SUCCEEDED(tab.webview.As(&webview3))) {
      const std::wstring folder = ExeDirectory() + L"\\ntp";
      webview3->SetVirtualHostNameToFolderMapping(kNtpHost, folder.c_str(),
                                                  COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY);
    }
  }

  const uint32_t id = tab.id;
  EventRegistrationToken token{};
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
            }
            return S_OK;
          })
          .Get(),
      &token);
  tab.webview->add_NewWindowRequested(
      Callback<ICoreWebView2NewWindowRequestedEventHandler>(
          [this](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
            OpenNewWindowAsTab(args);
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

// 新标签页回报：「ntp-frame <首帧 Unix 毫秒> <timeOrigin>」与「ntp-fcp <FCP Unix 毫秒>」
void BrowserWindow::OnWebMessage(uint32_t id, ICoreWebView2WebMessageReceivedEventArgs* args) {
  const double hostMs = ProcessClock::NowMs();
  CoTaskString text;
  if (FAILED(args->TryGetWebMessageAsString(text.Put()))) {
    return;
  }
  if (options_.ntp == NtpSource::kFolder) {
    CoTaskString source;
    if (FAILED(args->get_Source(source.Put())) || !source.Str().starts_with(kNtpOrigin)) {
      return;  // 只认新标签页自己的回报
    }
  }
  const std::wstring message = text.Str();
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

void BrowserWindow::OpenNewWindowAsTab(ICoreWebView2NewWindowRequestedEventArgs* args) {
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
  Activate(tabs_.size() - 1);
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
// 标签

void BrowserWindow::NewTab(const std::wstring& url) {
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
  if (newTabMeasureId_ == 0) {
    newTabRequestMs_ = ProcessClock::NowMs();
    newTabMeasureId_ = tab->id;
  }
  tabs_.push_back(std::move(tab));
  Relayout();
  Activate(tabs_.size() - 1);
  CreateController(*tabs_.back());  // 已有控件或正在创建时什么也不做
  if (options_.spare && !spare_) {
    SetTimer(hwnd_, kTimerSpare, 500, nullptr);
  }
  if (url.empty()) {
    FocusAddressBar();
  }
}

void BrowserWindow::CloseTab(size_t index) {
  if (index >= tabs_.size()) {
    return;
  }
  std::unique_ptr<Tab> closing = std::move(tabs_[index]);
  tabs_.erase(tabs_.begin() + static_cast<std::ptrdiff_t>(index));
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
  if (index >= tabs_.size()) {
    return;
  }
  active_ = index;
  // 先显示新标签再隐藏旧的，中间不露底色
  Tab& current = *tabs_[index];
  SetTabVisible(current, options_.preheatSeconds <= 0 && !IsIconic(hwnd_));
  for (auto& tab : tabs_) {
    if (tab.get() != &current) {
      SetTabVisible(*tab, false);
    }
  }
  editing_ = false;
  UpdateAddressBar();
  UpdateWindowTitle();
  InvalidateChrome();
}

void BrowserWindow::SetTabVisible(Tab& tab, bool visible) {
  if (!tab.controller) {
    return;
  }
  if (visible) {
    tab.controller->put_Bounds(ContentRect());
  }
  if (tab.visible != visible) {
    tab.controller->put_IsVisible(visible ? TRUE : FALSE);
    tab.visible = visible;
  }
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
  const std::wstring text = (tab && !IsNewTabPageUrl(tab->url)) ? tab->url : std::wstring();
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
  tab->url = url;
  if (tab->webview) {
    tab->webview->Navigate(url.c_str());
    tab->controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
  } else {
    tab->pendingUrl = url;  // 控件还没建好：建好后打开
  }
  UpdateAddressBar();
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
  switch (msg) {
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
      if (wParam == SIZE_MINIMIZED) {
        for (auto& tab : tabs_) {
          SetTabVisible(*tab, false);  // 最小化时让内核降载
        }
        return 0;
      }
      painter_.Resize(LOWORD(lParam), HIWORD(lParam));
      Relayout();
      if (Tab* tab = ActiveTab(); tab && options_.preheatSeconds <= 0) {
        SetTabVisible(*tab, true);
      }
      if (spare_ && spare_->controller) {
        spare_->controller->put_Bounds(ContentRect());
      }
      return 0;
    case WM_DPICHANGED: {
      dpi_ = HIWORD(wParam);
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
    case WM_CTLCOLOREDIT:
      if (reinterpret_cast<HWND>(lParam) == edit_) {
        const HDC dc = reinterpret_cast<HDC>(wParam);
        SetTextColor(dc, ToColorRef(palette_.text));
        SetBkColor(dc, ToColorRef(palette_.omnibox));
        return reinterpret_cast<LRESULT>(omniboxBrush_);
      }
      break;
    case WM_COMMAND:
      if (LOWORD(wParam) == kAddressBarId) {
        switch (HIWORD(wParam)) {
          case EN_SETFOCUS:
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
  for (auto& tab : tabs_) {
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

  constexpr float kLeft = 8.0f;
  constexpr float kTabTop = 6.0f;
  constexpr float kNewTabWidth = 28.0f;
  constexpr float kRightReserve = 16.0f;
  const size_t count = tabs_.size();
  const float available = std::max(0.0f, l.width - kLeft - kNewTabWidth - kRightReserve);
  const float tabWidth = count == 0 ? 0.0f : std::clamp(available / static_cast<float>(count), 40.0f, 240.0f);
  l.tabs.clear();
  l.tabCloses.clear();
  for (size_t i = 0; i < count; ++i) {
    const float left = kLeft + static_cast<float>(i) * tabWidth;
    const D2D1_RECT_F tab = D2D1::RectF(left, kTabTop, left + tabWidth, l.tabStripHeight);
    l.tabs.push_back(tab);
    const float centerY = (tab.top + tab.bottom) * 0.5f;
    l.tabCloses.push_back(D2D1::RectF(tab.right - 25.0f, centerY - 9.0f, tab.right - 7.0f, centerY + 9.0f));
  }
  const float newTabLeft = kLeft + static_cast<float>(count) * tabWidth + 4.0f;
  l.newTab = D2D1::RectF(newTabLeft, kTabTop + 3.0f, newTabLeft + kNewTabWidth, kTabTop + 29.0f);

  const float top = l.tabStripHeight;
  l.back = D2D1::RectF(8.0f, top + 4.0f, 40.0f, top + 36.0f);
  l.forward = D2D1::RectF(44.0f, top + 4.0f, 76.0f, top + 36.0f);
  l.reload = D2D1::RectF(80.0f, top + 4.0f, 112.0f, top + 36.0f);
  l.omnibox = D2D1::RectF(120.0f, top + 5.0f, std::max(160.0f, l.width - 12.0f), top + 35.0f);
  l.separatorPx = static_cast<int>(std::lround((l.tabStripHeight + l.toolbarHeight) * l.scale));
  l.contentTopPx = l.separatorPx + 1;

  if (edit_) {
    const int left = static_cast<int>(std::lround((l.omnibox.left + 12.0f) * l.scale));
    const int right = static_cast<int>(std::lround((l.omnibox.right - 12.0f) * l.scale));
    const int centerY = static_cast<int>(std::lround((l.omnibox.top + l.omnibox.bottom) * 0.5f * l.scale));
    MoveWindow(edit_, left, centerY - editHeightPx_ / 2, std::max(0, right - left), editHeightPx_, TRUE);
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
    state.tabs.push_back(
        ChromeTab{tab.title.empty() ? std::wstring(kNewTabTitle) : tab.title, i == active_, tab.loading});
  }
  state.hover = hover_;
  if (const Tab* tab = ActiveTab()) {
    state.canBack = tab->canBack;
    state.canForward = tab->canForward;
    state.loading = tab->loading;
  }
  state.omniboxFocused = omniboxFocused_;
  state.message = message_;
  return state;
}

ChromeHit BrowserWindow::HitTest(POINT point) const {
  const float x = static_cast<float>(point.x) / layout_.scale;
  const float y = static_cast<float>(point.y) / layout_.scale;
  auto inside = [x, y](const D2D1_RECT_F& r) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; };
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
  return {};
}

void BrowserWindow::InvalidateChrome() {
  if (hwnd_) {
    InvalidateRect(hwnd_, nullptr, FALSE);
  }
}

void BrowserWindow::OnMouseMove(POINT point) {
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
  pressed_ = HitTest(point);
  if (pressed_.kind == ChromeHit::kTab) {
    Activate(static_cast<size_t>(pressed_.index));  // 标签按下即切换
  }
}

void BrowserWindow::OnLButtonUp(POINT point) {
  const ChromeHit hit = HitTest(point);
  const ChromeHit pressed = pressed_;
  pressed_ = {};
  if (hit != pressed) {
    return;
  }
  switch (hit.kind) {
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
    default:
      break;
  }
}

void BrowserWindow::OnMButtonUp(POINT point) {
  const ChromeHit hit = HitTest(point);
  if (hit.kind == ChromeHit::kTab || hit.kind == ChromeHit::kTabClose) {
    CloseTab(static_cast<size_t>(hit.index));
  }
}

}  // namespace jisu
