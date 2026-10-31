// 标签的右键菜单、固定与静音、关闭的标签重开、会话恢复、内核退出后的重建。
#include "browser_url.h"
#include "browser_window.h"
#include "json.h"

#include <algorithm>
#include <iterator>

using Microsoft::WRL::Callback;

namespace jisu {
namespace {

constexpr UINT kMenuTabNew = 7001;
constexpr UINT kMenuTabReload = 7002;
constexpr UINT kMenuTabDuplicate = 7003;
constexpr UINT kMenuTabPin = 7004;
constexpr UINT kMenuTabMute = 7005;
constexpr UINT kMenuTabClose = 7006;
constexpr UINT kMenuTabCloseOthers = 7007;
constexpr UINT kMenuTabCloseRight = 7008;
constexpr UINT kMenuTabReopen = 7009;
constexpr size_t kClosedTabLimit = 25;

std::wstring HtmlEscape(const std::wstring& text) {
  std::wstring out;
  for (const wchar_t c : text) {
    switch (c) {
      case L'&': out += L"&amp;"; break;
      case L'<': out += L"&lt;"; break;
      case L'>': out += L"&gt;"; break;
      case L'"': out += L"&quot;"; break;
      default: out.push_back(c);
    }
  }
  return out;
}

}  // namespace

void BrowserWindow::ShowTabContextMenu(size_t index, POINT screen) {
  if (index >= tabs_.size()) return;
  const Tab& tab = *tabs_[index];
  const uint32_t id = tab.id;
  const bool agentBusy = tab.agent && tab.leased;
  HMENU menu = CreatePopupMenu();
  AppendMenuW(menu, MF_STRING, kMenuTabNew, L"在右侧新建标签页");
  AppendMenuW(menu, MF_STRING, kMenuTabReload, L"重新加载\tF5");
  AppendMenuW(menu, MF_STRING | (IsWebUrl(tab.url) && !agentBusy ? 0 : MF_GRAYED), kMenuTabDuplicate, L"复制标签页");
  AppendMenuW(menu, MF_STRING | (tab.agent ? MF_GRAYED : 0), kMenuTabPin, tab.pinned ? L"取消固定" : L"固定（不被自动关闭）");
  AppendMenuW(menu, MF_STRING | (tab.webview ? 0 : MF_GRAYED), kMenuTabMute, tab.muted ? L"取消静音" : L"静音此标签页");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, kMenuTabClose, L"关闭\tCtrl+W");
  AppendMenuW(menu, MF_STRING | (tabs_.size() > 1 ? 0 : MF_GRAYED), kMenuTabCloseOthers, L"关闭其他标签页");
  AppendMenuW(menu, MF_STRING | (index + 1 < tabs_.size() ? 0 : MF_GRAYED), kMenuTabCloseRight, L"关闭右侧标签页");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING | (profile_->closedTabs.empty() ? MF_GRAYED : 0), kMenuTabReopen,
              L"重新打开关闭的标签页\tCtrl+Shift+T");
  const UINT command = static_cast<UINT>(
      TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN | TPM_TOPALIGN, screen.x, screen.y, hwnd_, nullptr));
  DestroyMenu(menu);
  // 菜单开着时这个标签可能已被关掉（自动关旧标签、网页 window.close）：按 id 重新找
  Tab* current = FindTab(id);
  const size_t at = current ? IndexOf(*current) : tabs_.size();
  if (command == kMenuTabReopen) {
    ReopenClosedTab();
    return;
  }
  if (command == kMenuTabNew) {
    NewTab(L"", true, at < tabs_.size() ? at + 1 : tabs_.size());
    return;
  }
  if (!current || at >= tabs_.size()) return;
  switch (command) {
    case kMenuTabReload:
      if (current->webview) {
        current->webview->Reload();
      } else if (!current->controller && !current->creating) {
        current->retried = false;
        current->lazy = false;
        current->error.clear();
        CreateController(*current);
      }
      break;
    case kMenuTabDuplicate:
      DuplicateTab(at);
      break;
    case kMenuTabPin:
      TogglePinned(at);
      break;
    case kMenuTabMute:
      ToggleMuted(at);
      break;
    case kMenuTabClose:
      CloseTab(at);
      break;
    case kMenuTabCloseOthers:
    case kMenuTabCloseRight: {
      // 智能体标签（Pi 正在用或留着复用）与固定的标签不在批量关闭之列
      std::vector<uint32_t> ids;
      for (size_t i = command == kMenuTabCloseRight ? at + 1 : 0; i < tabs_.size(); ++i) {
        const Tab& other = *tabs_[i];
        if (other.id != id && !other.agent && !other.pinned) ids.push_back(other.id);
      }
      CloseTabs(ids);
      if (Tab* kept = FindTab(id); kept && IndexOf(*kept) < tabs_.size() && command == kMenuTabCloseOthers) {
        Activate(IndexOf(*kept));
      }
      break;
    }
    default:
      break;
  }
}

void BrowserWindow::DuplicateTab(size_t index) {
  if (index >= tabs_.size() || !IsWebUrl(tabs_[index]->url)) return;
  const std::wstring url = tabs_[index]->url;
  const std::wstring title = tabs_[index]->title;
  NewTab(url, true, index + 1);
  if (Tab* copy = ActiveTab(); copy && copy->title.empty()) copy->title = title;
}

void BrowserWindow::CloseTabs(const std::vector<uint32_t>& ids) {
  for (const uint32_t id : ids) {
    if (destroyed_) return;
    if (Tab* tab = FindTab(id)) {
      const size_t index = IndexOf(*tab);
      if (index < tabs_.size()) CloseTab(index);
    }
  }
}

// 固定的标签排在最左边一组，不被「自动关旧标签」关掉；取消固定放回这一组后面
void BrowserWindow::TogglePinned(size_t index) {
  if (index >= tabs_.size() || tabs_[index]->agent) return;
  Tab& tab = *tabs_[index];
  tab.pinned = !tab.pinned;
  size_t others = 0;
  for (size_t i = 0; i < tabs_.size(); ++i) {
    if (i != index && tabs_[i]->pinned) ++others;
  }
  MoveTab(index, std::min(others, tabs_.size() - 1));  // 位置没变时它什么也不做，下面照样重画
  Relayout();
  InvalidateChrome();
  if (!tab.pinned) EnforceTabLimit();  // 取消固定后按上限再收一次
  ScheduleSessionSave();
}

void BrowserWindow::ToggleMuted(size_t index) {
  if (index >= tabs_.size()) return;
  Tab& tab = *tabs_[index];
  ComPtr<ICoreWebView2_8> audio;
  if (!tab.webview || FAILED(tab.webview.As(&audio))) return;
  tab.muted = !tab.muted;
  audio->put_IsMuted(tab.muted ? TRUE : FALSE);
  Relayout();
  InvalidateChrome();
}

void BrowserWindow::RememberClosedTab(const Tab& tab) {
  if (tab.agent) return;
  const std::wstring& url = tab.url.empty() ? tab.pendingUrl : tab.url;
  if (url.empty() || IsNewTabPageUrl(url) || url.size() > 16384 ||
      !(IsWebUrl(url) || IsInternalPage(url) || url.starts_with(L"file:"))) {
    return;
  }
  auto& closed = profile_->closedTabs;
  closed.push_back(ClosedTab{url, tab.title});
  if (closed.size() > kClosedTabLimit) closed.erase(closed.begin());
}

void BrowserWindow::ReopenClosedTab() {
  auto& closed = profile_->closedTabs;
  if (closed.empty()) return;
  const ClosedTab item = closed.back();
  closed.pop_back();
  NewTab(item.url, true);
  if (Tab* tab = ActiveTab(); tab && tab->title.empty()) tab->title = item.title;
  InvalidateChrome();
}

// 链接、window.open 打开的标签排在打开者右边；同一打开者连开的几个依次往后
size_t BrowserWindow::InsertIndexFor(uint32_t openerId) const {
  for (size_t i = 0; i < tabs_.size(); ++i) {
    if (tabs_[i]->id != openerId) continue;
    size_t at = i + 1;
    while (at < tabs_.size() && tabs_[at]->openedBy == openerId) ++at;
    return at;
  }
  return tabs_.size();
}

// 跳过缓存重新加载（Ctrl+Shift+R、Shift+F5、Ctrl+F5）
void BrowserWindow::HardReload() {
  Tab* tab = ActiveTab();
  if (!tab || !tab->webview) return;
  const HRESULT hr = tab->webview->CallDevToolsProtocolMethod(
      L"Page.reload", L"{\"ignoreCache\":true}",
      Callback<ICoreWebView2CallDevToolsProtocolMethodCompletedHandler>([](HRESULT, LPCWSTR) -> HRESULT { return S_OK; })
          .Get());
  if (FAILED(hr)) tab->webview->Reload();
}

// 用户在空闲的智能体标签里自己打开网页：这个标签归用户，不再放回池里被 Pi 租走；
// 去掉智能体专用的设置（禁自动播放、固定光栅比例、默认弹框关闭）。Pi 正在用的标签不接管
bool BrowserWindow::AdoptAgentTab(Tab& tab) {
  if (!tab.agent) return true;
  if (tab.leased || tab.leaseDone) return false;
  ResumeTab(tab);
  ApplyAgentScale(tab, false);
  tab.agent = false;
  tab.agentAwake = false;
  if (tab.webview) {
    ComPtr<ICoreWebView2Settings> settings;
    if (SUCCEEDED(tab.webview->get_Settings(settings.GetAddressOf()))) {
      settings->put_AreDefaultScriptDialogsEnabled(TRUE);
    }
    if (!tab.agentScript.empty()) {
      tab.webview->RemoveScriptToExecuteOnDocumentCreated(tab.agentScript.c_str());
      tab.agentScript.clear();
    }
    tab.webview->ExecuteScript(L"window.__jisuAllowMedia=true", nullptr);  // 当前页面里已装的拦截随之放行
  }
  PlaceTab(tab);
  UpdateMemoryTarget(tab);
  InvalidateChrome();
  return true;
}

// 外壳发起的打开：可以是内部页面（书签、历史……）；没建控件的标签现在建
void BrowserWindow::NavigateTab(Tab& tab, const std::wstring& url) {
  tab.url = url;
  tab.allowInternal = IsInternalPage(url);
  tab.lazy = false;
  if (tab.webview) {
    tab.webview->Navigate(url.c_str());
  } else {
    tab.pendingUrl = url;
    CreateController(tab);
  }
  ScheduleSessionSave();
}

std::wstring BrowserWindow::SessionFile() const { return profileDir_ + L"\\Session.json"; }

// 记下所有窗口里用户开着的标签。clean = 用户正常关掉了最后一个窗口：下次启动不恢复；
// 崩溃、被结束、关机时文件停在 clean=false，下次启动恢复
void BrowserWindow::SaveSession(bool clean, bool durable) {
  if (!profile_->sessionWritable || profileDir_.empty()) return;
  std::string out = std::string("{\"clean\":") + (clean ? "true" : "false") + ",\"tabs\":[";
  bool first = true;
  const BrowserWindow* main = nullptr;
  for (const auto* window : liveWindows_) {
    if (window->profile_ != profile_) continue;
    if (!main) main = window;
    for (size_t i = 0; i < window->tabs_.size(); ++i) {
      const Tab& tab = *window->tabs_[i];
      const std::wstring& url = tab.url.empty() ? tab.pendingUrl : tab.url;
      if (tab.agent || url.empty() || IsNewTabPageUrl(url) || url.size() > 16384) continue;
      out += first ? "{\"url\":" : ",{\"url\":";
      first = false;
      json::AppendQuoted(out, ToUtf8(url));
      out += ",\"title\":";
      json::AppendQuoted(out, ToUtf8(TruncateUtf16(tab.title, 512)));
      out += std::string(",\"active\":") + (window == main && i == window->active_ ? "true" : "false");
      out += std::string(",\"pinned\":") + (tab.pinned ? "true" : "false") + "}";
    }
  }
  out += "]}";
  profile_->writer.Write(SessionFile(), std::move(out), durable);
}

// 首帧前：上次没正常退出时开着的标签放回来。只有当时的当前标签马上加载，其余点到才建（不一下子全开）
void BrowserWindow::RestoreSession() {
  auto& saved = profile_->restoreTabs;
  if (saved.empty() || root_ != this || options_.appMode || tabs_.empty()) return;
  const std::vector<SessionTab> items = std::move(saved);
  saved.clear();
  std::vector<std::unique_ptr<Tab>> restored;
  const Tab* chosen = nullptr;
  for (const SessionTab& item : items) {
    auto tab = std::make_unique<Tab>();
    tab->id = nextTabId_++;
    tab->url = item.url;
    tab->pendingUrl = item.url;
    tab->title = item.title;
    tab->pinned = item.pinned;
    tab->lazy = true;
    if (item.active || !chosen) chosen = tab.get();
    restored.push_back(std::move(tab));
  }
  std::stable_partition(restored.begin(), restored.end(), [](const auto& tab) { return tab->pinned; });
  // 空白启动：恢复的标签替换掉初始的新标签页；带网址启动：恢复的标签排在前面，新网址仍是当前标签
  const bool replaceFirst = options_.startUrl.empty() && tabs_.size() == 1 && tabs_[0]->pendingUrl.empty() &&
                            !tabs_[0]->controller && !tabs_[0]->creating;
  if (replaceFirst) {
    tabs_.clear();
    for (auto& tab : restored) tabs_.push_back(std::move(tab));
    active_ = 0;
    for (size_t i = 0; i < tabs_.size(); ++i) {
      if (tabs_[i].get() == chosen) active_ = i;
    }
    tabs_[active_]->lazy = false;
    firstTabId_ = tabs_[active_]->id;
  } else {
    const uint32_t current = tabs_[active_]->id;
    tabs_.insert(tabs_.begin(), std::make_move_iterator(restored.begin()), std::make_move_iterator(restored.end()));
    if (const Tab* tab = FindTab(current)) active_ = IndexOf(*tab);
  }
  Relayout();
  UpdateAddressBar();
  UpdateWindowTitle();
  ScheduleSessionSave();
}

void BrowserWindow::OnProcessFailed(uint32_t id, COREWEBVIEW2_PROCESS_FAILED_KIND kind, ICoreWebView2* sender) {
  Tab* t = FindTab(id);
  if (fullscreenTab_ == id) ExitContentFullscreen();
  if (options_.appMode) {
    if (kind == COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED) {
      // 共享内核进程没了（例如日常实例连同子进程被结束）：控件已失效，整套重建
      environment_.Reset();
      ScheduleAppRecovery(500, L"网页内核已退出，正在重新连接…");
    } else if (kind == COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_EXITED) {
      sender->Reload();
    }
    return;
  }
  if (kind == COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED) {
    // 日常窗口：共享内核进程退出（崩溃、被结束、运行时更新），所有标签的控件都失效。
    // 每个标签都会报一次，只处理第一次；具体重建放到回调之外做
    if (!environment_ || profile_->recovering) return;
    profile_->recovering = true;
    PostTask([this] {
      for (auto* window : liveWindows_) {
        if (window->profile_ != profile_) continue;
        window->environment_.Reset();
        ++window->envGeneration_;  // 旧内核迟到的控件回调都丢掉
        window->CloseExtensionPopup();
        if (window->spare_) {
          if (window->spare_->controller) window->spare_->controller->Close();
          window->spare_.reset();
        }
        std::vector<uint32_t> agents;
        for (auto& tab : window->tabs_) {
          if (tab->agent) agents.push_back(tab->id);
          else window->ResetTabWebView(*tab);
        }
        // 智能体标签交还给桥（连接随之断开），内核回来后 Pi 会重新租
        for (const uint32_t agentId : agents) {
          if (Tab* agent = window->FindTab(agentId); agent && window->IndexOf(*agent) < window->tabs_.size()) {
            if (window->tabs_.size() > 1) {
              window->CloseTab(window->IndexOf(*agent));
            } else {
              if (window->bridge_) window->bridge_->OnAgentTabGone(agentId);
              agent->agent = false;
              agent->leased = false;
              agent->url.clear();
              window->ResetTabWebView(*agent);
            }
          }
        }
        window->message_ = L"网页内核已退出，正在重新连接…";
        window->InvalidateChrome();
      }
      // 由主窗口重建内核（智能体桥、扩展都在它身上）；扩展在新内核里重新装
      BrowserWindow* primary = &PrimaryWindow();
      for (auto& extension : primary->extensions_) {
        extension.handle.Reset();
        extension.result = E_PENDING;
      }
      primary->extensionIndex_ = 0;
      primary->extensionProfile_.Reset();
      primary->profileConfigured_ = false;
      primary->extensionsStarted_ = false;
      primary->recoverDelay_ = 0;
      primary->ScheduleEnvironmentRecovery();
    });
    return;
  }
  if (!t || kind != COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_EXITED) {
    return;
  }
  if (t->agent) {
    // 智能体页面崩溃：断开当前连接、标签回池，并重新加载
    if (bridge_) {
      bridge_->OnAgentTabGone(id);
    }
    t->leased = false;
    t->agentAwake = false;
    sender->Reload();
    PlaceTab(*t);
    InvalidateChrome();
    return;
  }
  // 普通页面的渲染进程崩溃：当前标签马上重载，后台标签切过来时重载；一分钟内再崩就停下，换成说明页
  const ULONGLONG now = GetTickCount64();
  const bool again = t->crashedAt && now - t->crashedAt < 60000;
  t->crashedAt = now;
  if (again) {
    const std::wstring url = t->url;
    const std::wstring page =
        L"<!doctype html><meta charset=utf-8><title>页面已崩溃</title>"
        L"<body style=\"margin:0;height:100vh;display:flex;flex-direction:column;align-items:center;justify-content:center;"
        L"font:14px 'Microsoft YaHei UI',sans-serif;color:#5f6368;background:#fff\">"
        L"<h2 style=\"color:#1f1f1f;font-weight:600\">这个页面崩溃了</h2>"
        L"<p>一分钟内连续崩溃两次，已停止自动重新加载。</p>" +
        (IsWebUrl(url) ? L"<p><a href=\"" + HtmlEscape(url) + L"\">重新加载</a></p>" : std::wstring()) + L"</body>";
    sender->NavigateToString(page.c_str());
  } else if (IsActive(*t)) {
    sender->Reload();
  } else {
    t->crashed = true;
  }
}

void BrowserWindow::RecoverEnvironment() {
  profile_->recovering = false;
  if (options_.appMode || destroyed_ || environment_) return;
  StartEnvironment();
}

BrowserWindow& BrowserWindow::PrimaryWindow() {
  for (auto* window : liveWindows_) {
    if (window->profile_ == profile_ && window->ownsPrimary_ && !window->destroyed_) return *window;
  }
  return *this;
}

// 内核没了：丢掉失效的控件，留着网址；当前标签内核回来马上重建，后台标签点到时再建
void BrowserWindow::ResetTabWebView(Tab& tab) {
  AbandonNewWindow(tab);
  if (tab.controller) tab.controller->Close();
  tab.controller.Reset();
  tab.webview.Reset();
  tab.creating = false;
  tab.retried = false;
  tab.visible = true;
  tab.suspended = false;
  tab.suspending = false;
  tab.loading = false;
  tab.audible = false;
  tab.faviconHost = false;
  tab.agentScript.clear();
  if (tab.pendingUrl.empty() && !IsNewTabPageUrl(tab.url)) tab.pendingUrl = tab.url;
  tab.lazy = !IsActive(tab);
}

// 看不见的标签（后台标签、池里闲着的智能体标签）降到低内存档，显示、出声、被租用时恢复正常
void BrowserWindow::UpdateMemoryTarget(Tab& tab) {
  ComPtr<ICoreWebView2_19> view;
  if (!tab.webview || FAILED(tab.webview.As(&view))) return;
  const bool low = !tab.visible && !tab.audible && !IsSpare(tab) && !(tab.agent && tab.leased);
  view->put_MemoryUsageTargetLevel(low ? COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL_LOW
                                       : COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL_NORMAL);
}

// 切到一个标签后把键盘焦点交给它的网页（新标签页给地址栏），方向键、空格翻页马上能用；
// 只在本窗口在前台、用户没在地址栏里输入时做，不抢别的程序的焦点
void BrowserWindow::FocusActiveContent() {
  Tab* tab = ActiveTab();
  if (!tab || options_.appMode || GetForegroundWindow() != hwnd_ || omniboxFocused_) return;
  if (!tab->agent && IsNewTabPageUrl(tab->url) && tab->pendingUrl.empty()) {
    if (edit_) SetFocus(edit_);
    return;
  }
  if (tab->controller && tab->visible) {
    tab->controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
  } else {
    tab->focusOnCreate = true;
  }
}

}  // namespace jisu
