#include "browser_window.h"
#include <algorithm>
namespace jisu {
void BrowserWindow::CancelBookmarkDrag() {
  if (!dragBookmarkId_ && !draggingBookmark_) return;
  dragBookmarkId_ = 0; draggingBookmark_ = false; dropBookmark_ = -1;
  dropIntoFolder_ = false; dropParentId_ = 0; pressed_ = {}; hover_ = {};
  if (GetCapture() == hwnd_) ReleaseCapture();
  SetCursor(LoadCursorW(nullptr, IDC_ARROW));
  InvalidateChrome();
}
void BrowserWindow::UpdateBookmarkDrop(POINT point) {
  dropBookmark_ = -1; dropIntoFolder_ = false; dropParentId_ = 0;
  const float x = point.x / layout_.scale, y = point.y / layout_.scale;
  const float top = layout_.tabStripHeight + layout_.toolbarHeight;
  if (x < 0 || x >= layout_.width || y < top || y >= top + layout_.bookmarkBarHeight) {
    SetCursor(LoadCursorW(nullptr, IDC_NO)); InvalidateChrome(); return;
  }
  const auto* source = bookmarks_.FindById(dragBookmarkId_);
  if (!source) { CancelBookmarkDrag(); return; }
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
void BrowserWindow::FinishBookmarkDrag(POINT point) {
  UpdateBookmarkDrop(point);
  const auto id=dragBookmarkId_, parent=dropParentId_; const auto index=dropIndex_;
  CancelBookmarkDrag();
  if (id && parent && bookmarks_.Move(id,parent,index)) OnBookmarksChanged();
}
}  // namespace jisu
