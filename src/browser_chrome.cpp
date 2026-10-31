// 外壳界面的小件：悬停提示、地址栏右键菜单与粘贴、书签栏空白处菜单、弹窗拦截提示、扩展按钮悬停。
#include "bookmark_editor.h"
#include "browser_url.h"
#include "browser_window.h"
#include "extension_toolbar.h"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <cwctype>

namespace jisu {
namespace {

constexpr UINT kMenuBarAddPage = 7101;
constexpr UINT kMenuBarNewFolder = 7102;
constexpr UINT kMenuBarToggle = 7103;
constexpr UINT kMenuBarManager = 7104;
constexpr UINT kMenuEditUndo = 7201;
constexpr UINT kMenuEditCut = 7202;
constexpr UINT kMenuEditCopy = 7203;
constexpr UINT kMenuEditPaste = 7204;
constexpr UINT kMenuEditPasteGo = 7205;
constexpr UINT kMenuEditDelete = 7206;
constexpr UINT kMenuEditSelectAll = 7207;
constexpr UINT kMenuPopupOpenAll = 7290;
constexpr UINT kMenuPopupAllow = 7291;
constexpr UINT kMenuPopupBase = 7300;

std::wstring ClipboardText(HWND owner) {
  std::wstring text;
  if (!IsClipboardFormatAvailable(CF_UNICODETEXT) || !OpenClipboard(owner)) return text;
  if (HANDLE data = GetClipboardData(CF_UNICODETEXT)) {
    if (const auto* chars = static_cast<const wchar_t*>(GlobalLock(data))) {
      const size_t limit = GlobalSize(data) / sizeof(wchar_t);
      text.assign(chars, wcsnlen(chars, std::min<size_t>(limit, 1 << 16)));
      GlobalUnlock(data);
    }
  }
  CloseClipboard();
  return text;
}

// 多行文字粘进单行地址栏：被换行拆开的网址去掉换行接回去，一段文字的换行变成空格（系统 EDIT 只留第一行）
std::wstring SingleLine(const std::wstring& text) {
  std::wstring joined;
  for (const wchar_t c : text) {
    if (c != L'\r' && c != L'\n') joined.push_back(c);
  }
  const std::wstring trimmed = Trim(joined);
  if (trimmed.find_first_of(L" \t") == std::wstring::npos) return trimmed;
  std::wstring spaced;
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == L'\r' && i + 1 < text.size() && text[i + 1] == L'\n') continue;
    spaced.push_back(text[i] == L'\r' || text[i] == L'\n' ? L' ' : text[i]);
  }
  return Trim(spaced);
}

}  // namespace

// ---------------------------------------------------------------------------
// 悬停提示：标签显示完整标题与网址，图标按钮显示名称与快捷键

std::wstring BrowserWindow::TooltipText(const ChromeHit& hit) const {
  const size_t index = hit.index < 0 ? 0 : static_cast<size_t>(hit.index);
  const Tab* active = ActiveTab();
  switch (hit.kind) {
    case ChromeHit::kTab: {
      if (index >= tabs_.size()) return {};
      const Tab& tab = *tabs_[index];
      std::wstring title = tab.title.empty() ? std::wstring(tab.agent ? L"智能体" : L"新标签页") : tab.title;
      if (tab.agent) title = L"智能体标签 · " + title;
      if (tab.pinned) title += L"（已固定）";
      const std::wstring url = DisplayUrl(tab.url.empty() ? tab.pendingUrl : tab.url);
      return url.empty() || url == title ? title : title + L"\n" + url;
    }
    case ChromeHit::kTabClose:
      return L"关闭标签页 (Ctrl+W)";
    case ChromeHit::kTabAudio:
      return index < tabs_.size() && tabs_[index]->muted ? L"取消静音" : L"将此标签页静音";
    case ChromeHit::kNewTab:
      return L"新建标签页 (Ctrl+T)";
    case ChromeHit::kBack:
      return L"后退 (Alt+←)";
    case ChromeHit::kForward:
      return L"前进 (Alt+→)";
    case ChromeHit::kReload:
      return active && active->loading ? L"停止加载" : L"重新加载 (F5)\nCtrl+F5 跳过缓存重新加载";
    case ChromeHit::kStar:
      return starredValue_ && active && active->url == starredUrl_ ? L"编辑书签 (Ctrl+D)" : L"为此网页添加书签 (Ctrl+D)";
    case ChromeHit::kPopupBlocked:
      return L"已拦截此网页的弹出窗口";
    case ChromeHit::kMenu:
      return L"菜单";
    case ChromeHit::kBookmark: {
      if (index >= barItems_.size()) return {};
      const BookmarkNode& node = *barItems_[index];
      if (node.folder) return node.title;
      return node.title.empty() ? node.url : node.title + L"\n" + node.url;
    }
    case ChromeHit::kBookmarkOverflow:
      return L"更多书签";
    case ChromeHit::kMinimize:
      return L"最小化";
    case ChromeHit::kMaximize:
      return IsZoomed(hwnd_) ? L"向下还原" : L"最大化";
    case ChromeHit::kWindowClose:
      return L"关闭";
    default:
      return {};
  }
}

void BrowserWindow::UpdateChromeTooltip() {
  if (options_.appMode || destroyed_ || !hwnd_) return;
  const ChromeHit hit = dragTabId_ || draggingBookmark_ ? ChromeHit{} : hover_;
  const std::wstring text = hit.kind == ChromeHit::kNone ? std::wstring() : TooltipText(hit);
  if (!chromeTooltip_) {
    if (text.empty()) return;
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_WIN95_CLASSES};
    InitCommonControlsEx(&controls);
    chromeTooltip_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE, TOOLTIPS_CLASSW, nullptr,
                                     WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                                     CW_USEDEFAULT, hwnd_, nullptr, instance_, nullptr);
    if (!chromeTooltip_) return;
    SendMessageW(chromeTooltip_, TTM_SETMAXTIPWIDTH, 0, MulDiv(420, static_cast<int>(dpi_), 96));
    TOOLINFOW info{};
    info.cbSize = sizeof(info);
    info.uFlags = TTF_SUBCLASS;
    info.hwnd = hwnd_;
    info.uId = 1;
    info.lpszText = const_cast<LPWSTR>(L"");
    SendMessageW(chromeTooltip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&info));
  }
  if (hit == tooltipHit_ && text == tooltipText_) return;
  tooltipHit_ = hit;
  tooltipText_ = text;
  // 提示区域就是悬停中的那个按钮 / 标签：离开它提示消失，停在别处换成别的文字
  D2D1_RECT_F area{};
  const size_t index = hit.index < 0 ? 0 : static_cast<size_t>(hit.index);
  switch (hit.kind) {
    case ChromeHit::kTab: if (index < layout_.tabs.size()) area = layout_.tabs[index]; break;
    case ChromeHit::kTabClose: if (index < layout_.tabCloses.size()) area = layout_.tabCloses[index]; break;
    case ChromeHit::kTabAudio: if (index < layout_.tabAudio.size()) area = layout_.tabAudio[index]; break;
    case ChromeHit::kNewTab: area = layout_.newTab; break;
    case ChromeHit::kBack: area = layout_.back; break;
    case ChromeHit::kForward: area = layout_.forward; break;
    case ChromeHit::kReload: area = layout_.reload; break;
    case ChromeHit::kStar: area = layout_.star; break;
    case ChromeHit::kPopupBlocked: area = layout_.popupBlocked; break;
    case ChromeHit::kMenu: area = layout_.menu; break;
    case ChromeHit::kBookmark: if (index < layout_.bookmarks.size()) area = layout_.bookmarks[index]; break;
    case ChromeHit::kBookmarkOverflow: area = layout_.bookmarkOverflow; break;
    case ChromeHit::kMinimize: area = layout_.minimize; break;
    case ChromeHit::kMaximize: area = layout_.maximize; break;
    case ChromeHit::kWindowClose: area = layout_.windowClose; break;
    default: break;
  }
  TOOLINFOW info{};
  info.cbSize = sizeof(info);
  info.hwnd = hwnd_;
  info.uId = 1;
  if (!text.empty()) {
    const float scale = layout_.scale;
    info.rect = RECT{static_cast<LONG>(area.left * scale), static_cast<LONG>(area.top * scale),
                     static_cast<LONG>(area.right * scale), static_cast<LONG>(area.bottom * scale)};
  }
  SendMessageW(chromeTooltip_, TTM_POP, 0, 0);
  SendMessageW(chromeTooltip_, TTM_NEWTOOLRECTW, 0, reinterpret_cast<LPARAM>(&info));
  info.lpszText = const_cast<LPWSTR>(tooltipText_.c_str());
  SendMessageW(chromeTooltip_, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&info));
}

// ---------------------------------------------------------------------------
// 地址栏

LRESULT CALLBACK BrowserWindow::AddressBarProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR id,
                                               DWORD_PTR data) {
  auto* self = reinterpret_cast<BrowserWindow*>(data);
  switch (msg) {
    case WM_CONTEXTMENU: {
      POINT screen{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
      if (screen.x == -1 && screen.y == -1) {  // 键盘调出：菜单放在地址栏左下
        RECT rect{};
        GetWindowRect(hwnd, &rect);
        screen = POINT{rect.left + 8, rect.bottom};
      }
      self->ShowAddressBarMenu(screen);
      return 0;
    }
    case WM_PASTE:
      if (self->PasteIntoAddressBar()) return 0;
      break;
    case WM_NCDESTROY:
      RemoveWindowSubclass(hwnd, &BrowserWindow::AddressBarProc, id);
      break;
    default:
      break;
  }
  return DefSubclassProc(hwnd, msg, wParam, lParam);
}

void BrowserWindow::ShowAddressBarMenu(POINT screen) {
  if (!edit_) return;
  if (GetFocus() != edit_) SetFocus(edit_);
  DWORD start = 0, end = 0;
  SendMessageW(edit_, EM_GETSEL, reinterpret_cast<WPARAM>(&start), reinterpret_cast<LPARAM>(&end));
  const UINT selection = start != end ? 0 : MF_GRAYED;
  const UINT paste = IsClipboardFormatAvailable(CF_UNICODETEXT) ? 0 : MF_GRAYED;
  HMENU menu = CreatePopupMenu();
  AppendMenuW(menu, MF_STRING | (SendMessageW(edit_, EM_CANUNDO, 0, 0) ? 0 : MF_GRAYED), kMenuEditUndo, L"撤销\tCtrl+Z");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING | selection, kMenuEditCut, L"剪切\tCtrl+X");
  AppendMenuW(menu, MF_STRING | selection, kMenuEditCopy, L"复制\tCtrl+C");
  AppendMenuW(menu, MF_STRING | paste, kMenuEditPaste, L"粘贴\tCtrl+V");
  AppendMenuW(menu, MF_STRING | paste, kMenuEditPasteGo, L"粘贴并转到");
  AppendMenuW(menu, MF_STRING | selection, kMenuEditDelete, L"删除");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, kMenuEditSelectAll, L"全选\tCtrl+A");
  const UINT command = static_cast<UINT>(
      TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN | TPM_TOPALIGN, screen.x, screen.y, hwnd_, nullptr));
  DestroyMenu(menu);
  switch (command) {
    case kMenuEditUndo: SendMessageW(edit_, EM_UNDO, 0, 0); break;
    case kMenuEditCut: SendMessageW(edit_, WM_CUT, 0, 0); break;
    case kMenuEditCopy: SendMessageW(edit_, WM_COPY, 0, 0); break;
    case kMenuEditPaste: SendMessageW(edit_, WM_PASTE, 0, 0); break;
    case kMenuEditPasteGo: PasteAndGo(); break;
    case kMenuEditDelete: SendMessageW(edit_, WM_CLEAR, 0, 0); break;
    case kMenuEditSelectAll: SendMessageW(edit_, EM_SETSEL, 0, -1); break;
    default: break;
  }
}

bool BrowserWindow::PasteIntoAddressBar() {
  const std::wstring text = ClipboardText(hwnd_);
  if (text.find_first_of(L"\r\n") == std::wstring::npos) return false;  // 单行：交给系统 EDIT 照常粘贴
  const std::wstring line = SingleLine(text);
  SendMessageW(edit_, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(line.c_str()));
  return true;
}

void BrowserWindow::PasteAndGo() {
  const std::wstring line = SingleLine(ClipboardText(hwnd_));
  if (line.empty() || !edit_) return;
  SetWindowTextW(edit_, line.c_str());
  editing_ = true;
  NavigateFromAddressBar();
}

// Ctrl+Backspace：删掉光标前一个词（网址里的 / . : 等算分隔）
void BrowserWindow::DeletePreviousWord() {
  DWORD start = 0, end = 0;
  SendMessageW(edit_, EM_GETSEL, reinterpret_cast<WPARAM>(&start), reinterpret_cast<LPARAM>(&end));
  if (start != end) {
    SendMessageW(edit_, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L""));
    return;
  }
  const int length = GetWindowTextLengthW(edit_);
  std::wstring text(static_cast<size_t>(length) + 1, L'\0');
  GetWindowTextW(edit_, text.data(), length + 1);
  text.resize(static_cast<size_t>(length));
  auto isWord = [](wchar_t c) { return std::iswalnum(c) || c == L'_' || c > 0x7F; };
  size_t pos = std::min<size_t>(start, text.size());
  while (pos > 0 && !isWord(text[pos - 1])) --pos;
  while (pos > 0 && isWord(text[pos - 1])) --pos;
  if (pos == start) return;
  SendMessageW(edit_, EM_SETSEL, pos, start);
  SendMessageW(edit_, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L""));
}

// ---------------------------------------------------------------------------
// 书签栏空白处、弹窗拦截

void BrowserWindow::ShowBookmarkBarMenu(POINT screen) {
  const Tab* active = ActiveTab();
  const bool canAdd = profileLoaded_ && active && IsStarrable(active->url);
  const std::wstring url = active ? active->url : std::wstring();
  const std::wstring title = active && !active->title.empty() ? active->title : url;
  HMENU menu = CreatePopupMenu();
  AppendMenuW(menu, MF_STRING | (canAdd ? 0 : MF_GRAYED), kMenuBarAddPage, L"将此网页加入书签栏…");
  AppendMenuW(menu, MF_STRING | (profileLoaded_ ? 0 : MF_GRAYED), kMenuBarNewFolder, L"新建文件夹…");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING | (settings_.bookmarkBar ? MF_CHECKED : 0), kMenuBarToggle, L"显示书签栏\tCtrl+Shift+B");
  AppendMenuW(menu, MF_STRING, kMenuBarManager, L"书签管理器\tCtrl+Shift+O");
  const UINT command = static_cast<UINT>(
      TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN | TPM_TOPALIGN, screen.x, screen.y, hwnd_, nullptr));
  DestroyMenu(menu);
  switch (command) {
    case kMenuBarAddPage: {
      const auto edit = ShowBookmarkEditor(hwnd_, instance_, bookmarks_, title, url, bookmarks_.Bar().id, false);
      if (edit && ApplyBookmarkEdit(bookmarks_, *edit, 0)) OnBookmarksChanged();
      break;
    }
    case kMenuBarNewFolder:
      if (BookmarkNode* folder = bookmarks_.AddFolder(bookmarks_.Bar(), bookmarks_.Bar().children.size(), L"新建文件夹")) {
        const int64_t id = folder->id;
        OnBookmarksChanged();
        OpenBookmarkManager(L"#edit=" + std::to_wstring(id));  // 在管理器里接着改名
      }
      break;
    case kMenuBarToggle:
      settings_.bookmarkBar = !settings_.bookmarkBar;
      SaveSettings();
      OnBookmarksChanged();
      break;
    case kMenuBarManager:
      OpenBookmarkManager(std::wstring());
      break;
    default:
      break;
  }
}

bool BrowserWindow::PopupAllowed(const std::wstring& url) const {
  const std::wstring origin = FaviconOrigin(url);
  return !origin.empty() && std::find(settings_.popupAllow.begin(), settings_.popupAllow.end(), origin) != settings_.popupAllow.end();
}

void BrowserWindow::OnPopupBlocked(Tab& source, const std::wstring& url) {
  auto& list = source.blockedPopups;
  if (std::find(list.begin(), list.end(), url) == list.end()) {
    list.push_back(url);
    if (list.size() > 8) list.erase(list.begin());
  }
  if (IsActive(source)) {
    Relayout();
    InvalidateChrome();
  }
}

void BrowserWindow::ShowPopupBlockedMenu(POINT screen) {
  Tab* tab = ActiveTab();
  if (!tab || tab->blockedPopups.empty()) return;
  const uint32_t id = tab->id;
  const std::vector<std::wstring> urls = tab->blockedPopups;
  const std::wstring origin = FaviconOrigin(tab->url);
  HMENU menu = CreatePopupMenu();
  AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"已拦截此网页未经点击就打开的窗口：");
  for (size_t i = 0; i < urls.size(); ++i) {
    AppendMenuW(menu, MF_STRING, kMenuPopupBase + static_cast<UINT>(i), MenuText(urls[i].empty() ? L"about:blank" : urls[i]).c_str());
  }
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  if (urls.size() > 1) AppendMenuW(menu, MF_STRING, kMenuPopupOpenAll, L"全部打开");
  if (!origin.empty()) {
    const std::wstring host = origin.substr(origin.find(L"://") + 3);
    AppendMenuW(menu, MF_STRING, kMenuPopupAllow, (L"始终允许 " + MenuText(host) + L" 弹出窗口").c_str());
  }
  const UINT command = static_cast<UINT>(
      TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN | TPM_TOPALIGN, screen.x, screen.y, hwnd_, nullptr));
  DestroyMenu(menu);
  Tab* current = FindTab(id);
  if (!current || IndexOf(*current) >= tabs_.size()) return;
  std::vector<std::wstring> open;
  if (command >= kMenuPopupBase && command - kMenuPopupBase < urls.size()) {
    open.push_back(urls[command - kMenuPopupBase]);
    std::erase(current->blockedPopups, open.front());
  } else if (command == kMenuPopupOpenAll || command == kMenuPopupAllow) {
    open = urls;
    current->blockedPopups.clear();
    if (command == kMenuPopupAllow && std::find(settings_.popupAllow.begin(), settings_.popupAllow.end(), origin) == settings_.popupAllow.end()) {
      settings_.popupAllow.push_back(origin);
      SaveSettings();
    }
  }
  for (const auto& url : open) {
    NewTab(url.empty() ? std::wstring(L"about:blank") : url, true, InsertIndexFor(id));
  }
  Relayout();
  InvalidateChrome();
}

// ---------------------------------------------------------------------------
// 图标、提示、关机

void BrowserWindow::OnFaviconUpdated(uint64_t hash) {
  // 只失效变了的那一个图标，其余已解码的不动；书签管理页只重取图标，不整页重画
  const std::wstring file = favicons_.Directory() + L"\\" + FaviconKeyFromHash(hash) + L".png";
  for (auto* window : liveWindows_) {
    if (window->profile_ != profile_ || window->destroyed_) continue;
    window->painter_.ForgetFavicon(file);
    window->InvalidateChrome();
    for (auto& tab : window->tabs_) {
      if (tab->webview && IsBookmarksPageUrl(tab->url)) window->PostToPage(*tab, "{\"type\":\"bm.icons.refresh\"}");
    }
  }
}

// 提示框不在 WebView2 回调里弹（嵌套模态循环）：投递到消息队列再显示
void BrowserWindow::ShowNotice(const std::wstring& text) {
  PostTask([this, text] { MessageBoxW(hwnd_, text.c_str(), kAppName, MB_OK | MB_ICONINFORMATION); });
}

void BrowserWindow::FlushForEndSession() {
  if (profile_->endingSession) return;
  profile_->endingSession = true;
  for (auto* window : liveWindows_) {
    if (window->profile_ != profile_) continue;
    window->SaveWindowPlacement();
    window->FlushBookmarkSave();
  }
  SaveSession(false, true);
  history_.Flush();
  profile_->writer.Flush(4000);
}

// ---------------------------------------------------------------------------
// 扩展按钮：悬停有底色；点击不拿走键盘焦点（拿走后外壳快捷键、网页输入都会失灵）

LRESULT CALLBACK BrowserWindow::ExtensionButtonProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR id,
                                                    DWORD_PTR data) {
  auto* self = reinterpret_cast<BrowserWindow*>(data);
  switch (msg) {
    case WM_MOUSEMOVE:
      if (self->hoverExtensionButton_ != hwnd) {
        if (HWND previous = self->hoverExtensionButton_) InvalidateRect(previous, nullptr, FALSE);
        self->hoverExtensionButton_ = hwnd;
        InvalidateRect(hwnd, nullptr, FALSE);
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&track);
      }
      break;
    case WM_MOUSELEAVE:
      if (self->hoverExtensionButton_ == hwnd) {
        self->hoverExtensionButton_ = nullptr;
        InvalidateRect(hwnd, nullptr, FALSE);
      }
      break;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
      SetCapture(hwnd);
      SendMessageW(hwnd, BM_SETSTATE, TRUE, 0);
      return 0;
    case WM_LBUTTONUP:
      if (GetCapture() == hwnd) {
        ReleaseCapture();
        SendMessageW(hwnd, BM_SETSTATE, FALSE, 0);
        RECT client{};
        GetClientRect(hwnd, &client);
        const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (PtInRect(&client, point) && IsWindowEnabled(hwnd)) {
          SendMessageW(GetParent(hwnd), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(hwnd), BN_CLICKED), reinterpret_cast<LPARAM>(hwnd));
        }
      }
      return 0;
    case WM_CAPTURECHANGED:
      SendMessageW(hwnd, BM_SETSTATE, FALSE, 0);
      break;
    case WM_NCDESTROY:
      if (self->hoverExtensionButton_ == hwnd) self->hoverExtensionButton_ = nullptr;
      RemoveWindowSubclass(hwnd, &BrowserWindow::ExtensionButtonProc, id);
      break;
    default:
      break;
  }
  return DefSubclassProc(hwnd, msg, wParam, lParam);
}

}  // namespace jisu
