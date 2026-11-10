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
}  // namespace jisu
