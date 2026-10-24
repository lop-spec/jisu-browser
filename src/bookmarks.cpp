#include "bookmarks.h"

#include "json.h"

#include <objbase.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <set>

namespace jisu {
namespace {

// Chromium 三个根目录的固定 GUID（bookmark_node.cc），保持一致才能被其他浏览器原样读回
constexpr char kBarGuid[] = "0bc5d13f-2cba-5d74-951f-3f233fe6c908";
constexpr char kOtherGuid[] = "82b081ec-3dd3-529c-8475-ab6c344590dd";
constexpr char kMobileGuid[] = "4cf2e351-0e85-532b-bb37-df045d8f8d0f";

std::string NewGuid() {
  GUID guid{};
  if (FAILED(CoCreateGuid(&guid))) {
    return {};
  }
  char text[40];
  snprintf(text, sizeof(text), "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", guid.Data1, guid.Data2,
           guid.Data3, guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3], guid.Data4[4], guid.Data4[5],
           guid.Data4[6], guid.Data4[7]);
  return text;
}

int64_t ParseInt(const json::Value& value) {
  if (value.IsNumber()) {
    return static_cast<int64_t>(value.AsNumber());
  }
  return _strtoi64(value.AsString().c_str(), nullptr, 10);
}

void AppendField(std::string& out, const char* key, const std::string& value, bool comma = true) {
  out += '"';
  out += key;
  out += "\":";
  json::AppendQuoted(out, value);
  if (comma) {
    out += ',';
  }
}

void SerializeNode(std::string& out, const BookmarkNode& node) {
  out += '{';
  if (node.folder) {
    out += "\"children\":[";
    for (size_t i = 0; i < node.children.size(); ++i) {
      if (i) {
        out += ',';
      }
      SerializeNode(out, *node.children[i]);
    }
    out += "],";
  }
  AppendField(out, "date_added", std::to_string(node.dateAdded));
  AppendField(out, "date_last_used", "0");
  if (node.folder) {
    AppendField(out, "date_modified", std::to_string(node.dateModified));
  }
  AppendField(out, "guid", node.guid);
  AppendField(out, "id", std::to_string(node.id));
  AppendField(out, "name", ToUtf8(node.title));
  AppendField(out, "type", node.folder ? "folder" : "url", !node.folder);
  if (!node.folder) {
    AppendField(out, "url", ToUtf8(node.url), false);
  }
  out += '}';
}

void TreeNodeJson(std::string& out, const BookmarkNode& node) {
  out += "{\"id\":" + std::to_string(node.id) + ",\"title\":";
  json::AppendQuoted(out, ToUtf8(node.title));
  if (node.folder) {
    out += ",\"children\":[";
    for (size_t i = 0; i < node.children.size(); ++i) {
      if (i) {
        out += ',';
      }
      TreeNodeJson(out, *node.children[i]);
    }
    out += ']';
  } else {
    out += ",\"url\":";
    json::AppendQuoted(out, ToUtf8(node.url));
  }
  out += ",\"added\":" + std::to_string(node.dateAdded / 1000 - 11644473600000LL);  // Unix 毫秒
  out += '}';
}

void AppendHtmlEscaped(std::string& out, const std::string& text) {
  for (const char c : text) {
    switch (c) {
      case '&':
        out += "&amp;";
        break;
      case '<':
        out += "&lt;";
        break;
      case '>':
        out += "&gt;";
        break;
      case '"':
        out += "&quot;";
        break;
      default:
        out.push_back(c);
    }
  }
}

void HtmlNode(std::string& out, const BookmarkNode& node, int depth, bool toolbar) {
  const std::string indent(static_cast<size_t>(depth) * 4, ' ');
  const long long added = node.dateAdded / 1000000 - 11644473600LL;  // Unix 秒
  if (node.folder) {
    out += indent + "<DT><H3 ADD_DATE=\"" + std::to_string(added) + "\"";
    if (toolbar) {
      out += " PERSONAL_TOOLBAR_FOLDER=\"true\"";
    }
    out += ">";
    AppendHtmlEscaped(out, ToUtf8(node.title));
    out += "</H3>\n" + indent + "<DL><p>\n";
    for (const auto& child : node.children) {
      HtmlNode(out, *child, depth + 1, false);
    }
    out += indent + "</DL><p>\n";
  } else {
    out += indent + "<DT><A HREF=\"";
    AppendHtmlEscaped(out, ToUtf8(node.url));
    out += "\" ADD_DATE=\"" + std::to_string(added) + "\">";
    AppendHtmlEscaped(out, ToUtf8(node.title));
    out += "</A>\n";
  }
}

std::wstring LocalAppData() {
  wchar_t buffer[MAX_PATH];
  const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, MAX_PATH);
  return length && length < MAX_PATH ? std::wstring(buffer, length) : std::wstring();
}

int CountUrls(const json::Value& node) {
  if (node["type"].AsString() == "url") {
    return 1;
  }
  int count = 0;
  for (const json::Value& child : node["children"].Items()) {
    count += CountUrls(child);
  }
  return count;
}

bool ReadChromiumBookmarks(const std::wstring& file, json::Value& roots, std::string* error) {
  std::string text;
  if (!ReadFileBytes(file, text)) {
    if (error) {
      *error = "cannot read bookmarks file";
    }
    return false;
  }
  json::Value root;
  if (!json::Parse(text, root, error)) {
    return false;
  }
  roots = root["roots"];
  if (!roots.IsObject()) {
    if (error) {
      *error = "not a Chromium bookmarks file";
    }
    return false;
  }
  return true;
}

struct KnownBrowser {
  const wchar_t* name;
  const wchar_t* dataDir;  // 相对 %LOCALAPPDATA%
  const wchar_t* progIdPrefix;
};

constexpr KnownBrowser kKnownBrowsers[] = {
    {L"Quark", L"\\Quark\\User Data", L"QuarkHTM"},
    {L"Thorium", L"\\Thorium\\User Data", L"ThoriumHTM"},
    {L"Chrome", L"\\Google\\Chrome\\User Data", L"ChromeHTML"},
    {L"Edge", L"\\Microsoft\\Edge\\User Data", L"MSEdgeHTM"},
    {L"Brave", L"\\BraveSoftware\\Brave-Browser\\User Data", L"BraveHTML"},
    {L"Vivaldi", L"\\Vivaldi\\User Data", L"VivaldiHTM"},
    {L"QQ 浏览器", L"\\Tencent\\QQBrowser\\User Data", L"QQBrowser"},
    {L"360 极速浏览器", L"\\360ChromeX\\Chrome\\User Data", L"360ChromeX"},
};

constexpr const wchar_t* kProfiles[] = {L"Default", L"Profile 1", L"Profile 2", L"Profile 3"};

}  // namespace

int64_t ChromiumNow() {
  FILETIME now{};
  GetSystemTimeAsFileTime(&now);
  const ULONGLONG ticks = (static_cast<ULONGLONG>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
  return static_cast<int64_t>(ticks / 10);
}

BookmarkStore::BookmarkStore()
    : bar_(MakeRoot(1, L"书签栏", kBarGuid)),
      other_(MakeRoot(2, L"其他书签", kOtherGuid)),
      mobile_(MakeRoot(3, L"移动设备书签", kMobileGuid)) {}

std::unique_ptr<BookmarkNode> BookmarkStore::MakeRoot(int64_t id, const wchar_t* title, const char* guid) {
  auto node = std::make_unique<BookmarkNode>();
  node->id = id;
  node->folder = true;
  node->title = title;
  node->guid = guid;
  node->dateAdded = ChromiumNow();
  return node;
}

bool BookmarkStore::Load(const std::wstring& file) {
  file_ = file;
  json::Value roots;
  if (!ReadChromiumBookmarks(file, roots, nullptr)) {
    existed_ = GetFileAttributesW(file.c_str()) != INVALID_FILE_ATTRIBUTES;
    return !existed_;  // 不存在算成功（空书签）；存在却读不了算失败，此时不覆盖它
  }
  existed_ = true;
  int64_t maxId = 3;
  // 递归读入；id 沿用文件里的（便于和其他浏览器对照），缺失或重复时重新分配
  std::set<int64_t> seen;
  auto readChildren = [&](auto&& self, const json::Value& source, BookmarkNode& parent) -> void {
    for (const json::Value& item : source["children"].Items()) {
      const std::string& type = item["type"].AsString();
      if (type != "url" && type != "folder") {
        continue;
      }
      auto node = std::make_unique<BookmarkNode>();
      node->folder = type == "folder";
      node->id = ParseInt(item["id"]);
      node->title = FromUtf8(item["name"].AsString());
      node->url = FromUtf8(item["url"].AsString());
      node->guid = item["guid"].AsString();
      node->dateAdded = ParseInt(item["date_added"]);
      node->dateModified = ParseInt(item["date_modified"]);
      node->parent = &parent;
      if (node->id > maxId) {
        maxId = node->id;
      }
      if (node->folder) {
        self(self, item, *node);
      }
      parent.children.push_back(std::move(node));
    }
  };
  readChildren(readChildren, roots["bookmark_bar"], *bar_);
  readChildren(readChildren, roots["other"], *other_);
  readChildren(readChildren, roots["synced"], *mobile_);
  nextId_ = maxId + 1;
  // 修补 id 冲突与缺失的 GUID
  auto fix = [&](auto&& self, BookmarkNode& parent) -> void {
    for (auto& child : parent.children) {
      if (child->id <= 3 || seen.count(child->id)) {
        child->id = NextId();
      }
      seen.insert(child->id);
      if (child->guid.empty()) {
        child->guid = NewGuid();
      }
      if (child->folder) {
        self(self, *child);
      }
    }
  };
  fix(fix, *bar_);
  fix(fix, *other_);
  fix(fix, *mobile_);
  return true;
}

bool BookmarkStore::Save() {
  if (file_.empty()) {
    return false;
  }
  std::string out = "{\"roots\":{\"bookmark_bar\":";
  SerializeNode(out, *bar_);
  out += ",\"other\":";
  SerializeNode(out, *other_);
  out += ",\"synced\":";
  SerializeNode(out, *mobile_);
  out += "},\"version\":1}";
  EnsureDirectory(ParentDirectory(file_));
  const bool ok = WriteFileAtomic(file_, out);
  existed_ = existed_ || ok;
  return ok;
}

BookmarkNode* BookmarkStore::FindById(int64_t id) {
  auto find = [id](auto&& self, BookmarkNode& node) -> BookmarkNode* {
    if (node.id == id) {
      return &node;
    }
    for (auto& child : node.children) {
      if (BookmarkNode* found = self(self, *child)) {
        return found;
      }
    }
    return nullptr;
  };
  for (BookmarkNode* root : {bar_.get(), other_.get(), mobile_.get()}) {
    if (BookmarkNode* found = find(find, *root)) {
      return found;
    }
  }
  return nullptr;
}

BookmarkNode* BookmarkStore::FindByUrl(const std::wstring& url) {
  if (url.empty()) {
    return nullptr;
  }
  auto find = [&url](auto&& self, BookmarkNode& node) -> BookmarkNode* {
    if (!node.folder && node.url == url) {
      return &node;
    }
    for (auto& child : node.children) {
      if (BookmarkNode* found = self(self, *child)) {
        return found;
      }
    }
    return nullptr;
  };
  for (BookmarkNode* root : {bar_.get(), other_.get(), mobile_.get()}) {
    if (BookmarkNode* found = find(find, *root)) {
      return found;
    }
  }
  return nullptr;
}

bool BookmarkStore::IsRoot(const BookmarkNode& node) const {
  return &node == bar_.get() || &node == other_.get() || &node == mobile_.get();
}

void BookmarkStore::Touch() { ++revision_; }

BookmarkNode* BookmarkStore::AddUrl(BookmarkNode& parent, size_t index, const std::wstring& title,
                                    const std::wstring& url) {
  if (!parent.folder) {
    return nullptr;
  }
  auto node = std::make_unique<BookmarkNode>();
  node->id = NextId();
  node->title = title;
  node->url = url;
  node->guid = NewGuid();
  node->dateAdded = ChromiumNow();
  node->parent = &parent;
  BookmarkNode* raw = node.get();
  index = std::min(index, parent.children.size());
  parent.children.insert(parent.children.begin() + static_cast<std::ptrdiff_t>(index), std::move(node));
  parent.dateModified = raw->dateAdded;
  Touch();
  return raw;
}

BookmarkNode* BookmarkStore::AddFolder(BookmarkNode& parent, size_t index, const std::wstring& title) {
  if (!parent.folder) {
    return nullptr;
  }
  auto node = std::make_unique<BookmarkNode>();
  node->id = NextId();
  node->folder = true;
  node->title = title;
  node->guid = NewGuid();
  node->dateAdded = ChromiumNow();
  node->parent = &parent;
  BookmarkNode* raw = node.get();
  index = std::min(index, parent.children.size());
  parent.children.insert(parent.children.begin() + static_cast<std::ptrdiff_t>(index), std::move(node));
  parent.dateModified = raw->dateAdded;
  Touch();
  return raw;
}

bool BookmarkStore::Remove(int64_t id) {
  BookmarkNode* node = FindById(id);
  if (!node || IsRoot(*node) || !node->parent) {
    return false;
  }
  auto& siblings = node->parent->children;
  for (auto it = siblings.begin(); it != siblings.end(); ++it) {
    if (it->get() == node) {
      node->parent->dateModified = ChromiumNow();
      siblings.erase(it);
      Touch();
      return true;
    }
  }
  return false;
}

bool BookmarkStore::Rename(int64_t id, const std::wstring& title) {
  BookmarkNode* node = FindById(id);
  if (!node || IsRoot(*node)) {
    return false;
  }
  node->title = title;
  Touch();
  return true;
}

bool BookmarkStore::SetUrl(int64_t id, const std::wstring& url) {
  BookmarkNode* node = FindById(id);
  if (!node || node->folder || url.empty()) {
    return false;
  }
  node->url = url;
  Touch();
  return true;
}

bool BookmarkStore::Move(int64_t id, int64_t newParentId, size_t index) {
  BookmarkNode* node = FindById(id);
  BookmarkNode* target = FindById(newParentId);
  if (!node || !target || IsRoot(*node) || !target->folder || !node->parent) {
    return false;
  }
  for (const BookmarkNode* walk = target; walk; walk = walk->parent) {
    if (walk == node) {
      return false;  // 不能移进自己的子孙
    }
  }
  auto& siblings = node->parent->children;
  std::unique_ptr<BookmarkNode> owned;
  size_t oldIndex = 0;
  for (auto it = siblings.begin(); it != siblings.end(); ++it, ++oldIndex) {
    if (it->get() == node) {
      owned = std::move(*it);
      siblings.erase(it);
      break;
    }
  }
  if (!owned) {
    return false;
  }
  // 同一文件夹内往后挪：先移除再插入，目标下标要减一
  if (node->parent == target && index > oldIndex) {
    --index;
  }
  owned->parent = target;
  index = std::min(index, target->children.size());
  target->children.insert(target->children.begin() + static_cast<std::ptrdiff_t>(index), std::move(owned));
  target->dateModified = ChromiumNow();
  Touch();
  return true;
}

int BookmarkStore::Import(const std::wstring& file, const std::wstring& sourceName, std::string* error) {
  json::Value roots;
  if (!ReadChromiumBookmarks(file, roots, error)) {
    return -1;
  }
  int count = 0;
  auto copy = [&](auto&& self, const json::Value& source, BookmarkNode& parent) -> void {
    for (const json::Value& item : source["children"].Items()) {
      const std::string& type = item["type"].AsString();
      const std::wstring title = FromUtf8(item["name"].AsString());
      if (type == "url") {
        const std::wstring url = FromUtf8(item["url"].AsString());
        if (url.empty()) {
          continue;
        }
        if (BookmarkNode* node = AddUrl(parent, parent.children.size(), title, url)) {
          const int64_t added = ParseInt(item["date_added"]);
          if (added > 0) {
            node->dateAdded = added;
          }
          ++count;
        }
      } else if (type == "folder") {
        if (BookmarkNode* folder = AddFolder(parent, parent.children.size(), title)) {
          self(self, item, *folder);
        }
      }
    }
  };
  const bool fresh = bar_->children.empty() && other_->children.empty() && mobile_->children.empty();
  if (fresh) {
    copy(copy, roots["bookmark_bar"], *bar_);
    copy(copy, roots["other"], *other_);
    copy(copy, roots["synced"], *mobile_);
  } else {
    BookmarkNode* target = AddFolder(*bar_, bar_->children.size(), L"从 " + sourceName + L" 导入");
    copy(copy, roots["bookmark_bar"], *target);
    if (!roots["other"]["children"].Items().empty()) {
      copy(copy, roots["other"], *AddFolder(*target, target->children.size(), L"其他书签"));
    }
    if (!roots["synced"]["children"].Items().empty()) {
      copy(copy, roots["synced"], *AddFolder(*target, target->children.size(), L"移动设备书签"));
    }
  }
  Touch();
  if (!Save() && error) {
    *error = "imported but could not save";
  }
  return count;
}

std::vector<BookmarkImportSource> BookmarkStore::DetectImportSources() {
  std::vector<BookmarkImportSource> sources;
  const std::wstring base = LocalAppData();
  if (base.empty()) {
    return sources;
  }
  for (const KnownBrowser& browser : kKnownBrowsers) {
    for (const wchar_t* profile : kProfiles) {
      const std::wstring file = base + browser.dataDir + L"\\" + profile + L"\\Bookmarks";
      json::Value roots;
      if (!ReadChromiumBookmarks(file, roots, nullptr)) {
        continue;
      }
      BookmarkImportSource source;
      source.name = browser.name;
      if (wcscmp(profile, L"Default") != 0) {
        source.name += std::wstring(L"（") + profile + L"）";
      }
      source.file = file;
      source.urls = CountUrls(roots["bookmark_bar"]) + CountUrls(roots["other"]) + CountUrls(roots["synced"]);
      sources.push_back(std::move(source));
    }
  }
  return sources;
}

std::wstring BookmarkStore::DefaultBrowserBookmarksFile(std::wstring* name) {
  wchar_t progId[128] = {};
  DWORD size = sizeof(progId);
  if (RegGetValueW(HKEY_CURRENT_USER,
                   L"Software\\Microsoft\\Windows\\Shell\\Associations\\UrlAssociations\\https\\UserChoice",
                   L"ProgId", RRF_RT_REG_SZ, nullptr, progId, &size) != ERROR_SUCCESS) {
    return {};
  }
  const std::wstring base = LocalAppData();
  for (const KnownBrowser& browser : kKnownBrowsers) {
    if (wcsncmp(progId, browser.progIdPrefix, wcslen(browser.progIdPrefix)) != 0) {
      continue;
    }
    const std::wstring file = base + browser.dataDir + L"\\Default\\Bookmarks";
    if (GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES) {
      return {};
    }
    if (name) {
      *name = browser.name;
    }
    return file;
  }
  return {};
}

std::string BookmarkStore::TreeJson() const {
  std::string out = "{\"revision\":" + std::to_string(revision_) + ",\"bar\":";
  TreeNodeJson(out, *bar_);
  out += ",\"other\":";
  TreeNodeJson(out, *other_);
  out += ",\"mobile\":";
  TreeNodeJson(out, *mobile_);
  out += '}';
  return out;
}

std::string BookmarkStore::ExportHtml() const {
  std::string out =
      "<!DOCTYPE NETSCAPE-Bookmark-file-1>\n"
      "<!-- This is an automatically generated file.\n     It will be read and overwritten.\n     DO NOT EDIT! -->\n"
      "<META HTTP-EQUIV=\"Content-Type\" CONTENT=\"text/html; charset=UTF-8\">\n"
      "<TITLE>Bookmarks</TITLE>\n<H1>Bookmarks</H1>\n<DL><p>\n";
  HtmlNode(out, *bar_, 1, true);
  HtmlNode(out, *other_, 1, false);
  if (!mobile_->children.empty()) {
    HtmlNode(out, *mobile_, 1, false);
  }
  out += "</DL><p>\n";
  return out;
}

}  // namespace jisu
