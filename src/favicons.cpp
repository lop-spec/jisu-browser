#include "favicons.h"
#include <winhttp.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cwctype>
#include <deque>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>

namespace jisu {
namespace {
constexpr int64_t kMissTtlSeconds = 7LL * 24 * 3600;  // 站点没有 /favicon.ico：一周内不再问
constexpr ULONGLONG kFetchDelayMs = 3000;             // 启动后先让首个页面用网络
constexpr size_t kMaxQueue = 512;

int64_t UnixNow() {
  return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

uint64_t HashOrigin(const std::wstring& origin) {
  uint64_t hash = 14695981039346656037ull;
  for (unsigned char c : ToUtf8(origin)) {
    hash ^= c;
    hash *= 1099511628211ull;
  }
  return hash;
}

uint64_t HashBytes(const std::string& bytes) {
  uint64_t hash = 14695981039346656037ull;
  for (unsigned char c : bytes) {
    hash ^= c;
    hash *= 1099511628211ull;
  }
  return hash;
}

// Never probe loopback/private/intranet bookmark targets for a cosmetic asset.
bool PrivateHost(const std::wstring& host) {
  return host.find(L'.') == std::wstring::npos || host == L"localhost" || host.find(L':') != std::wstring::npos ||
         std::all_of(host.begin(), host.end(), [](wchar_t c) { return (c >= L'0' && c <= L'9') || c == L'.'; });
}

enum class FetchResult { kOk, kMissing, kError };
}  // namespace

struct FaviconCache::State {
  struct Job {
    std::wstring origin;
    std::string bytes;  // 非空 = 写入页面给的图标；空 = 抓取 /favicon.ico
  };
  std::wstring directory;
  HWND window = nullptr;
  UINT message = 0;
  ULONGLONG startedAt = 0;
  std::mutex mutex;
  std::condition_variable wake;
  std::deque<Job> queue;
  std::set<std::wstring> requested;             // 本次运行已排队抓取的源站
  std::unordered_map<std::wstring, int> known;  // key → 1 有文件 / 0 没有 / -1 最近抓过没有
  std::unordered_map<std::wstring, uint64_t> stored;  // key → 最近写入内容的哈希
  std::unordered_map<std::wstring, int64_t> misses;   // key → 上次没抓到的时刻（Unix 秒）
  bool stop = false;
  bool exited = false;

  std::wstring File(const std::wstring& key) const { return directory + L"\\" + key + L".png"; }
  std::wstring MissFile() const { return directory + L"\\misses.txt"; }

  void Notify(uint64_t hash) {
    HWND target = nullptr;
    {
      std::lock_guard<std::mutex> lock(mutex);
      target = window;
    }
    if (target) PostMessageW(target, message, static_cast<WPARAM>(hash), 0);
  }

  void LoadMisses() {
    std::string text;
    if (!ReadFileBytes(MissFile(), text)) return;
    const int64_t now = UnixNow();
    size_t lines = 0;
    std::unordered_map<std::wstring, int64_t> loaded;
    for (size_t start = 0; start < text.size();) {
      size_t end = text.find('\n', start);
      if (end == std::string::npos) end = text.size();
      const std::string line = text.substr(start, end - start);
      start = end + 1;
      ++lines;
      const size_t space = line.find(' ');
      if (space != 16) continue;
      const int64_t at = _strtoi64(line.c_str() + space + 1, nullptr, 10);
      if (at > 0 && now - at < kMissTtlSeconds) loaded[FromUtf8(line.substr(0, space))] = at;
    }
    {
      std::lock_guard<std::mutex> lock(mutex);
      for (const auto& [key, at] : loaded) misses.emplace(key, at);
    }
    // 过期条目多了就整理一遍，文件不无限长
    if (lines > loaded.size() * 2 + 64) {
      std::string compact;
      for (const auto& [key, at] : loaded) compact += ToUtf8(key) + " " + std::to_string(at) + "\n";
      WriteFileAtomic(MissFile(), compact, false);
    }
  }

  void RecordMiss(const std::wstring& key) {
    const int64_t now = UnixNow();
    {
      std::lock_guard<std::mutex> lock(mutex);
      misses[key] = now;
      known[key] = -1;
    }
    const std::string line = ToUtf8(key) + " " + std::to_string(now) + "\n";
    HANDLE file = CreateFileW(MissFile().c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(file, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    CloseHandle(file);
  }

  FetchResult Fetch(const std::wstring& origin, std::string& bytes) {
    const std::wstring url = origin + L"/favicon.ico";
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts)) return FetchResult::kError;
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    if (PrivateHost(host)) return FetchResult::kError;
    HINTERNET session = WinHttpOpen(L"JisuBrowser Favicon", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
    if (!session) return FetchResult::kError;
    WinHttpSetTimeouts(session, 2000, 2000, 2000, 2000);
    HINTERNET connection = WinHttpConnect(session, host.c_str(), parts.nPort, 0);
    HINTERNET request = connection ? WinHttpOpenRequest(connection, L"GET", L"/favicon.ico", nullptr, WINHTTP_NO_REFERER,
                                                        WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                        parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                                   : nullptr;
    FetchResult result = FetchResult::kError;
    if (request) {
      DWORD disabled = WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION | WINHTTP_DISABLE_REDIRECTS;
      WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof(disabled));
      if (WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) &&
          WinHttpReceiveResponse(request, nullptr)) {
        DWORD status = 0, length = sizeof(status);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &length,
                            nullptr);
        result = FetchResult::kMissing;  // 站点答复了但没有可用图标：记一周
        if (status == 200) {
          char buffer[8192];
          DWORD n = 0;
          bool complete = true;
          while (WinHttpReadData(request, buffer, sizeof(buffer), &n) && n) {
            bytes.append(buffer, n);
            if (bytes.size() > 512 * 1024) {
              bytes.clear();
              break;
            }
            std::lock_guard<std::mutex> lock(mutex);
            if (stop) {
              complete = false;
              break;
            }
          }
          if (!complete) {
            bytes.clear();
            result = FetchResult::kError;
          } else if (!bytes.empty()) {
            result = FetchResult::kOk;
          }
        }
      }
      WinHttpCloseHandle(request);
    }
    if (connection) WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return result;
  }

  // 后台线程：持有自己的 State，窗口和缓存对象先销毁也安全
  static void Run(std::shared_ptr<State> self) {
    EnsureDirectory(self->directory);
    self->LoadMisses();
    for (;;) {
      Job job;
      {
        std::unique_lock<std::mutex> lock(self->mutex);
        bool picked = false;
        while (!picked) {
          if (self->stop) {
            self->exited = true;
            self->wake.notify_all();
            return;
          }
          // 页面给的图标先写；抓取等启动 3 秒后再开始，不和首屏抢网络
          const auto store = std::find_if(self->queue.begin(), self->queue.end(), [](const Job& j) { return !j.bytes.empty(); });
          if (store != self->queue.end()) {
            job = std::move(*store);
            self->queue.erase(store);
            picked = true;
          } else if (!self->queue.empty()) {
            const ULONGLONG elapsed = GetTickCount64() - self->startedAt;
            if (elapsed >= kFetchDelayMs) {
              job = std::move(self->queue.front());
              self->queue.pop_front();
              picked = true;
            } else {
              self->wake.wait_for(lock, std::chrono::milliseconds(kFetchDelayMs - elapsed));
            }
          } else {
            self->wake.wait(lock);
          }
        }
      }
      const uint64_t hash = HashOrigin(job.origin);
      const std::wstring key = FaviconKeyFromHash(hash);
      const std::wstring file = self->File(key);
      if (!job.bytes.empty()) {
        std::string existing;
        const bool same = ReadFileBytes(file, existing) && existing == job.bytes;
        if (same || WriteFileAtomic(file, job.bytes, false)) {
          {
            std::lock_guard<std::mutex> lock(self->mutex);
            self->known[key] = 1;
          }
          if (!same) self->Notify(hash);
        }
        continue;
      }
      bool skip = false;
      {
        std::lock_guard<std::mutex> lock(self->mutex);
        const auto miss = self->misses.find(key);
        if (miss != self->misses.end() && UnixNow() - miss->second < kMissTtlSeconds) {
          self->known[key] = -1;
          skip = true;
        }
      }
      if (skip) continue;
      if (GetFileAttributesW(file.c_str()) != INVALID_FILE_ATTRIBUTES) {
        {
          std::lock_guard<std::mutex> lock(self->mutex);
          self->known[key] = 1;
        }
        self->Notify(hash);
        continue;
      }
      std::string bytes;
      const FetchResult result = self->Fetch(job.origin, bytes);
      if (result == FetchResult::kOk) {
        // Content decoding is done with WIC on the UI thread; a failed image retains the globe fallback.
        if (GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES && WriteFileAtomic(file, bytes, false)) {
          {
            std::lock_guard<std::mutex> lock(self->mutex);
            self->known[key] = 1;
          }
          self->Notify(hash);
        }
      } else if (result == FetchResult::kMissing) {
        self->RecordMiss(key);  // 断网、超时不记：下次启动再试
      }
    }
  }
};

std::wstring FaviconOrigin(const std::wstring& url) {
  const size_t split = url.find(L"://");
  if (split == std::wstring::npos) return {};
  std::wstring scheme = url.substr(0, split);
  std::transform(scheme.begin(), scheme.end(), scheme.begin(), towlower);
  if (scheme != L"https" && scheme != L"http") return {};
  const size_t end = url.find_first_of(L"/?#", split + 3);
  std::wstring host = url.substr(split + 3, end == std::wstring::npos ? end : end - split - 3);
  if (host.empty() || host.find(L'@') != std::wstring::npos) return {};
  std::transform(host.begin(), host.end(), host.begin(), towlower);
  return scheme + L"://" + host;
}

uint64_t FaviconHash(const std::wstring& url) {
  const std::wstring origin = FaviconOrigin(url);
  return origin.empty() ? 0 : HashOrigin(origin);
}

std::wstring FaviconKeyFromHash(uint64_t hash) {
  wchar_t text[20];
  swprintf_s(text, L"%016llx", static_cast<unsigned long long>(hash));
  return text;
}

std::wstring FaviconKey(const std::wstring& url) {
  const uint64_t hash = FaviconHash(url);
  return hash ? FaviconKeyFromHash(hash) : std::wstring();
}

FaviconCache::~FaviconCache() { Stop(); }

void FaviconCache::Start(const std::wstring& profile, HWND window, UINT message) {
  if (state_ || profile.empty()) return;
  state_ = std::make_shared<State>();
  state_->directory = profile + L"\\Favicons";
  state_->window = window;
  state_->message = message;
  state_->startedAt = GetTickCount64();
  std::thread(&State::Run, state_).detach();
}

void FaviconCache::SetWindow(HWND window) {
  if (!state_) return;
  std::lock_guard<std::mutex> lock(state_->mutex);
  state_->window = window;
}

void FaviconCache::Stop() {
  if (!state_) return;
  std::shared_ptr<State> state = std::move(state_);
  std::unique_lock<std::mutex> lock(state->mutex);
  state->stop = true;
  state->window = nullptr;
  state->queue.clear();
  state->wake.notify_all();
  // 空闲时线程马上退出；正在等网络就放手，进程退出时由系统收回
  state->wake.wait_for(lock, std::chrono::milliseconds(300), [&state] { return state->exited; });
}

std::wstring FaviconCache::Directory() const { return state_ ? state_->directory : std::wstring(); }

std::wstring FaviconCache::Request(const std::wstring& url) {
  if (!state_) return {};
  const std::wstring origin = FaviconOrigin(url);
  if (origin.empty()) return {};
  const std::wstring key = FaviconKeyFromHash(HashOrigin(origin));
  State& state = *state_;
  std::lock_guard<std::mutex> lock(state.mutex);
  auto known = state.known.find(key);
  if (known == state.known.end()) {
    // 每个源站只查一次盘，之后重绘只查内存
    const bool exists = GetFileAttributesW(state.File(key).c_str()) != INVALID_FILE_ATTRIBUTES;
    known = state.known.emplace(key, exists ? 1 : 0).first;
  }
  if (known->second == 1) return state.File(key);
  if (known->second == 0 && !state.stop && state.queue.size() < kMaxQueue && state.requested.insert(origin).second) {
    state.queue.push_back(State::Job{origin, {}});
    state.wake.notify_one();
  }
  return {};
}

void FaviconCache::Store(const std::wstring& url, IStream* stream) {
  if (!state_ || !stream) return;
  const std::wstring origin = FaviconOrigin(url);
  if (origin.empty()) return;
  STATSTG stat{};
  if (FAILED(stream->Stat(&stat, STATFLAG_NONAME)) || stat.cbSize.QuadPart == 0 || stat.cbSize.QuadPart > 512 * 1024) return;
  LARGE_INTEGER zero{};
  stream->Seek(zero, STREAM_SEEK_SET, nullptr);
  std::string bytes(static_cast<size_t>(stat.cbSize.QuadPart), '\0');
  ULONG read = 0;
  if (FAILED(stream->Read(bytes.data(), static_cast<ULONG>(bytes.size()), &read)) || read != bytes.size()) return;
  const std::wstring key = FaviconKeyFromHash(HashOrigin(origin));
  const uint64_t content = HashBytes(bytes);
  State& state = *state_;
  std::lock_guard<std::mutex> lock(state.mutex);
  if (state.stop) return;
  uint64_t& last = state.stored[key];
  if (last == content) return;  // 同一站点每次加载都会报一次图标：内容没变就不再写
  last = content;
  state.queue.push_back(State::Job{origin, std::move(bytes)});
  state.wake.notify_one();
}
}  // namespace jisu
