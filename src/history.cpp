#include "history.h"
#include "json.h"
#include <algorithm>
#include <chrono>
#include <cwctype>
namespace jisu {
namespace {
constexpr ULONGLONG kQuietMs = 1500;     // 最后一次改动后安静这么久再写
constexpr ULONGLONG kMaxDelayMs = 8000;  // 一直有改动时也至少这么久写一次
int64_t Now() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
std::wstring Lower(std::wstring s) { for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c)); return s; }
std::wstring SearchKey(const std::wstring& title, const std::wstring& url) { return Lower(title + L" " + url); }
json::Value Row(int64_t id, int64_t at, const std::wstring& url, const std::wstring& title) {
  auto row = json::Value::MakeObject();
  row.Set("id", json::Value::Number(static_cast<double>(id))); row.Set("at", json::Value::Number(static_cast<double>(at)));
  row.Set("url", json::Value::String(ToUtf8(url))); row.Set("title", json::Value::String(ToUtf8(title))); return row;
}
}
std::wstring HistoryStore::SafeUrl(std::wstring url) {
  if (url.size() > 8192 || !(url.starts_with(L"http://") || url.starts_with(L"https://")) ||
      url.starts_with(L"https://newtab.jisu.example/") || url.starts_with(L"https://favicons.jisu.example/")) return {};
  const auto authority = url.find(L"://") + 3, end = url.find_first_of(L"/?#", authority);
  if (url.substr(authority, end - authority).find(L'@') != std::wstring::npos) return {};
  // OAuth/SSO material is not a useful history entry; never persist its token-bearing URL.
  const auto lower = Lower(url);
  for (const auto* secret : {L"access_token=", L"id_token=", L"refresh_token=", L"authorization=", L"password=", L"session_token=", L"code="}) {
    auto p = lower.find(secret);
    if (p != std::wstring::npos && p > 0 && (lower[p-1] == L'?' || lower[p-1] == L'&' || lower[p-1] == L'#')) {
      url.resize(url.find_first_of(L"?#")); break;
    }
  }
  return url;
}
HistoryStore::~HistoryStore() {
  { std::lock_guard lock(mutex_); stop_ = true; } cv_.notify_all();
  if (worker_.joinable()) worker_.join();
}
void HistoryStore::Start(const std::wstring& file) {
  std::lock_guard lock(mutex_);
  if (worker_.joinable() || file.empty()) return;
  file_ = file; worker_ = std::thread([this] { Run(); });
}
void HistoryStore::Queue(Operation op) {
  { std::lock_guard lock(mutex_); if (!worker_.joinable() || pending_.size() >= 2048) return; pending_.push_back(std::move(op)); }
  cv_.notify_all();
}
void HistoryStore::Visit(std::wstring url, std::wstring title, int64_t now) {
  url = SafeUrl(std::move(url)); if (url.empty()) return;
  Queue({0, {0, now ? now : Now(), std::move(url), TruncateUtf16(std::move(title), 1024), {}}});
}
void HistoryStore::Title(const std::wstring& url, const std::wstring& title) {
  auto safe = SafeUrl(url);
  if (!safe.empty()) Queue({1, {0, 0, std::move(safe), TruncateUtf16(title, 1024), {}}});
}
void HistoryStore::Remove(int64_t id) { Queue({2, {id, 0, {}, {}, {}}}); }
void HistoryStore::Clear() { Queue({3, {}}); }
bool HistoryStore::WaitIdle(unsigned timeoutMs) {
  std::unique_lock lock(mutex_);
  return cv_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this] { return ready_ && pending_.empty() && !busy_; });
}
void HistoryStore::Flush() {
  std::unique_lock lock(mutex_);
  if (!worker_.joinable()) return;
  flushRequested_ = true;
  cv_.notify_all();
  cv_.wait_for(lock, std::chrono::milliseconds(3000), [this] { return stop_ || (ready_ && pending_.empty() && !busy_ && !dirty_); });
}
std::string HistoryStore::Query(std::wstring query, size_t offset, size_t limit) {
  std::lock_guard lock(mutex_); query = Lower(query); limit = (std::min)(limit, size_t(200));
  auto out = json::Value::MakeObject(), rows = json::Value::MakeArray(); size_t total = 0;
  for (const auto& e : entries_) {
    if (!query.empty() && e.search.find(query) == std::wstring::npos) continue;
    if (total++ >= offset && rows.Items().size() < limit) rows.Push(Row(e.id, e.at, e.url, e.title));
  }
  out.Set("type", json::Value::String("history.rows")); out.Set("loading", json::Value::Bool(!ready_ || !pending_.empty() || busy_));
  out.Set("query",json::Value::String(ToUtf8(query)));out.Set("error",json::Value::String(diskError_?"历史记录未能写入磁盘，请检查资料目录权限或磁盘空间":""));
  out.Set("total", json::Value::Number(static_cast<double>(total))); out.Set("rows", std::move(rows)); return json::Serialize(out);
}
void HistoryStore::Apply(Operation& op) {
  auto& n = op.entry;
  if (op.kind == 0) {
    auto old = std::find_if(entries_.begin(), entries_.end(), [&n](const auto& e) { return e.url == n.url && n.at >= e.at && n.at - e.at < 30000; });
    if (old != entries_.end()) { n.id = old->id; entries_.erase(old); } else n.id = nextId_++;
    n.search = SearchKey(n.title, n.url);
    entries_.insert(entries_.begin(), std::move(n));
  } else if (op.kind == 1) {
    auto it = std::find_if(entries_.begin(), entries_.end(), [&n](const auto& e) { return e.url == n.url; });
    if (it != entries_.end()) { it->title = std::move(n.title); it->search = SearchKey(it->title, it->url); }
  } else if (op.kind == 2) std::erase_if(entries_, [&n](const auto& e) { return e.id == n.id; });
  else if (op.kind == 3) entries_.clear();
}
// 持锁进入、持锁返回；序列化与写盘时放开锁
bool HistoryStore::SaveLocked(std::unique_lock<std::mutex>& lock) {
  const auto snapshot = entries_;
  const uint64_t generation = generation_;
  dirty_ = false; firstDirty_ = 0;
  lock.unlock();
  auto data = json::Value::MakeArray();
  for (const auto& e : snapshot) data.Push(Row(e.id, e.at, e.url, e.title));
  const bool saved = WriteFileAtomic(file_, json::Serialize(data));
  lock.lock();
  diskError_ = !saved;
  if (saved) {
    savedGeneration_ = std::max(savedGeneration_, generation);
  } else if (!stop_) {
    dirty_ = true;  // 写失败（磁盘满、被占用）：隔一会儿再试，不空转
    firstDirty_ = lastChange_ = GetTickCount64();
  }
  return saved;
}
void HistoryStore::Run() {
  std::string bytes; json::Value root;
  std::vector<Entry> loaded;
  if (ReadFileBytes(file_, bytes) && json::Parse(bytes, root)) {
    const int64_t now = Now();
    for (const auto& row : root.Items()) {
      auto url = SafeUrl(FromUtf8(row["url"].AsString()));
      const auto at = static_cast<int64_t>(row["at"].AsNumber());
      if (url.empty() || at < now - 90LL*86400000 || at > now + 86400000 || loaded.size() >= 20000) continue;
      auto title = TruncateUtf16(FromUtf8(row["title"].AsString()), 4096);
      auto search = SearchKey(title, url);
      loaded.push_back({nextId_++, at, std::move(url), std::move(title), std::move(search)});
    }
  }
  bytes.clear(); bytes.shrink_to_fit();
  std::unique_lock lock(mutex_); entries_ = std::move(loaded); ready_ = true; cv_.notify_all();
  for (;;) {
    if (!pending_.empty()) {
      busy_ = true;
      while (!pending_.empty()) { auto op = std::move(pending_.front()); pending_.pop_front(); Apply(op); }
      const int64_t now = Now();
      std::erase_if(entries_, [now](const auto& e) { return e.at < now - 90LL*86400000; });
      if (entries_.size() > 20000) entries_.resize(20000);
      size_t textSize=0;size_t keep=0;for(const auto& e:entries_){textSize+=e.url.size()+e.title.size();if(textSize>6*1024*1024)break;++keep;}if(entries_.size()>keep)entries_.resize(keep);
      ++generation_;
      lastChange_ = GetTickCount64();
      if (!dirty_) firstDirty_ = lastChange_;
      dirty_ = true;
      busy_ = false;
      cv_.notify_all();
      continue;
    }
    if (dirty_) {
      const ULONGLONG now = GetTickCount64();
      const ULONGLONG due = std::min(lastChange_ + kQuietMs, firstDirty_ + kMaxDelayMs);
      if (stop_ || flushRequested_ || now >= due) {
        flushRequested_ = false;
        SaveLocked(lock);
        cv_.notify_all();
        if (stop_ && !dirty_ && pending_.empty()) break;
        if (stop_ && dirty_) break;  // 停止时写失败也不再重试
        continue;
      }
      cv_.wait_for(lock, std::chrono::milliseconds(due - now), [this] { return stop_ || flushRequested_ || !pending_.empty(); });
      continue;
    }
    flushRequested_ = false;
    cv_.notify_all();
    if (stop_) break;
    cv_.wait(lock, [this] { return stop_ || flushRequested_ || !pending_.empty(); });
  }
}
}  // namespace jisu
