#include "browser_window.h"
#include "browser_url.h"
#include <commctrl.h>
#include <algorithm>
#include <cwctype>

namespace jisu {
namespace {
std::wstring Lower(std::wstring text) { for (auto& c : text) c = static_cast<wchar_t>(std::towlower(c)); return text; }
std::wstring EditText(HWND edit) {
  const int length = GetWindowTextLengthW(edit); std::wstring text(static_cast<size_t>(length) + 1, L'\0');
  GetWindowTextW(edit, text.data(), length + 1); text.resize(length); return text;
}
}
void BrowserWindow::HideSuggestions() {
  ++suggestionGeneration_;
  if (suggestions_) ShowWindow(suggestions_, SW_HIDE);
}
void BrowserWindow::AcceptSuggestion() {
  if (!suggestions_ || !IsWindowVisible(suggestions_)) return;
  const auto selected = SendMessageW(suggestions_, LB_GETCURSEL, 0, 0);
  if (selected == LB_ERR || static_cast<size_t>(selected) >= suggestionUrls_.size()) return;
  const auto url = suggestionUrls_[selected];
  HideSuggestions();
  for (size_t i = 0; i < tabs_.size(); ++i) if (!tabs_[i]->agent && tabs_[i]->url == url) { Activate(i); FocusActiveContent(); return; }
  settingAddress_ = true; SetWindowTextW(edit_, url.c_str()); settingAddress_ = false;
  editing_ = true; NavigateFromAddressBar();
}
void BrowserWindow::ShowSuggestions() {
  if (!edit_ || !editing_ || !omniboxFocused_ || imeComposing_ || destroyed_) return;
  const auto text = EditText(edit_), query = Lower(Trim(text));
  if (query.empty() || query.size() > 200) { HideSuggestions(); return; }
  const uint64_t generation = ++suggestionGeneration_;
  const auto profile = profile_; const HWND target = hwnd_;
  const auto alive = std::weak_ptr<int>(alive_);
  history_.Start(profileDir_ + L"\\History.json");
  std::thread([this, profile, target, alive, generation, text, query] {
    auto rows = profile->history.Suggest(query, 8);
    auto* task = new std::function<void()>([this, alive, generation, text, query, rows = std::move(rows)]() mutable {
      if (alive.expired() || generation != suggestionGeneration_ || !omniboxFocused_ || EditText(edit_) != text || imeComposing_) return;
      // Each source has a label; matching existing tabs activates them rather than opening duplicates.
      std::vector<std::pair<std::wstring, std::wstring>> candidates;
      auto add = [&](const std::wstring& url, const std::wstring& label) {
        if (candidates.size() >= 10 || std::any_of(candidates.begin(), candidates.end(), [&](const auto& c) { return c.first == url; })) return;
        candidates.emplace_back(url, label);
      };
      for (const auto& t : tabs_) if (!t->agent && Lower(t->title + L" " + t->url).find(query) != std::wstring::npos)
        add(t->url, L"已开标签 · " + t->title + L"  " + DisplayUrl(t->url));
      std::function<void(const BookmarkNode&)> bookmarks = [&](const BookmarkNode& node) {
        if (candidates.size() >= 10) return;
        if (!node.folder && Lower(node.title + L" " + node.url).find(query) != std::wstring::npos)
          add(node.url, L"书签 · " + node.title + L"  " + node.url);
        for (const auto& child : node.children) bookmarks(*child);
      };
      if (profileLoaded_) { bookmarks(bookmarks_.Bar()); bookmarks(bookmarks_.Other()); bookmarks(bookmarks_.Mobile()); }
      for (const auto& row : rows) add(row.url, L"历史 · " + row.title + L"  " + row.url);
      if (!suggestions_) {
        suggestions_ = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, L"LISTBOX", L"地址建议",
            WS_POPUP | WS_BORDER | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
            0, 0, 0, 0, hwnd_, nullptr, instance_, nullptr);
        if (!suggestions_) return;
        SendMessageW(suggestions_, WM_SETFONT, reinterpret_cast<WPARAM>(editFont_), TRUE);
      }
      SendMessageW(suggestions_, LB_RESETCONTENT, 0, 0); suggestionUrls_.clear();
      for (const auto& [url, label] : candidates) {
        suggestionUrls_.push_back(url);
        SendMessageW(suggestions_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
      }
      suggestionUrls_.push_back(text);
      const auto search = L"搜索 · " + text;
      SendMessageW(suggestions_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(search.c_str()));
      SendMessageW(suggestions_, LB_SETITEMHEIGHT, 0, MulDiv(30, dpi_, 96));
      RECT rect{}; GetWindowRect(edit_, &rect);
      const int width = std::max(150L, rect.right - rect.left);
      SetWindowPos(suggestions_, HWND_TOP, rect.left, rect.bottom + MulDiv(8, dpi_, 96), width,
          MulDiv(static_cast<int>(suggestionUrls_.size()) * 30 + 4, dpi_, 96), SWP_NOACTIVATE | SWP_SHOWWINDOW);
      // Complete host prefixes only; never replace selected text, an IME composition, or a deletion.
      DWORD start = 0, end = 0;
      SendMessageW(edit_, EM_GETSEL, reinterpret_cast<WPARAM>(&start), reinterpret_cast<LPARAM>(&end));
      if (allowInline_ && start == text.size() && start == end && query.size() >= 2 && query.find_first_of(L" /:?=") == std::wstring::npos) {
        for (const auto& [url, label] : candidates) {
          (void)label;
          const auto origin = FaviconOrigin(url);
          const auto split = origin.find(L"://");
          if (split == std::wstring::npos) continue;
          const auto host = origin.substr(split + 3);
          if (!Lower(host).starts_with(query) || host.size() <= query.size()) continue;
          settingAddress_ = true; SetWindowTextW(edit_, host.c_str()); settingAddress_ = false;
          SendMessageW(edit_, EM_SETSEL, text.size(), host.size()); break;
        }
      }
      allowInline_ = false;
    });
    if (alive.expired() || !PostMessageW(target, WM_APP + 6, 0, reinterpret_cast<LPARAM>(task))) delete task;
  }).detach();
}
}  // namespace jisu
