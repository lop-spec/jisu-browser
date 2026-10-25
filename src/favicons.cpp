#include "favicons.h"
#include <winhttp.h>
#include <algorithm>
#include <cwctype>
#include <iomanip>
#include <sstream>

namespace jisu {
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
std::wstring FaviconKey(const std::wstring& url) {
  const std::string origin = ToUtf8(FaviconOrigin(url));
  if (origin.empty()) return {};
  uint64_t hash = 14695981039346656037ull;
  for (unsigned char c : origin) { hash ^= c; hash *= 1099511628211ull; }
  std::wostringstream key;
  key << std::hex << std::setfill(L'0') << std::setw(16) << hash;
  return key.str();
}
FaviconCache::~FaviconCache() { Stop(); }
void FaviconCache::Start(const std::wstring& profile, HWND window, UINT message) {
  directory_ = profile + L"\\Favicons";
  EnsureDirectory(directory_);
  window_ = window; message_ = message;
  worker_ = std::thread(&FaviconCache::Worker, this);
}
void FaviconCache::Stop() {
  { std::lock_guard<std::mutex> lock(mutex_); stop_ = true; queue_.clear(); }
  wake_.notify_all();
  if (worker_.joinable()) worker_.join();
}
std::wstring FaviconCache::Request(const std::wstring& url) {
  const std::wstring key = FaviconKey(url), origin = FaviconOrigin(url);
  if (key.empty() || directory_.empty()) return {};
  const std::wstring file = directory_ + L"\\" + key + L".png";
  if (GetFileAttributesW(file.c_str()) != INVALID_FILE_ATTRIBUTES) return file;
  { std::lock_guard<std::mutex> lock(mutex_);
    if (!stop_ && requested_.insert(origin).second && queue_.size() < 512) queue_.push_back(origin);
  }
  wake_.notify_one();
  return {};
}
void FaviconCache::Store(const std::wstring& url, IStream* stream) {
  const std::wstring key = FaviconKey(url);
  if (key.empty() || directory_.empty() || !stream) return;
  STATSTG stat{};
  if (FAILED(stream->Stat(&stat, STATFLAG_NONAME)) || stat.cbSize.QuadPart == 0 || stat.cbSize.QuadPart > 512 * 1024) return;
  LARGE_INTEGER zero{}; stream->Seek(zero, STREAM_SEEK_SET, nullptr);
  std::string bytes(static_cast<size_t>(stat.cbSize.QuadPart), '\0');
  ULONG read = 0;
  if (SUCCEEDED(stream->Read(bytes.data(), static_cast<ULONG>(bytes.size()), &read)) && read == bytes.size()) {
    WriteFileAtomic(directory_ + L"\\" + key + L".png", bytes);
    PostMessageW(window_, message_, 0, 0);
  }
}
void FaviconCache::Worker() {
  for (;;) {
    std::wstring origin;
    { std::unique_lock<std::mutex> lock(mutex_); wake_.wait(lock, [this] { return stop_ || !queue_.empty(); });
      if (stop_) return; origin = std::move(queue_.front()); queue_.pop_front();
    }
    const std::wstring url = origin + L"/favicon.ico";
    URL_COMPONENTS parts{}; parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts)) continue;
    std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    // Never probe loopback/private/intranet bookmark targets for a cosmetic asset.
    if (host.find(L'.') == std::wstring::npos || host == L"localhost" || host.find(L':') != std::wstring::npos ||
        host.rfind(L"127.", 0) == 0 || host.rfind(L"10.", 0) == 0 || host.rfind(L"192.168.", 0) == 0 ||
        std::all_of(host.begin(), host.end(), [](wchar_t c) { return (c >= L'0' && c <= L'9') || c == L'.'; })) continue;
    HINTERNET session = WinHttpOpen(L"JisuBrowser Favicon", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
    if (!session) continue;
    WinHttpSetTimeouts(session, 2000, 2000, 2000, 2000);
    HINTERNET connection = WinHttpConnect(session, host.c_str(), parts.nPort, 0);
    HINTERNET request = connection ? WinHttpOpenRequest(connection, L"GET", L"/favicon.ico", nullptr,
      WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0) : nullptr;
    std::string bytes;
    if (request) {
      DWORD disabled = WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION | WINHTTP_DISABLE_REDIRECTS;
      WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof(disabled));
      if (WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) && WinHttpReceiveResponse(request, nullptr)) {
        DWORD status = 0, length = sizeof(status);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &length, nullptr);
        if (status == 200) {
          char buffer[8192]; DWORD n = 0;
          while (WinHttpReadData(request, buffer, sizeof(buffer), &n) && n) {
            bytes.append(buffer, n); if (bytes.size() > 512 * 1024) { bytes.clear(); break; }
            std::lock_guard<std::mutex> lock(mutex_); if (stop_) { bytes.clear(); break; }
          }
        }
      }
      WinHttpCloseHandle(request);
    }
    if (connection) WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    // Content decoding is done with WIC on the UI thread; a failed image retains the globe fallback.
    if (!bytes.empty()) {
      const std::wstring file = directory_ + L"\\" + FaviconKey(origin) + L".png";
      if (GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES && WriteFileAtomic(file, bytes)) PostMessageW(window_, message_, 0, 0);
    }
  }
}
}  // namespace jisu
