#include "browser_window.h"
#include "json.h"
#include "metrics.h"
#include "tab_policy.h"
#include <algorithm>
using Microsoft::WRL::Callback;

namespace jisu {
void BrowserWindow::ConfigureProfile(Tab& tab) {
  if (profileConfigured_ || options_.appMode) return; // Daily browser alone manages shared profile settings/extensions.
  ComPtr<ICoreWebView2_13> view;
  ComPtr<ICoreWebView2Profile> profile;
  if (FAILED(tab.webview.As(&view)) || FAILED(view->get_Profile(profile.GetAddressOf()))) return;
  profileConfigured_ = true;
  ComPtr<ICoreWebView2Profile6> autofill;
  if (SUCCEEDED(profile.As(&autofill))) {
    autofill->put_IsPasswordAutosaveEnabled(TRUE);
    autofill->put_IsGeneralAutofillEnabled(TRUE);
  }
  if (!options_.metricsPath.empty() || options_.preheatSeconds > 0) return;
  // extensions.json 只读一次：内核退出重建后在新内核里重装同一批扩展，不重复加条目
  if (!extensionsConfigLoaded_) {
    std::string text;
    json::Value config;
    if (!ReadFileBytes(profileDir_ + L"\\extensions.json", text) || !json::Parse(text, config)) return;
    extensionsConfigLoaded_ = true;
    removeUnlistedExtensions_ = config["removeUnlisted"].AsBool(false);
    for (const auto& item : config["extensions"].Items()) {
      const std::wstring path = FromUtf8(item["path"].AsString());
      // Only immutable, machine-local snapshots under this profile are accepted.
      const std::wstring prefix = profileDir_ + L"\\Extensions\\";
      if (path.size() <= prefix.size() || _wcsnicmp(path.c_str(), prefix.c_str(), prefix.size()) != 0 ||
          path.find(L"..") != std::wstring::npos || extensions_.size() >= 100) continue;
      ExtensionEntry entry;
      entry.path = path; entry.name = FromUtf8(item["name"].AsString());
      entry.popup = FromUtf8(item["popup"].AsString()); entry.options = FromUtf8(item["options"].AsString());
      entry.enabled = item["enabled"].AsBool();
      entry.pinned = item["pinned"].AsBool(true);
      entry.nativeTabLimit = item["sourceId"].AsString() == "auto-close-old-tabs";
      extensions_.push_back(std::move(entry));
    }
  }
  if (FAILED(profile.As(&extensionProfile_))) { SaveExtensionStatus(); return; }
  // 真正开始装要等首个页面加载完（StartExtensions），不和首屏抢
}
void BrowserWindow::InstallNextExtension() {
  if (destroyed_ || !extensionProfile_) return;
  if (extensionIndex_ >= extensions_.size()) {
    SaveExtensionStatus();
    PruneUnlistedExtensions();
    PostTask([this] { EnforceTabLimit(); });  // 「自动关旧标签」刚生效：恢复的会话、启动参数开的标签按上限收一次
    return;
  }
  const size_t index = extensionIndex_++;
  const std::weak_ptr<int> alive = alive_;
  const std::wstring path = extensions_[index].path;
  const HRESULT started = extensionProfile_->AddBrowserExtension(path.c_str(),
    Callback<ICoreWebView2ProfileAddBrowserExtensionCompletedHandler>([this,path,alive](HRESULT hr, ICoreWebView2BrowserExtension* extension) -> HRESULT {
      if (alive.expired()) return S_OK;
      // 回调到达前列表可能变了：按路径找回这一项
      const auto e = std::find_if(extensions_.begin(), extensions_.end(), [&path](const ExtensionEntry& item) { return item.path == path; });
      if (e == extensions_.end()) { InstallNextExtension(); return S_OK; }
      e->result = hr;
      if (SUCCEEDED(hr) && extension) {
        e->handle = extension;
        CoTaskString id; extension->get_Id(id.Put()); e->id = id.Str();
        const HRESULT enable = extension->Enable(e->enabled ? TRUE : FALSE,
          Callback<ICoreWebView2BrowserExtensionEnableCompletedHandler>([this,path,alive](HRESULT result) -> HRESULT {
            if (alive.expired()) return S_OK;
            const auto item = std::find_if(extensions_.begin(), extensions_.end(), [&path](const ExtensionEntry& x) { return x.path == path; });
            if (item != extensions_.end()) item->result = result;
            SaveExtensionStatus(); InstallNextExtension();
            return S_OK;
          }).Get());
        if (SUCCEEDED(enable)) return S_OK;
        e->result = enable;
      }
      SaveExtensionStatus(); InstallNextExtension(); return S_OK;
    }).Get());
  if (FAILED(started)) { extensions_[index].result = started; SaveExtensionStatus(); InstallNextExtension(); }
}
// 同一轮消息里多次调用（装扩展时每个回调都会调）只写一次文件、刷新一次工具栏
void BrowserWindow::SaveExtensionStatus() {
  if (destroyed_ || profileDir_.empty() || extensionStatusQueued_) return;
  extensionStatusQueued_ = true;
  PostTask([this] {
    extensionStatusQueued_ = false;
    WriteExtensionStatus();
  });
}
void BrowserWindow::WriteExtensionStatus() {
  if (destroyed_ || profileDir_.empty()) return;
  std::string out = "{\"extensions\":[";
  bool first = true;
  for (const auto& e : extensions_) {
    if (!first) out += ','; first = false;
    out += "{\"name\":"; json::AppendQuoted(out, ToUtf8(e.name));
    out += ",\"id\":"; json::AppendQuoted(out, ToUtf8(e.id));
    out += ",\"nativeTabLimit\":" + std::string(e.nativeTabLimit ? "true" : "false");
    out += ",\"pinned\":" + std::string(e.pinned ? "true" : "false");
    out += ",\"enabled\":" + std::string(e.enabled ? "true" : "false");
    out += ",\"loaded\":" + std::string(SUCCEEDED(e.result) && !e.id.empty() ? "true" : "false");
    out += ",\"hresult\":" + std::to_string(static_cast<uint32_t>(e.result));
    out += ",\"popup\":"; json::AppendQuoted(out, ToUtf8(e.popup));
    out += ",\"options\":"; json::AppendQuoted(out, ToUtf8(e.options));
    out += '}';
  }
  out += "]}";
  WriteFileAtomic(profileDir_ + L"\\extensions-status.json", out, false);  // 状态文件：不等落盘
  RefreshExtensionToolbar();
  for (auto& tab : tabs_) if (tab->url.starts_with(L"https://newtab.jisu.example/extensions.html")) PostToPage(*tab, "{\"type\":\"ext.list\",\"data\":" + out + "}");
}
void BrowserWindow::PruneUnlistedExtensions() {
  if (!removeUnlistedExtensions_ || extensionPruneStarted_ || destroyed_) return;
  extensionPruneStarted_ = true;
  std::weak_ptr<int> alive = alive_;
  extensionProfile_->GetBrowserExtensions(Callback<ICoreWebView2ProfileGetBrowserExtensionsCompletedHandler>(
    [this,alive](HRESULT hr, ICoreWebView2BrowserExtensionList* list) -> HRESULT {
      if (alive.expired() || FAILED(hr) || !list) return S_OK;
      UINT32 count = 0; list->get_Count(&count);
      for (UINT32 i = 0; i < count; ++i) {
        ComPtr<ICoreWebView2BrowserExtension> extension;
        if (FAILED(list->GetValueAtIndex(i, extension.GetAddressOf()))) continue;
        CoTaskString id; extension->get_Id(id.Put());
        const std::wstring value = id.Str();
        const bool approved = std::any_of(extensions_.begin(), extensions_.end(), [&value](const auto& e) { return !value.empty() && e.id == value; });
        if (!approved) unlistedExtensions_.push_back(extension);
      }
      RemoveNextUnlistedExtension(); return S_OK;
    }).Get());
}
void BrowserWindow::RemoveNextUnlistedExtension() {
  if (destroyed_) return;
  if (unlistedExtensions_.empty()) {
    WriteFileAtomic(profileDir_ + L"\\extensions-cleanup.json", "{\"completed\":true,\"removed\":" + std::to_string(removedExtensionCount_) + "}", false);
    return;
  }
  const auto extension = unlistedExtensions_.back();
  unlistedExtensions_.pop_back();
  std::weak_ptr<int> alive = alive_;
  extension->Remove(Callback<ICoreWebView2BrowserExtensionRemoveCompletedHandler>(
    [this,alive](HRESULT hr) -> HRESULT {
      if (!alive.expired()) {
        if (SUCCEEDED(hr)) ++removedExtensionCount_;
        RemoveNextUnlistedExtension();
      }
      return S_OK;
    }).Get());
}
void BrowserWindow::EnforceTabLimit() {
  if (destroyed_ || !std::any_of(extensions_.begin(), extensions_.end(), [](const auto& e) { return e.nativeTabLimit && e.enabled && SUCCEEDED(e.result); })) return;
  std::vector<TabPolicyEntry> candidates;
  for (size_t i = 0; i < tabs_.size(); ++i) {
    const auto& tab = *tabs_[i];
    // 弹出页落地前不关它的打开者：交接中关会让内核崩，交接后立刻关会丢掉弹出页的首次导航；落地后会再收一次
    const bool opener = std::any_of(tabs_.begin(), tabs_.end(), [&](const auto& t) { return t->openerId == tab.id; });
    // 固定的标签、智能体页面弹出的标签不计数也不关：用户的标签不会因为 Pi 点开的链接被挤掉
    const bool privileged = tab.url.starts_with(L"chrome-extension://") || tab.pinned || tab.fromAgent;
    candidates.push_back({tab.id, i == active_, tab.agent || tab.leased || tab.media != nullptr || tab.mediaTarget != 0 || opener, privileged});
  }
  for (const uint32_t id : OldTabsToClose(candidates, static_cast<size_t>(settings_.maxRegularTabs))) {
    if (const Tab* tab = FindTab(id)) CloseTab(IndexOf(*tab));
  }
}
bool BrowserWindow::HandleTabLimitMessage(Tab& tab, const std::wstring& source, const std::string& text) {
  const bool trusted = std::any_of(extensions_.begin(), extensions_.end(), [&source](const auto& e) {
    return e.nativeTabLimit && e.enabled && !e.id.empty() && source.starts_with(L"chrome-extension://" + e.id + L"/");
  });
  if (!trusted) return false;
  json::Value message;
  if (!json::Parse(text, message)) return true;
  const std::string type = message["type"].AsString();
  if (type != "oldtabs.get" && type != "oldtabs.set") return true;
  if (type == "oldtabs.set") {
    const int limit = static_cast<int>(message["maxTabs"].AsNumber(0));
    if (limit < 1 || limit > 99) return true;
    settings_.maxRegularTabs = limit; SaveSettings(); EnforceTabLimit();
  }
  PostToPage(tab, "{\"type\":\"oldtabs.settings\",\"maxTabs\":" + std::to_string(settings_.maxRegularTabs) + "}");
  return true;
}
void BrowserWindow::OpenExtensions() {
  for (size_t i = 0; i < tabs_.size(); ++i) if (tabs_[i]->url.starts_with(L"https://newtab.jisu.example/extensions.html")) { Activate(i); return; }
  NewTab(L"https://newtab.jisu.example/extensions.html");
}
}  // namespace jisu
