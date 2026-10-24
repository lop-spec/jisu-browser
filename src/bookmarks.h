// 书签：与 Chromium 的 Bookmarks 文件同一格式（roots.bookmark_bar / other / synced），可直接从 Quark、Thorium、
// Chrome、Edge 等导入，也能被它们读回。文件在用户数据目录下，改动后整份原子写回。
#pragma once

#include "common.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace jisu {

struct BookmarkNode {
  int64_t id = 0;
  bool folder = false;
  std::wstring title;
  std::wstring url;
  std::string guid;
  int64_t dateAdded = 0;  // Chromium 时间：1601-01-01 起的微秒
  int64_t dateModified = 0;
  BookmarkNode* parent = nullptr;
  std::vector<std::unique_ptr<BookmarkNode>> children;
};

struct BookmarkImportSource {
  std::wstring name;  // 显示名，例如「Quark」
  std::wstring file;  // Bookmarks 文件路径
  int urls = 0;
};

class BookmarkStore {
 public:
  BookmarkStore();

  // 读入书签文件；文件不存在时得到三个空根目录。可在后台线程调用（之后只在 UI 线程用）
  bool Load(const std::wstring& file);
  bool Save();
  const std::wstring& File() const { return file_; }
  bool Existed() const { return existed_; }

  BookmarkNode& Bar() { return *bar_; }
  BookmarkNode& Other() { return *other_; }
  BookmarkNode& Mobile() { return *mobile_; }
  BookmarkNode* FindById(int64_t id);
  BookmarkNode* FindByUrl(const std::wstring& url);
  bool IsRoot(const BookmarkNode& node) const;
  uint64_t Revision() const { return revision_; }

  BookmarkNode* AddUrl(BookmarkNode& parent, size_t index, const std::wstring& title, const std::wstring& url);
  BookmarkNode* AddFolder(BookmarkNode& parent, size_t index, const std::wstring& title);
  bool Remove(int64_t id);
  bool Rename(int64_t id, const std::wstring& title);
  bool SetUrl(int64_t id, const std::wstring& url);
  // 移到 newParent 的 index 位置（index 超出则放末尾）；不能移进自己的子孙
  bool Move(int64_t id, int64_t newParentId, size_t index);

  // 从 Chromium 系书签文件导入：本机书签栏为空时直接放进书签栏，否则放进书签栏里的「从 <名称> 导入」文件夹
  int Import(const std::wstring& file, const std::wstring& sourceName, std::string* error);
  static std::vector<BookmarkImportSource> DetectImportSources();
  // 系统默认浏览器对应的导入源（不是 Chromium 系或没有书签文件时为空）
  static std::wstring DefaultBrowserBookmarksFile(std::wstring* name);

  // 书签管理页用：整棵树（id、标题、网址、子项）
  std::string TreeJson() const;
  // Netscape 书签 HTML，各浏览器都能导入
  std::string ExportHtml() const;

 private:
  std::unique_ptr<BookmarkNode> MakeRoot(int64_t id, const wchar_t* title, const char* guid);
  void Touch();
  int64_t NextId() { return nextId_++; }

  std::wstring file_;
  bool existed_ = false;
  std::unique_ptr<BookmarkNode> bar_;
  std::unique_ptr<BookmarkNode> other_;
  std::unique_ptr<BookmarkNode> mobile_;
  int64_t nextId_ = 4;
  uint64_t revision_ = 1;
};

int64_t ChromiumNow();

}  // namespace jisu
