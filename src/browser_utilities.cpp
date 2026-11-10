#include "browser_window.h"
#include "browser_url.h"
#include "json.h"
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <algorithm>
#include <chrono>

using Microsoft::WRL::Callback;
namespace jisu {
namespace {
constexpr wchar_t kDownloads[] = L"https://newtab.jisu.example/downloads.html";
constexpr wchar_t kSettings[] = L"https://newtab.jisu.example/settings.html";
std::wstring DefaultDownloads() {
  PWSTR path = nullptr;
  if (FAILED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &path))) return {};
  std::wstring result(path); CoTaskMemFree(path); return result;
}
std::wstring FileName(const std::wstring& path) {
  const auto at = path.find_last_of(L"/\\");
  auto result = path.substr(at == std::wstring::npos ? 0 : at + 1);
  for (auto& c : result) if (c < 32 || std::wstring(L"<>:\"/\\|?*").find(c) != std::wstring::npos) c = L'_';
  while (!result.empty() && (result.back() == L'.' || result.back() == L' ')) result.pop_back();
  if (result.empty()) result = L"download";
  return TruncateUtf16(result, 180);
}
}
uint64_t BrowserWindow::UseStamp() {
  static uint64_t previous = 0;
  const auto now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count());
  return previous = std::max(previous + 1, now);
}
void BrowserWindow::WatchControllers() {
  if (destroyed_ || options_.appMode) return;
  const auto now = GetTickCount64();
  if (!environment_ && environmentStarted_ && now - environmentStarted_ >= 15000) {
    environmentStarted_ = 0; ++envGeneration_;
    message_ = L"内核连接超时，正在重新连接…";
    ScheduleEnvironmentRecovery(); InvalidateChrome(); return;
  }
  std::vector<uint32_t> overdue;
  for (auto* list : {&tabs_, &agentTabs_}) for (const auto& t : *list)
    if (t->creating && now - t->creatingAt >= 15000) overdue.push_back(t->id);
  if (spare_ && spare_->creating && now - spare_->creatingAt >= 15000) overdue.push_back(spare_->id);
  for (auto id : overdue) if (Tab* t = FindTab(id)) {
    ++t->createAttempt; t->creating = false;
    OnControllerFailed(*t, HRESULT_FROM_WIN32(ERROR_TIMEOUT));
  }
  if (profile_->writer.Failed() && message_.empty()) {
    message_ = L"资料保存失败，请检查磁盘空间与资料目录权限。"; InvalidateChrome();
  }
}
bool BrowserWindow::TabDownloading(uint32_t id) const {
  return std::any_of(profile_->downloads.begin(), profile_->downloads.end(), [id](const auto& d) {
    return d->tabId == id && d->operation && (d->state == COREWEBVIEW2_DOWNLOAD_STATE_IN_PROGRESS || d->paused);
  });
}
void BrowserWindow::LoadDownloads() {
  if (profile_->downloadsLoaded) return;
  profile_->downloadsLoaded = true;
  std::string bytes; json::Value root;
  if (!ReadFileBytes(profileDir_ + L"\\Downloads.json", bytes) || !json::Parse(bytes, root)) return;
  for (const auto& row : root["rows"].Items()) {
    if (profile_->downloads.size() >= 500) break;
    auto d = std::make_shared<Download>(); d->id = profile_->nextDownload++;
    d->url = HistoryStore::SafeUrl(FromUtf8(row["url"].AsString()));
    d->path = FromUtf8(row["path"].AsString());
    if (!IsLocalPath(d->path) || d->path.size() > 32000) continue;
    d->received = static_cast<int64_t>(row["received"].AsNumber());
    d->total = static_cast<int64_t>(row["total"].AsNumber(-1));
    d->state = row["state"].AsNumber() == COREWEBVIEW2_DOWNLOAD_STATE_COMPLETED ? COREWEBVIEW2_DOWNLOAD_STATE_COMPLETED : COREWEBVIEW2_DOWNLOAD_STATE_INTERRUPTED;
    profile_->downloads.push_back(std::move(d));
  }
}
std::string BrowserWindow::DownloadsJson() const {
  auto root = json::Value::MakeObject(), rows = json::Value::MakeArray();
  root.Set("type", json::Value::String("downloads.rows"));
  for (const auto& d : profile_->downloads) {
    auto row = json::Value::MakeObject();
    row.Set("id", json::Value::Number(static_cast<double>(d->id)));
    row.Set("url", json::Value::String(ToUtf8(d->url)));
    row.Set("path", json::Value::String(ToUtf8(d->path)));
    row.Set("received", json::Value::Number(static_cast<double>(d->received)));
    row.Set("total", json::Value::Number(static_cast<double>(d->total)));
    row.Set("state", json::Value::Number(d->state));
    row.Set("paused", json::Value::Bool(d->paused));
    row.Set("live", json::Value::Bool(d->operation != nullptr));
    const auto elapsed = d->started ? GetTickCount64() - d->started : 0;
    row.Set("speed", json::Value::Number(elapsed && d->state == 0 && !d->paused ? d->received * 1000.0 / elapsed : 0));
    rows.Push(std::move(row));
  }
  root.Set("rows", std::move(rows)); return json::Serialize(root);
}
void BrowserWindow::SaveDownloads() {
  profile_->downloadsSaved = GetTickCount64();
  profile_->writer.Write(profileDir_ + L"\\Downloads.json", DownloadsJson(), false);
}
void BrowserWindow::ConfigureDownloads(Tab& tab) {
  ComPtr<ICoreWebView2_4> view;
  if (FAILED(tab.webview.As(&view))) return;
  const auto owner = tab.owner; const auto id = tab.id;
  EventRegistrationToken token{};
  view->add_DownloadStarting(Callback<ICoreWebView2DownloadStartingEventHandler>(
      [owner, id](ICoreWebView2*, ICoreWebView2DownloadStartingEventArgs* args) -> HRESULT {
    auto* self = *owner;
    const Tab* source = self && !self->destroyed_ ? self->FindTab(id) : nullptr;
    // Invisible automation must not silently create arbitrary files. Human handoff must adopt the tab first.
    if (!source || source->agent || source->fromAgent) { args->put_Cancel(TRUE); args->put_Handled(TRUE); return S_OK; }
    self->LoadDownloads();
    ComPtr<ICoreWebView2Deferral> deferral;
    if (FAILED(args->GetDeferral(deferral.GetAddressOf()))) { args->put_Cancel(TRUE); return S_OK; }
    const ComPtr<ICoreWebView2DownloadStartingEventArgs> retained = args;
    const auto alive = std::weak_ptr<int>(self->alive_);
    self->PostTask([self, alive, retained, deferral, id] {
      if (alive.expired()) { retained->put_Cancel(TRUE); deferral->Complete(); return; }
      auto d = std::make_shared<Download>();
      retained->get_DownloadOperation(d->operation.GetAddressOf());
      if (!d->operation) { retained->put_Cancel(TRUE); deferral->Complete(); return; }
      CoTaskString path, uri;
      retained->get_ResultFilePath(path.Put()); d->operation->get_Uri(uri.Put());
      const auto directory = self->settings_.downloadDirectory.empty() ? DefaultDownloads() : self->settings_.downloadDirectory;
      if (directory.empty() || !EnsureDirectory(directory)) {
        retained->put_Cancel(TRUE); deferral->Complete(); self->ShowNotice(L"下载目录不可写，请在设置中更换目录。"); return;
      }
      d->path = directory + L"\\" + FileName(path.Str());
      if (self->settings_.askDownload) {
        std::vector<wchar_t> file(32768, 0); wcscpy_s(file.data(), file.size(), d->path.c_str());
        OPENFILENAMEW dialog{}; dialog.lStructSize = sizeof(dialog); dialog.hwndOwner = self->hwnd_;
        dialog.lpstrFile = file.data(); dialog.nMaxFile = static_cast<DWORD>(file.size());
        dialog.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST;
        if (!GetSaveFileNameW(&dialog) || self->destroyed_) { retained->put_Cancel(TRUE); deferral->Complete(); return; }
        d->path = file.data();
      } else {
        const auto original = d->path; size_t suffix = 1;
        while (GetFileAttributesW(d->path.c_str()) != INVALID_FILE_ATTRIBUTES && suffix < 10000) {
          const auto dot = original.find_last_of(L'.'), slash = original.find_last_of(L'\\');
          const auto split = dot != std::wstring::npos && dot > slash ? dot : original.size();
          d->path = original.substr(0, split) + L" (" + std::to_wstring(suffix++) + L")" + original.substr(split);
        }
        if (suffix >= 10000) { retained->put_Cancel(TRUE); deferral->Complete(); return; }
      }
      retained->put_Handled(TRUE);
      if (FAILED(retained->put_ResultFilePath(d->path.c_str()))) { retained->put_Cancel(TRUE); deferral->Complete(); return; }
      d->id = self->profile_->nextDownload++; d->tabId = id; d->started = GetTickCount64();
      d->url = HistoryStore::SafeUrl(uri.Str());
      // Do not turn a sanitized signed/token URL into a different retry request.
      if (d->url != uri.Str()) d->url.clear();
      d->operation->get_TotalBytesToReceive(&d->total);
      self->profile_->downloads.insert(self->profile_->downloads.begin(), d);
      if (self->profile_->downloads.size() > 500) {
        auto& rows = self->profile_->downloads;
        auto last = std::find_if(rows.rbegin(), rows.rend(), [](const auto& row) { return row->state != 0; });
        if (last != rows.rend()) rows.erase(std::next(last).base());
      }
      const auto weak = std::weak_ptr<Download>(d);
      const auto* source = self->FindTab(id);
      if (!source) { retained->put_Cancel(TRUE); deferral->Complete(); return; }
      auto update = [owner = source->owner, weak](ICoreWebView2DownloadOperation* operation, IUnknown*) -> HRESULT {
        auto* current = *owner; const auto item = weak.lock();
        if (!current || current->destroyed_ || !item) return S_OK;
        COREWEBVIEW2_DOWNLOAD_STATE state; operation->get_State(&state); item->state = static_cast<int>(state);
        operation->get_BytesReceived(&item->received); operation->get_TotalBytesToReceive(&item->total);
        COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON reason;
        if (SUCCEEDED(operation->get_InterruptReason(&reason))) item->paused = reason == COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_USER_PAUSED;
        if (item->state != 0 || GetTickCount64() - current->profile_->downloadsSaved > 1000) current->SaveDownloads();
        return S_OK;
      };
      EventRegistrationToken event{};
      d->operation->add_BytesReceivedChanged(Callback<ICoreWebView2BytesReceivedChangedEventHandler>(update).Get(), &event);
      d->operation->add_StateChanged(Callback<ICoreWebView2StateChangedEventHandler>(update).Get(), &event);
      self->SaveDownloads(); deferral->Complete();
    });
    return S_OK;
  }).Get(), &token);
}
void BrowserWindow::OpenDownloads() {
  LoadDownloads();
  for (size_t i = 0; i < tabs_.size(); ++i) if (tabs_[i]->url == kDownloads) { Activate(i); return; }
  NewTab(kDownloads);
}
void BrowserWindow::OpenSettings() {
  for (size_t i = 0; i < tabs_.size(); ++i) if (tabs_[i]->url == kSettings) { Activate(i); return; }
  NewTab(kSettings);
}
bool BrowserWindow::HandleUtilityMessage(Tab& tab, const std::string& text) {
  if (tab.url != kDownloads && tab.url != kSettings) return false;
  json::Value message; if (!json::Parse(text, message)) return true;
  const auto type = message["type"].AsString();
  if (tab.url == kDownloads && type.starts_with("downloads.")) {
    LoadDownloads();
    const auto id = static_cast<uint64_t>(std::max(0.0, message["id"].AsNumber()));
    for (auto& d : profile_->downloads) if (d->id == id) {
      if (type == "downloads.cancel" && d->operation) { d->paused = false; d->operation->Cancel(); }
      if (type == "downloads.pause" && d->operation && SUCCEEDED(d->operation->Pause())) d->paused = true;
      if (type == "downloads.resume" && d->operation) {
        BOOL can = FALSE; d->operation->get_CanResume(&can);
        if (can && SUCCEEDED(d->operation->Resume())) d->paused = false;
      }
      if (type == "downloads.retry" && !d->url.empty() && d->state != 0) { NewTab(d->url); break; }
      if (type == "downloads.open" && d->state == COREWEBVIEW2_DOWNLOAD_STATE_COMPLETED)
        ShellExecuteW(hwnd_, L"open", d->path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
      if (type == "downloads.folder") {
        const auto args = L"/select,\"" + d->path + L"\"";
        ShellExecuteW(hwnd_, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
      }
      break;
    }
    PostToPage(tab, DownloadsJson()); return true;
  }
  if (tab.url != kSettings) return true;
  if (type == "settings.save") {
    settings_.continueSession = message["continueSession"].AsBool();
    settings_.askDownload = message["askDownload"].AsBool();
    const auto engine = FromUtf8(message["searchEngine"].AsString());
    if (engine == L"bing" || engine == L"google" || engine == L"baidu") settings_.searchEngine = engine;
    const auto dir = FromUtf8(message["downloadDirectory"].AsString());
    if (dir.empty() || (IsLocalPath(dir) && dir.size() < 1000)) settings_.downloadDirectory = dir;
    settings_.maxRegularTabs = static_cast<int>(std::clamp(message["maxRegularTabs"].AsNumber(10), 1.0, 99.0));
    SaveSettings(); EnforceTabLimit();
  } else if (type == "settings.popups.clear") { settings_.popupAllow.clear(); SaveSettings(); }
  else if (type == "settings.clear") {
    ComPtr<ICoreWebView2_13> view; ComPtr<ICoreWebView2Profile> profile; ComPtr<ICoreWebView2Profile2> data;
    if (tab.webview && SUCCEEDED(tab.webview.As(&view)) && SUCCEEDED(view->get_Profile(profile.GetAddressOf())) && SUCCEEDED(profile.As(&data))) {
      const auto kinds = message["cookies"].AsBool() ? COREWEBVIEW2_BROWSING_DATA_KINDS_ALL_PROFILE :
          static_cast<COREWEBVIEW2_BROWSING_DATA_KINDS>(COREWEBVIEW2_BROWSING_DATA_KINDS_DISK_CACHE | COREWEBVIEW2_BROWSING_DATA_KINDS_BROWSING_HISTORY);
      data->ClearBrowsingData(kinds, Callback<ICoreWebView2ClearBrowsingDataCompletedHandler>([alive = std::weak_ptr<int>(alive_), this](HRESULT hr) -> HRESULT {
        if (!alive.expired()) ShowNotice(SUCCEEDED(hr) ? L"浏览数据已清除。" : L"清除失败，请稍后重试。"); return S_OK;
      }).Get());
      history_.Start(profileDir_ + L"\\History.json"); history_.Clear();
    }
  }
  auto out = json::Value::MakeObject(); out.Set("type", json::Value::String("settings.data"));
  out.Set("continueSession", json::Value::Bool(settings_.continueSession));
  out.Set("askDownload", json::Value::Bool(settings_.askDownload));
  out.Set("downloadDirectory", json::Value::String(ToUtf8(settings_.downloadDirectory.empty() ? DefaultDownloads() : settings_.downloadDirectory)));
  out.Set("searchEngine", json::Value::String(ToUtf8(settings_.searchEngine)));
  out.Set("maxRegularTabs", json::Value::Number(settings_.maxRegularTabs));
  out.Set("popupCount", json::Value::Number(static_cast<double>(settings_.popupAllow.size())));
  PostToPage(tab, json::Serialize(out)); return true;
}
void BrowserWindow::PageAction(UINT action) {
  if (action == 0) {
    Options options;
    options.userDataFolder = profileDir_; options.defaultUserData = options_.defaultUserData;
    options.agentConfigPath = options_.agentConfigPath;
    options.browserArgs = options_.browserArgs; options.trackingPrevention = options_.trackingPrevention;
    auto window = std::make_unique<BrowserWindow>(options, warmup_, root_);
    window->environment_ = environment_;
    if (!window->Create(instance_)) return;
    auto* target = window.get(); root_->detachedWindows_.push_back(std::move(window));
    target->NewTab(L""); target->ShowFirstFrame(SW_SHOWNORMAL); return;
  }
  auto* tab = ActiveTab(); if (!tab || !tab->webview) return;
  HRESULT hr = E_NOINTERFACE;
  if (action == 1) {
    ComPtr<ICoreWebView2_28> view; ComPtr<ICoreWebView2Find> find;
    ComPtr<ICoreWebView2Environment15> env; ComPtr<ICoreWebView2FindOptions> options;
    if (SUCCEEDED(tab->webview.As(&view)) && SUCCEEDED(view->get_Find(find.GetAddressOf())) &&
        SUCCEEDED(environment_.As(&env)) && SUCCEEDED(env->CreateFindOptions(options.GetAddressOf()))) {
      options->put_FindTerm(L""); options->put_SuppressDefaultFindDialog(FALSE);
      hr = find->Start(options.Get(), Callback<ICoreWebView2FindStartCompletedHandler>([](HRESULT) -> HRESULT { return S_OK; }).Get());
    }
  } else if (action >= 2 && action <= 4 && tab->controller) {
    double zoom = 1; tab->controller->get_ZoomFactor(&zoom);
    hr = tab->controller->put_ZoomFactor(action == 4 ? 1 : std::clamp(zoom + (action == 2 ? 0.1 : -0.1), 0.25, 5.0));
  } else if (action == 5) {
    ComPtr<ICoreWebView2_16> view;
    if (SUCCEEDED(tab->webview.As(&view))) hr = view->ShowPrintUI(COREWEBVIEW2_PRINT_DIALOG_KIND_BROWSER);
  } else if (action == 6) hr = tab->webview->OpenDevToolsWindow();
  else if (action == 7) {
    ComPtr<ICoreWebView2_6> view;
    if (SUCCEEDED(tab->webview.As(&view))) hr = view->OpenTaskManagerWindow();
  }
  if (FAILED(hr)) ShowNotice(L"当前 WebView2 运行时不支持此操作，或页面暂未准备好。");
}
}  // namespace jisu
