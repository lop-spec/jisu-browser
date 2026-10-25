#include "history.h"
#include "json.h"
#include <algorithm>
#include <chrono>
#include <cwctype>
namespace jisu {
namespace {
int64_t Now() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
std::wstring Lower(std::wstring s) { for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c)); return s; }
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
  title.resize((std::min)(title.size(), size_t(1024)));
  Queue({0, {0, now ? now : Now(), std::move(url), std::move(title)}});
}
void HistoryStore::Title(const std::wstring& url, const std::wstring& title) {
  if (title.size() <= 1024) Queue({1, {0, 0, SafeUrl(url), title}});
}
void HistoryStore::Remove(int64_t id) { Queue({2, {id, 0, {}, {}}}); }
void HistoryStore::Clear() { Queue({3, {}}); }
bool HistoryStore::WaitIdle(unsigned timeoutMs) {
  std::unique_lock lock(mutex_);
  return cv_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this] { return ready_ && pending_.empty() && !busy_; });
}
std::string HistoryStore::Query(std::wstring query, size_t offset, size_t limit) {
  std::lock_guard lock(mutex_); query = Lower(query); limit = (std::min)(limit, size_t(200));
  auto out = json::Value::MakeObject(), rows = json::Value::MakeArray(); size_t total = 0;
  for (const auto& e : entries_) {
    if (!query.empty() && Lower(e.title + L" " + e.url).find(query) == std::wstring::npos) continue;
    if (total++ >= offset && rows.Items().size() < limit) rows.Push(Row(e.id, e.at, e.url, e.title));
  }
  out.Set("type", json::Value::String("history.rows")); out.Set("loading", json::Value::Bool(!ready_ || !pending_.empty() || busy_));
  out.Set("total", json::Value::Number(static_cast<double>(total))); out.Set("rows", std::move(rows)); return json::Serialize(out);
}
void HistoryStore::Run() {
  std::string bytes; json::Value root;
  std::vector<Entry> loaded;
  if (ReadFileBytes(file_, bytes) && json::Parse(bytes, root)) {
    for (const auto& row : root.Items()) {
      auto url = SafeUrl(FromUtf8(row["url"].AsString()));
      const auto at = static_cast<int64_t>(row["at"].AsNumber());
      if (url.empty() || at < Now() - 90LL*86400000 || at > Now() + 86400000 || loaded.size() >= 20000) continue;
      loaded.push_back({nextId_++, at, std::move(url), FromUtf8(row["title"].AsString().substr(0,4096))});
    }
  }
  std::unique_lock lock(mutex_); entries_ = std::move(loaded); ready_ = true; cv_.notify_all();
  for (;;) {
    cv_.wait(lock, [this] { return stop_ || !pending_.empty(); });
    if (pending_.empty() && stop_) break;
    busy_ = true;
    // Batch visits/title updates, without holding the lock during disk I/O.
    cv_.wait_for(lock, std::chrono::milliseconds(200), [this] { return stop_; });
    while (!pending_.empty()) {
      auto op = std::move(pending_.front()); pending_.pop_front(); auto& n = op.entry;
      if (op.kind == 0) {
        auto old = std::find_if(entries_.begin(), entries_.end(), [&n](const auto& e) { return e.url == n.url && n.at >= e.at && n.at - e.at < 30000; });
        if (old != entries_.end()) { n.id = old->id; entries_.erase(old); } else n.id = nextId_++;
        entries_.insert(entries_.begin(), std::move(n));
      } else if (op.kind == 1) {
        auto it = std::find_if(entries_.begin(), entries_.end(), [&n](const auto& e) { return e.url == n.url; });
        if (it != entries_.end()) it->title = std::move(n.title);
      } else if (op.kind == 2) std::erase_if(entries_, [&n](const auto& e) { return e.id == n.id; });
      else if (op.kind == 3) entries_.clear();
    }
    std::erase_if(entries_, [](const auto& e) { return e.at < Now() - 90LL*86400000; });
    if (entries_.size() > 20000) entries_.resize(20000);
    const auto snapshot = entries_; lock.unlock();
    auto data = json::Value::MakeArray(); for (const auto& e : snapshot) data.Push(Row(e.id, e.at, e.url, e.title));
    bytes = json::Serialize(data); WriteFileAtomic(file_, bytes); lock.lock();
    busy_ = false; cv_.notify_all();
  }
}
}  // namespace jisu
