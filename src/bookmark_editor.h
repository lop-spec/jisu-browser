// Modal bookmark editor. Folder creation is staged: Cancel never mutates BookmarkStore.
#pragma once
#include "bookmarks.h"
#include <optional>
#include <functional>
namespace jisu {
struct BookmarkFolderChoice { int64_t id = 0, parent = 0; std::wstring path, title; bool draft = false; };
struct BookmarkEdit {
  std::wstring title, url;
  int64_t parent = 1;
  bool remove = false;
  std::vector<BookmarkFolderChoice> folders;
};
std::optional<BookmarkEdit> ShowBookmarkEditor(HWND owner, HINSTANCE instance, BookmarkStore& store,
  const std::wstring& title, const std::wstring& url, int64_t parent, bool existing,
  const std::function<void(HWND)>& ready = {});
// Applies an accepted editor result. Returns a bookmark id, or 0 if the parent vanished.
int64_t ApplyBookmarkEdit(BookmarkStore& store, const BookmarkEdit& edit, int64_t existingId = 0);
}  // namespace jisu
