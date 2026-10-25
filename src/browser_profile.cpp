#include "browser_window.h"
#include "json.h"
#include "metrics.h"
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
    extensions_.push_back(std::move(entry));
  }
  if (FAILED(profile.As(&extensionProfile_))) { SaveExtensionStatus(); return; }
  InstallNextExtension();
}
void BrowserWindow::InstallNextExtension() {
  if (destroyed_ || extensionIndex_ >= extensions_.size() || !extensionProfile_) { SaveExtensionStatus(); return; }
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
void BrowserWindow::OpenExtensions() {
  for (size_t i = 0; i < tabs_.size(); ++i) if (tabs_[i]->url.starts_with(L"https://newtab.jisu.example/extensions.html")) { Activate(i); return; }
  NewTab(L"https://newtab.jisu.example/extensions.html");
}
}  // namespace jisu
