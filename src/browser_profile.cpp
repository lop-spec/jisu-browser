#include "browser_window.h"
#include "json.h"
#include "metrics.h"
#include "tab_policy.h"
#include <algorithm>
using Microsoft::WRL::Callback;

namespace jisu {
void BrowserWindow::ConfigureProfile(Tab& tab) {
  if (profileConfigured_) return;
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
  std::string text;
  json::Value config;
  if (!ReadFileBytes(profileDir_ + L"\\extensions.json", text) || !json::Parse(text, config)) return;
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
    entry.nativeTabLimit = item["sourceId"].AsString() == "auto-close-old-tabs";
    extensions_.push_back(std::move(entry));
  }
  if (FAILED(profile.As(&extensionProfile_))) { SaveExtensionStatus(); return; }
  InstallNextExtension();
}
void BrowserWindow::InstallNextExtension() {
  if (destroyed_ || !extensionProfile_) return;
  if (extensionIndex_ >= extensions_.size()) { SaveExtensionStatus(); PruneUnlistedExtensions(); return; }
  const size_t index = extensionIndex_++;
  const std::weak_ptr<int> alive = alive_;
  const HRESULT started = extensionProfile_->AddBrowserExtension(extensions_[index].path.c_str(),
    Callback<ICoreWebView2ProfileAddBrowserExtensionCompletedHandler>([this,index,alive](HRESULT hr, ICoreWebView2BrowserExtension* extension) -> HRESULT {
      if (alive.expired()) return S_OK;
      auto& e = extensions_[index]; e.result = hr;
      if (SUCCEEDED(hr) && extension) {
        e.handle = extension;
        CoTaskString id; extension->get_Id(id.Put()); e.id = id.Str();
        const HRESULT enable = extension->Enable(e.enabled ? TRUE : FALSE,
          Callback<ICoreWebView2BrowserExtensionEnableCompletedHandler>([this,index,alive](HRESULT result) -> HRESULT {
            if (!alive.expired()) { extensions_[index].result = result; SaveExtensionStatus(); InstallNextExtension(); }
            return S_OK;
          }).Get());
        if (SUCCEEDED(enable)) return S_OK;
        e.result = enable;
      }
      SaveExtensionStatus(); InstallNextExtension(); return S_OK;
    }).Get());
  if (FAILED(started)) { extensions_[index].result = started; SaveExtensionStatus(); InstallNextExtension(); }
}
void BrowserWindow::SaveExtensionStatus() {
  if (destroyed_ || profileDir_.empty()) return;
  std::string out = "{\"extensions\":[";
  bool first = true;
  for (const auto& e : extensions_) {
    if (!first) out += ','; first = false;
    out += "{\"name\":"; json::AppendQuoted(out, ToUtf8(e.name));
    out += ",\"id\":"; json::AppendQuoted(out, ToUtf8(e.id));
    out += ",\"nativeTabLimit\":" + std::string(e.nativeTabLimit ? "true" : "false");
    out += ",\"enabled\":" + std::string(e.enabled ? "true" : "false");
    out += ",\"loaded\":" + std::string(SUCCEEDED(e.result) && !e.id.empty() ? "true" : "false");
    out += ",\"hresult\":" + std::to_string(static_cast<uint32_t>(e.result));
    out += ",\"popup\":"; json::AppendQuoted(out, ToUtf8(e.popup));
    out += ",\"options\":"; json::AppendQuoted(out, ToUtf8(e.options));
    out += '}';
  }
  out += "]}";
  WriteFileAtomic(profileDir_ + L"\\extensions-status.json", out);
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
    WriteFileAtomic(profileDir_ + L"\\extensions-cleanup.json", "{\"completed\":true,\"removed\":" + std::to_string(removedExtensionCount_) + "}");
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
    candidates.push_back({tab.id, i == active_, tab.agent || tab.leased, tab.url.starts_with(L"chrome-extension://")});
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
