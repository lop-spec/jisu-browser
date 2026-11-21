#include "browser_window.h"
#include "browser_url.h"
#include <ole2.h>
#include <algorithm>
namespace jisu {
void BrowserWindow::CancelBookmarkDrag() {
  if (!dragBookmarkId_ && !draggingBookmark_ && !externalBookmarkDrag_) return;
  dragBookmarkId_ = 0; draggingBookmark_ = false; externalBookmarkDrag_ = false; dropBookmark_ = -1;
  dropIntoFolder_ = false; dropParentId_ = 0; pressed_ = {}; hover_ = {};
  if (GetCapture() == hwnd_ && !dragTabId_) ReleaseCapture();
  SetCursor(LoadCursorW(nullptr, IDC_ARROW));
  InvalidateChrome();
}
void BrowserWindow::UpdateBookmarkDrop(POINT point) {
  dropBookmark_ = -1; dropIntoFolder_ = false; dropParentId_ = 0;
  const float x = point.x / layout_.scale, y = point.y / layout_.scale;
  const float top = layout_.bookmarks.empty() ? layout_.tabStripHeight + layout_.toolbarHeight : layout_.bookmarks.front().top;
  if (x < 0 || x >= layout_.width || y < top || y >= top + layout_.bookmarkBarHeight) {
    SetCursor(LoadCursorW(nullptr, IDC_NO)); InvalidateChrome(); return;
  }
  const auto* source = bookmarks_.FindById(dragBookmarkId_);
  if (dragBookmarkId_ && !source) { CancelBookmarkDrag(); return; }
  for (size_t i=0; i<barItems_.size(); ++i) {
    const auto& r=layout_.bookmarks[i];
    const auto* target=barItems_[i];
    if (x>=r.left+6 && x<r.right-6 && target->folder) {
      bool cycle=false;
      for (const auto* parent=target; parent; parent=parent->parent) if (parent==source) cycle=true;
      if (!cycle) { dropParentId_=target->id; dropIndex_=target->children.size(); dropBookmark_=static_cast<int>(i); dropIntoFolder_=true; }
      SetCursor(LoadCursorW(nullptr, cycle?IDC_NO:IDC_ARROW)); InvalidateChrome(); return;
    }
    if (x<(r.left+r.right)/2) {
      dropBookmark_=static_cast<int>(i); dropParentId_=bookmarks_.Bar().id;
      const auto& siblings=bookmarks_.Bar().children;
      const auto where=std::find_if(siblings.begin(),siblings.end(),[target](const auto& child){return child.get()==target;});
      dropIndex_=static_cast<size_t>(where-siblings.begin());
      SetCursor(LoadCursorW(nullptr, IDC_ARROW)); InvalidateChrome(); return;
    }
  }
  // End of the visible bar: before the first overflowing item, not after unseen bookmarks.
  if (!layout_.hasOverflow || x<layout_.bookmarkOverflow.left) {
    dropBookmark_=static_cast<int>(barItems_.size()); dropParentId_=bookmarks_.Bar().id; dropIndex_=barItems_.size();
  }
  SetCursor(LoadCursorW(nullptr, dropParentId_?IDC_ARROW:IDC_NO)); InvalidateChrome();
}
bool BrowserWindow::AddDroppedBookmark(const std::wstring& url,const std::wstring& title,POINT point) {
  if(!profileLoaded_||!settings_.bookmarkBar||url.size()>16384||!IsStarrable(url)||IsBlockedNavigationUrl(url)||FaviconOrigin(url).empty()||url.find_first_of(L" \t\r\n")!=std::wstring::npos)return false;
  externalBookmarkDrag_=true;UpdateBookmarkDrop(point);
  auto* parent=bookmarks_.FindById(dropParentId_);const auto index=dropIndex_;
  CancelBookmarkDrag();
  if(!parent||!parent->folder)return false;
  if(!bookmarks_.AddUrl(*parent,index,TruncateUtf16(title.empty()?url:title,512),url))return false;
  OnBookmarksChanged();return true;
}
void BrowserWindow::EnableBookmarkDrop() {
  if(destroyed_||options_.appMode||bookmarkDropTarget_)return;
  if(!dropOle_){if(FAILED(OleInitialize(nullptr)))return;dropOle_=true;}
  const auto alive=std::weak_ptr<int>(alive_);
  bookmarkDropTarget_=MakeUrlDropTarget([this,alive](POINTL screen,const std::wstring* url,bool finish){
    if(alive.expired()||destroyed_||!bookmarkDropTarget_)return false;
    if(finish&&!url){CancelBookmarkDrag();return false;}
    POINT point{screen.x,screen.y};ScreenToClient(hwnd_,&point);
    if(!profileLoaded_||!settings_.bookmarkBar){CancelBookmarkDrag();return false;}
    if(url)return AddDroppedBookmark(*url,*url,point);
    externalBookmarkDrag_=true;UpdateBookmarkDrop(point);return dropParentId_!=0;
  });
  if(!bookmarkDropTarget_||FAILED(RegisterDragDrop(hwnd_,bookmarkDropTarget_.Get())))bookmarkDropTarget_.Reset();
  else SetPropW(hwnd_,L"JisuBrowser.BookmarkDrop",reinterpret_cast<HANDLE>(1));
}

void BrowserWindow::FinishBookmarkDrag(POINT point) {
  UpdateBookmarkDrop(point);
  const auto id=dragBookmarkId_, parent=dropParentId_; const auto index=dropIndex_;
  CancelBookmarkDrag();
  if (id && parent && bookmarks_.Move(id,parent,index)) OnBookmarksChanged();
}

thread_local BrowserWindow* BrowserWindow::bookmarkMenuInput_ = nullptr;

LRESULT CALLBACK BrowserWindow::BookmarkMenuFilter(int code, WPARAM wParam, LPARAM lParam) {
  if (code == MSGF_MENU && bookmarkMenuInput_ && bookmarkMenuInput_->OnBookmarkMenuInput(*reinterpret_cast<const MSG*>(lParam)))
    return 1;
  return CallNextHookEx(nullptr, code, wParam, lParam);
}

bool BrowserWindow::OnBookmarkMenuInput(const MSG& msg) {
  if (!bookmarkMenu_ || destroyed_) return false;
  auto& state = *bookmarkMenu_;
  const auto rowAt = [&](HMENU menu, RECT* rect = nullptr) -> int64_t {
    const auto rows = state.rows.find(menu);
    if (rows == state.rows.end()) return 0;
    const int row = MenuItemFromPoint(nullptr, menu, msg.pt);
    if (row < 0 || static_cast<size_t>(row) >= rows->second.size()) return 0;
    if (rect && !GetMenuItemRect(nullptr, menu, row, rect)) return 0;
    return rows->second[row];
  };
  switch (msg.message) {
    case WM_KEYDOWN:
      if (msg.wParam == VK_ESCAPE && state.dragging) { EndMenu(); return true; }
      return false;
    case WM_LBUTTONDOWN:
    case WM_NCLBUTTONDOWN:
      state.dragged = 0;
      for (auto it = state.visibleMenus.rbegin(); it != state.visibleMenus.rend(); ++it) {
        if (const auto id = rowAt(*it)) {
          state.dragMenu = *it; state.dragged = id; state.start = msg.pt;
          break;
        }
      }
      return false;  // Windows still owns click, keyboard and submenu behavior.
    case WM_MOUSEMOVE:
    case WM_NCMOUSEMOVE:
    case WM_LBUTTONUP:
    case WM_NCLBUTTONUP: {
      const bool release = msg.message == WM_LBUTTONUP || msg.message == WM_NCLBUTTONUP;
      if (!state.dragged) return false;
      if (!state.dragging && !release) {
        state.dragging = std::abs(msg.pt.x - state.start.x) >= GetSystemMetricsForDpi(SM_CXDRAG, dpi_) ||
                         std::abs(msg.pt.y - state.start.y) >= GetSystemMetricsForDpi(SM_CYDRAG, dpi_);
      }
      if (!state.dragging) { if (release) state.dragged = 0; return false; }
      RECT rect{};
      const auto target = rowAt(state.dragMenu, &rect);
      state.after = msg.pt.y >= (rect.top + rect.bottom) / 2;
      if (release) {
        state.target = target != state.dragged ? target : 0;
        EndMenu();
        return true;
      }
      if (target && target != state.dragged) {
        if (!state.marker) state.marker = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT,
            L"STATIC", nullptr, WS_POPUP | WS_DISABLED | SS_BLACKRECT, 0, 0, 0, 0, hwnd_, nullptr, instance_, nullptr);
        if (state.marker) {
          const int h = std::max(2, MulDiv(2, dpi_, 96));
          SetWindowPos(state.marker, HWND_TOPMOST, rect.left + 4, (state.after ? rect.bottom : rect.top) - h / 2,
                       std::max(1L, rect.right - rect.left - 8), h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        }
      } else if (state.marker) ShowWindow(state.marker, SW_HIDE);
      return true;  // Do not open another submenu while dragging.
    }
    default:
      return false;
  }
}

void BrowserWindow::CommitBookmarkMenuDrop(const BookmarkMenuState& state) {
  const auto* source = bookmarks_.FindById(state.dragged);
  const auto* target = bookmarks_.FindById(state.target);
  if (!source || !target || !target->parent || source->parent != target->parent) return;
  const auto& siblings = target->parent->children;
  const auto at = std::find_if(siblings.begin(), siblings.end(), [target](const auto& sibling) { return sibling.get() == target; });
  if (at == siblings.end()) return;
  const size_t slot = static_cast<size_t>(at - siblings.begin()) + (state.after ? 1 : 0);
  if (bookmarks_.Move(source->id, target->parent->id, slot)) OnBookmarksChanged();
}
}  // namespace jisu
