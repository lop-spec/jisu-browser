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
std::wstring DecodeUrl(const std::wstring& input) {
  const auto bytes = ToUtf8(input); std::string out;
  auto hex = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
  for (size_t i = 0; i < bytes.size(); ++i) {
    if (bytes[i] == '%' && i + 2 < bytes.size() && hex(bytes[i+1]) >= 0 && hex(bytes[i+2]) >= 0) {
      out.push_back(static_cast<char>(hex(bytes[i+1])*16 + hex(bytes[i+2]))); i += 2;
    } else out.push_back(bytes[i] == '+' ? ' ' : bytes[i]);
  }
  return FromUtf8(out);
}
std::wstring SearchKey(const std::wstring& title, const std::wstring& url) { return Lower(title + L" " + url + L" " + DecodeUrl(url)); }
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
  // code alone is a normal business parameter. Treat it as OAuth only with callback/state evidence.
  const auto lower = Lower(DecodeUrl(url));
  const bool oauth = lower.find(L"state=") != std::wstring::npos || lower.find(L"/callback") != std::wstring::npos ||
                     lower.find(L"/oauth") != std::wstring::npos || lower.find(L"/authorize") != std::wstring::npos;
  const auto start = url.find_first_of(L"?#");
  if (start == std::wstring::npos) return url;
  std::wstring out = url.substr(0, start);
  size_t pos = start;
  while (pos < url.size()) {
    const wchar_t delimiter = url[pos++];
    const auto stop = url.find_first_of(L"?&#", pos);
    const auto part = url.substr(pos, stop == std::wstring::npos ? stop : stop - pos);
    auto key = Lower(DecodeUrl(part.substr(0, part.find(L'='))));
    const bool secret = key == L"access_token" || key == L"id_token" || key == L"refresh_token" ||
        key == L"authorization" || key == L"password" || key == L"session_token" || key == L"token" ||
        key == L"api_key" || key == L"apikey" || key == L"secret" || key == L"signature" || key == L"sig" ||
        key == L"x-amz-signature" || key == L"x-oss-signature" || key == L"security-token" || (oauth && (key == L"code" || key == L"state"));
    if (!secret && !part.empty()) {
      wchar_t sep = delimiter;
      if (sep == L'&' && out.find(L'?') == std::wstring::npos) sep = L'?';
      if (sep == L'&' && out.find(L'#') != std::wstring::npos) sep = L'&';
      out.push_back(sep); out += part;
    }
    if (stop == std::wstring::npos) break;
    pos = stop;
  }
  return out;
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
  { std::lock_guard lock(mutex_); if (!worker_.joinable()) return;
    // Coalesce only adjacent operations: a remove/clear remains an ordering barrier.
    if (memoryOnly_) return;
    if (!pending_.empty() && pending_.back().kind == op.kind && pending_.back().entry.url == op.entry.url && op.kind <= 1) {
      auto& previous = pending_.back().entry;
      if (op.kind == 0) { op.entry.visits += previous.visits; op.entry.typed += previous.typed; if (op.entry.title.empty()) op.entry.title = previous.title; }
      previous = std::move(op.entry);
    } else pending_.push_back(std::move(op)); }
  cv_.notify_all();
}
void HistoryStore::Visit(std::wstring url, std::wstring title, int64_t now, bool typed) {
  url = SafeUrl(std::move(url)); if (url.empty()) return;
  Queue({0, {0, now ? now : Now(), std::move(url), TruncateUtf16(std::move(title), 1024), {}, 1, typed ? 1ULL : 0ULL}});
}
void HistoryStore::Title(const std::wstring& url, const std::wstring& title) {
  auto safe = SafeUrl(url);
  if (!safe.empty()) Queue({1, {0, 0, std::move(safe), TruncateUtf16(title, 1024), {}}});
}
void HistoryStore::Remove(int64_t id) { Queue({2, {id, 0, {}, {}, {}}}); }
void HistoryStore::Clear() { Queue({3, {}}); }
void HistoryStore::RemoveRange(std::wstring origin, int64_t start, int64_t end) {
  if (start < 0 || end <= start) return;
  origin = Lower(std::move(origin));
  if (!origin.empty() && !(origin.starts_with(L"https://") || origin.starts_with(L"http://"))) return;
  Queue({4, {end, start, std::move(origin), {}, {}}});
}
bool HistoryStore::WaitIdle(unsigned timeoutMs) {
  std::unique_lock lock(mutex_);
  return cv_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this] { return ready_ && pending_.empty() && !busy_; });
}
void HistoryStore::Flush(unsigned timeoutMs) {
  std::unique_lock lock(mutex_);
  if (!worker_.joinable()) return;
  flushRequested_ = true;
  cv_.notify_all();
  cv_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this] { return stop_ || (ready_ && pending_.empty() && !busy_ && !dirty_); });
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
std::vector<HistoryStore::Suggestion> HistoryStore::Suggest(std::wstring query, size_t limit) {
  query = Lower(std::move(query));
  std::vector<Suggestion> rows;
  {
    std::lock_guard lock(mutex_);
    for (const auto& e : entries_) if (query.empty() || e.search.find(query) != std::wstring::npos)
      rows.push_back({e.url, e.title, e.visits, e.typed});
  }
  std::stable_sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
    return a.typed * 5 + a.visits > b.typed * 5 + b.visits;
  });
  if (rows.size() > limit) rows.resize(limit);
  return rows;
}
bool HistoryStore::Apply(Operation& op) {
  auto& n = op.entry;
  if (op.kind == 0) {
    auto old = std::find_if(entries_.begin(), entries_.end(), [&n](const auto& e) { return e.url == n.url; });
    if (old != entries_.end()) {
      n.id = old->id; n.visits += old->visits; n.typed += old->typed;
      if (n.title.empty()) n.title = old->title;
      entries_.erase(old);
    } else n.id = nextId_++;
    n.search = SearchKey(n.title, n.url);
    entries_.insert(entries_.begin(), std::move(n));
  } else if (op.kind == 1) {
    auto it = std::find_if(entries_.begin(), entries_.end(), [&n](const auto& e) { return e.url == n.url; });
    if (it != entries_.end() && it->title != n.title) { it->title = std::move(n.title); it->search = SearchKey(it->title, it->url); }
    return false; // Title-only changes are visible immediately but do not create disk churn.
  } else if (op.kind == 2) return std::erase_if(entries_, [&n](const auto& e) { return e.id == n.id; }) != 0;
  else if (op.kind == 3) { if (entries_.empty()) return false; entries_.clear(); }
  else if (op.kind == 4) return std::erase_if(entries_, [&n](const auto& e) {
    const auto split = e.url.find(L"://");
    const auto origin = Lower(e.url.substr(0, e.url.find_first_of(L"/?#", split + 3)));
    return e.at >= n.at && e.at < n.id && (n.url.empty() || n.url == origin);
  }) != 0;
  return true;
}
// 持锁进入、持锁返回；序列化与写盘时放开锁
bool HistoryStore::SaveLocked(std::unique_lock<std::mutex>& lock) {
  const auto snapshot = entries_;
  const uint64_t generation = generation_;
  dirty_ = false; firstDirty_ = 0;
  lock.unlock();
  auto data = json::Value::MakeArray();
  for (const auto& e : snapshot) {
    auto row = Row(e.id, e.at, e.url, e.title);
    row.Set("visits", json::Value::Number(static_cast<double>(e.visits)));
    row.Set("typed", json::Value::Number(static_cast<double>(e.typed)));
    data.Push(std::move(row));
  }
  const bool saved = memoryOnly_ || WriteFileAtomic(file_, json::Serialize(data));
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
  if (!memoryOnly_ && ReadFileBytes(file_, bytes) && json::Parse(bytes, root)) {
    const int64_t now = Now();
    for (const auto& row : root.Items()) {
      auto url = SafeUrl(FromUtf8(row["url"].AsString()));
      const auto at = static_cast<int64_t>(row["at"].AsNumber());
      if (url.empty() || at < now - 90LL*86400000 || at > now + 86400000 || loaded.size() >= 20000) continue;
      auto title = TruncateUtf16(FromUtf8(row["title"].AsString()), 4096);
      auto search = SearchKey(title, url);
      loaded.push_back({nextId_++, at, std::move(url), std::move(title), std::move(search),
          static_cast<uint64_t>(std::clamp(row["visits"].AsNumber(1), 1.0, 1000000000.0)),
          static_cast<uint64_t>(std::clamp(row["typed"].AsNumber(), 0.0, 1000000000.0))});
    }
  }
  bytes.clear(); bytes.shrink_to_fit();
  std::unique_lock lock(mutex_); entries_ = std::move(loaded); ready_ = true; cv_.notify_all();
  for (;;) {
    if (!pending_.empty()) {
      busy_ = true;
      bool changed = false;
      for (size_t batch = 0; batch < 64 && !pending_.empty(); ++batch) { auto op = std::move(pending_.front()); pending_.pop_front(); changed = Apply(op) || changed; }
      if (!changed) { busy_ = false; cv_.notify_all(); lock.unlock(); std::this_thread::yield(); lock.lock(); continue; }
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
      lock.unlock(); std::this_thread::yield(); lock.lock();
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
