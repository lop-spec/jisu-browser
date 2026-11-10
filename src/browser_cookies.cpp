// Cookie 守卫：定时取出全部 cookie，站点 cookie 超限时回收最早的「带 ID 的一次性 cookie」。
// 策略（回收谁、保留谁）在 cookie_policy.h；这里只管取、删、配置和状态文件。
// 配置 <用户数据目录>\cookie-guard.json（没有就用默认值，每轮重读，改了不用重启）：
//   {"enabled":true,"dryRun":false,"maxPerSite":20,"keepUnits":10,"intervalSeconds":300}
// 状态 <用户数据目录>\cookie-guard-status.json：每轮的站点计数与回收数，不含 cookie 名称或值。
#include "browser_window.h"
#include "json.h"

#include <algorithm>
#include <chrono>
#include <cstdio>

using Microsoft::WRL::Callback;

namespace jisu {
namespace {
constexpr UINT kFirstCheckMs = 20000;          // 内核就绪后先让首屏和扩展装完再查
constexpr ULONGLONG kCheckStuckMs = 60000;     // 回调超过一分钟没回来就当丢了
constexpr size_t kMaxEvictPerRound = 500;      // 单轮最多删这么多，余下的下一轮再来
constexpr size_t kStatusSites = 12;
}  // namespace

void BrowserWindow::StartCookieGuard() {
  if (destroyed_ || options_.appMode || !hwnd_) return;
  std::string problem;
  const CookieGuardConfig config = LoadCookieGuardConfig(problem);
  SetTimer(hwnd_, kTimerCookieGuard, std::min<UINT>(kFirstCheckMs, static_cast<UINT>(config.intervalSeconds) * 1000u), nullptr);
}

ComPtr<ICoreWebView2> BrowserWindow::AnyLiveWebView() const {
  const auto usable = [](const Tab* tab) { return tab && tab->webview && tab->controller && !tab->creating; };
  for (const auto& tab : tabs_) {
    if (usable(tab.get())) return tab->webview;
  }
  for (const auto& tab : agentTabs_) {
    if (usable(tab.get())) return tab->webview;
  }
  if (usable(spare_.get())) return spare_->webview;
  return nullptr;
}

CookieGuardConfig BrowserWindow::LoadCookieGuardConfig(std::string& problem) const {
  CookieGuardConfig config;
  problem.clear();
  std::string text;
  if (!ReadFileBytes(profileDir_ + L"\\cookie-guard.json", text)) return config;  // 没有文件 = 默认值
  json::Value value;
  if (!json::Parse(text, value) || !value.IsObject()) {
    problem = "cookie-guard.json is not valid JSON; defaults in use";
    return config;
  }
  config.enabled = value["enabled"].AsBool(config.enabled);
  config.dryRun = value["dryRun"].AsBool(config.dryRun);
  config.maxPerSite = static_cast<size_t>(std::clamp(value["maxPerSite"].AsNumber(static_cast<double>(config.maxPerSite)), 0.0, 10000.0));
  config.keepUnits = static_cast<size_t>(std::clamp(value["keepUnits"].AsNumber(static_cast<double>(config.keepUnits)), 0.0, 1000.0));
  config.intervalSeconds = static_cast<int>(std::clamp(value["intervalSeconds"].AsNumber(config.intervalSeconds), 10.0, 86400.0));
  return config;
}

void BrowserWindow::RunCookieGuard() {
  if (destroyed_ || options_.appMode || !hwnd_) return;
  std::string problem;
  const CookieGuardConfig config = LoadCookieGuardConfig(problem);
  // 下一轮先排上：这一轮出什么事都不影响以后
  SetTimer(hwnd_, kTimerCookieGuard, static_cast<UINT>(config.intervalSeconds) * 1000u, nullptr);
  if (cookieGuardRunning_ && GetTickCount64() - cookieGuardStartedAt_ < kCheckStuckMs) return;
  cookieGuardRunning_ = false;

  ComPtr<ICoreWebView2> webview = AnyLiveWebView();
  ComPtr<ICoreWebView2_2> view2;
  ComPtr<ICoreWebView2CookieManager> manager;
  // 内核还没建好或正在重建：没有可用的页面，下一轮再来
  if (!webview || FAILED(webview.As(&view2)) || FAILED(view2->get_CookieManager(manager.GetAddressOf())) || !manager) return;
  if (!config.enabled || config.maxPerSite == 0) {
    WriteCookieGuardStatus(config, CookiePlan{}, 0, 0, problem.empty() ? "disabled" : problem);
    return;
  }

  cookieGuardRunning_ = true;
  cookieGuardStartedAt_ = GetTickCount64();
  const std::weak_ptr<int> alive = alive_;
  const HRESULT started = manager->GetCookies(
      nullptr,  // 空 = 当前资料里的全部 cookie
      Callback<ICoreWebView2GetCookiesCompletedHandler>(
          [this, alive, manager, config, problem](HRESULT result, ICoreWebView2CookieList* list) -> HRESULT {
            if (alive.expired() || destroyed_) return S_OK;
            cookieGuardRunning_ = false;
            if (FAILED(result) || !list) {
              char failure[64];
              sprintf_s(failure, "get-cookies-failed 0x%08X", static_cast<unsigned>(result));
              WriteCookieGuardStatus(config, CookiePlan{}, 0, 0, failure);
              return S_OK;
            }
            UINT count = 0;
            list->get_Count(&count);
            std::vector<CookieInfo> infos;
            std::vector<ComPtr<ICoreWebView2Cookie>> handles;
            infos.reserve(count);
            handles.reserve(count);
            for (UINT i = 0; i < count; ++i) {
              ComPtr<ICoreWebView2Cookie> cookie;
              if (FAILED(list->GetValueAtIndex(i, cookie.GetAddressOf())) || !cookie) continue;
              CoTaskString name, domain, path;
              cookie->get_Name(name.Put());
              cookie->get_Domain(domain.Put());
              cookie->get_Path(path.Put());
              double expires = -1;
              BOOL session = TRUE;
              cookie->get_Expires(&expires);
              cookie->get_IsSession(&session);
              CookieInfo info;
              info.name = ToUtf8(name.Str());
              info.domain = ToUtf8(domain.Str());
              info.path = ToUtf8(path.Str());
              info.expires = expires;
              info.session = session != FALSE || !(expires > 0);
              infos.push_back(std::move(info));
              handles.push_back(std::move(cookie));
            }

            const CookiePlan plan = PlanCookieEvictions(infos, config);
            size_t deleted = 0;
            std::string note = problem.empty() ? "ok" : problem;
            if (config.dryRun) {
              if (!plan.evict.empty()) note = "dry-run";
            } else {
              for (const size_t index : plan.evict) {
                if (deleted >= kMaxEvictPerRound) {
                  note = "capped";
                  break;
                }
                if (index < handles.size() && SUCCEEDED(manager->DeleteCookie(handles[index].Get()))) ++deleted;
              }
            }
            cookieEvictedTotal_ += deleted;
            WriteCookieGuardStatus(config, plan, infos.size(), deleted, note);
            return S_OK;
          })
          .Get());
  if (FAILED(started)) {
    cookieGuardRunning_ = false;
    char failure[64];
    sprintf_s(failure, "get-cookies-call-failed 0x%08X", static_cast<unsigned>(started));
    WriteCookieGuardStatus(config, CookiePlan{}, 0, 0, failure);
  }
}

void BrowserWindow::WriteCookieGuardStatus(const CookieGuardConfig& config, const CookiePlan& plan, size_t total,
                                           size_t deleted, const std::string& note) {
  if (destroyed_ || profileDir_.empty()) return;
  const long long now = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count();
  std::vector<CookieSiteReport> sites = plan.sites;
  std::sort(sites.begin(), sites.end(), [](const CookieSiteReport& a, const CookieSiteReport& b) {
    return a.count != b.count ? a.count > b.count : a.site < b.site;
  });
  if (sites.size() > kStatusSites) sites.resize(kStatusSites);

  std::string out = "{\"version\":";
  json::AppendQuoted(out, JISU_VERSION);
  out += ",\"updatedAt\":" + std::to_string(now);
  out += ",\"enabled\":" + std::string(config.enabled ? "true" : "false");
  out += ",\"dryRun\":" + std::string(config.dryRun ? "true" : "false");
  out += ",\"maxPerSite\":" + std::to_string(config.maxPerSite);
  out += ",\"keepUnits\":" + std::to_string(config.keepUnits);
  out += ",\"intervalSeconds\":" + std::to_string(config.intervalSeconds);
  out += ",\"cookies\":" + std::to_string(total);
  out += ",\"planned\":" + std::to_string(plan.evict.size());
  out += ",\"evicted\":" + std::to_string(deleted);
  out += ",\"evictedTotal\":" + std::to_string(cookieEvictedTotal_);
  out += ",\"note\":";
  json::AppendQuoted(out, note);
  out += ",\"sites\":[";
  for (size_t i = 0; i < sites.size(); ++i) {
    if (i) out += ',';
    out += "{\"site\":";
    json::AppendQuoted(out, sites[i].site);
    out += ",\"count\":" + std::to_string(sites[i].count);
    out += ",\"units\":" + std::to_string(sites[i].units);
    out += ",\"evictUnits\":" + std::to_string(sites[i].evictUnits);
    out += ",\"evictCookies\":" + std::to_string(sites[i].evictCookies);
    out += '}';
  }
  out += "]}";
  profile_->writer.Write(profileDir_ + L"\\cookie-guard-status.json", out, false);  // 状态文件：不等落盘
}

}  // namespace jisu
