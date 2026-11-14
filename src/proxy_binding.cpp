#include <winsock2.h>
#include <ws2tcpip.h>
#include "proxy_binding.h"
#include "common.h"
#include "json.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cwctype>
#include <mutex>
#include <thread>

namespace jisu {
namespace {
struct Socket {
  SOCKET value = INVALID_SOCKET;
  explicit Socket(SOCKET s = INVALID_SOCKET) : value(s) {}
  ~Socket() { if (value != INVALID_SOCKET) closesocket(value); }
  Socket(const Socket&) = delete;
};
std::wstring Trim(std::wstring s) {
  const auto first = s.find_first_not_of(L" \t\r\n");
  if (first == std::wstring::npos) return {};
  return s.substr(first, s.find_last_not_of(L" \t\r\n") - first + 1);
}
std::wstring LocalEndpoint(std::wstring value) {
  value = Trim(value);
  std::transform(value.begin(), value.end(), value.begin(), towlower);
  if (value.starts_with(L"http://")) value.erase(0, 7);
  const auto colon = value.rfind(L':');
  if (colon == std::wstring::npos || value.size() - colon > 6) return {};
  const auto host = value.substr(0, colon), number = value.substr(colon + 1);
  if (host != L"127.0.0.1" && host != L"localhost" && host != L"[::1]") return {};
  if (number.empty() || !std::all_of(number.begin(), number.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; })) return {};
  const auto port = wcstoul(number.c_str(), nullptr, 10);
  if (port == 0 || port > 65535) return {};
  return host + L":" + std::to_wstring(port);
}
std::wstring ReadProxy(const std::wstring& key) {
  std::array<wchar_t, 4096> value{};
  DWORD bytes = static_cast<DWORD>(sizeof(value));
  if (RegGetValueW(HKEY_CURRENT_USER, key.c_str(), L"ProxyServer", RRF_RT_REG_SZ, nullptr, value.data(), &bytes) != ERROR_SUCCESS) return {};
  return std::wstring(value.data());
}
SOCKET Dial(const std::wstring& endpoint) {
  const auto colon = endpoint.rfind(L':');
  std::wstring host = endpoint.substr(0, colon);
  if (host == L"localhost") host = L"127.0.0.1";
  if (host.starts_with(L"[")) host = host.substr(1, host.size() - 2);
  ADDRINFOW hints{}; hints.ai_socktype = SOCK_STREAM; hints.ai_protocol = IPPROTO_TCP; hints.ai_flags = AI_NUMERICHOST;
  ADDRINFOW* addresses = nullptr;
  if (GetAddrInfoW(host.c_str(), endpoint.substr(colon + 1).c_str(), &hints, &addresses) != 0) return INVALID_SOCKET;
  SOCKET result = INVALID_SOCKET;
  for (auto* a = addresses; a; a = a->ai_next) {
    const SOCKET s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
    if (s == INVALID_SOCKET) continue;
    u_long nonblocking = 1; ioctlsocket(s, FIONBIO, &nonblocking);
    int status = connect(s, a->ai_addr, static_cast<int>(a->ai_addrlen));
    if (status == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) {
      fd_set writable, errors; FD_ZERO(&writable); FD_ZERO(&errors); FD_SET(s, &writable); FD_SET(s, &errors);
      timeval timeout{3, 0};
      if (select(0, nullptr, &writable, &errors, &timeout) > 0 && FD_ISSET(s, &writable)) {
        int error = 0, length = sizeof(error);
        if (getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &length) == 0 && error == 0) status = 0;
      }
    }
    if (status == 0) {
      nonblocking = 0; ioctlsocket(s, FIONBIO, &nonblocking);
      DWORD timeout = 5000; setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
      result = s; break;
    }
    closesocket(s);
  }
  FreeAddrInfoW(addresses);
  return result;
}
bool SendAll(SOCKET socket, const char* data, int size) {
  for (int done = 0; done < size;) {
    const int sent = send(socket, data + done, size - done, 0);
    if (sent <= 0) return false;
    done += sent;
  }
  return true;
}
void Unavailable(SOCKET socket) {
  constexpr char response[] = "HTTP/1.1 502 Bad Gateway\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
  SendAll(socket, response, static_cast<int>(sizeof(response) - 1));
}
}  // namespace

ProxyRoute ParseLocalProxyRoute(const std::wstring& value) {
  if (value.find(L'=') == std::wstring::npos) {
    const auto endpoint = LocalEndpoint(value);
    return {endpoint, endpoint};
  }
  ProxyRoute result;
  for (size_t start = 0; start < value.size();) {
    const auto end = value.find(L';', start);
    const auto entry = Trim(value.substr(start, end == std::wstring::npos ? end : end - start));
    const auto equals = entry.find(L'=');
    if (equals != std::wstring::npos) {
      auto scheme = Trim(entry.substr(0, equals));
      std::transform(scheme.begin(), scheme.end(), scheme.begin(), towlower);
      const auto endpoint = LocalEndpoint(entry.substr(equals + 1));
      if (scheme == L"http") result.http = endpoint;
      if (scheme == L"https") result.https = endpoint;
    }
    if (end == std::wstring::npos) break;
    start = end + 1;
  }
  if (result.http.empty()) result.http = result.https;
  if (result.https.empty()) result.https = result.http;
  return result;
}

struct ProxyBinding::State : std::enable_shared_from_this<ProxyBinding::State> {
  std::wstring profile, registryKey, mutexName;
  unsigned port = 0;
  std::atomic<bool> stop{false};
  std::atomic<unsigned> connections{0};
  std::thread worker;
  std::mutex routeMutex;
  ProxyRoute route;
  bool poolMode = false;
  std::string lastStatus;
  void Status(const char* error = "") {
    std::string text = "{\"mode\":\"" + std::string(poolMode ? "v2ray-pool" : "latest-system-address") + "\",\"ignoresSystemSwitch\":true,\"listen\":\"127.0.0.1:" + std::to_string(port) + "\",\"ownerPid\":" + std::to_string(GetCurrentProcessId()) + ",\"http\":";
    json::AppendQuoted(text, ToUtf8(route.http)); text += ",\"https\":"; json::AppendQuoted(text, ToUtf8(route.https));
    text += ",\"error\":"; json::AppendQuoted(text, error); text += "}";
    if (text != lastStatus) { EnsureDirectory(profile); WriteFileAtomic(profile + L"\\proxy-binding-status.json", text, false); lastStatus = text; }
  }
  void Refresh() {
    ProxyRoute next;
    const bool modeBefore = poolMode;
    std::string bindingText; json::Value binding;
    const std::wstring bindingPath = profile + L"\\proxy-binding-upstream.json";
    const bool configured = ReadFileBytes(bindingPath, bindingText);
    if (configured && json::Parse(bindingText, binding) && binding["mode"].AsString() == "v2ray-pool") {
      next = ParseLocalProxyRoute(L"http=" + FromUtf8(binding["http"].AsString()) + L";https=" + FromUtf8(binding["https"].AsString()));
      if (!next.Valid()) { if (!poolMode) route = {}; poolMode = true; Status("pool-binding-unreadable"); return; }
      poolMode = true;
    } else if (configured || poolMode) {
      if (!poolMode) { poolMode = true; route = {}; }
      Status("pool-binding-unreadable"); return; // Never silently fall back to system proxies after opting in.
    } else next = ParseLocalProxyRoute(ReadProxy(registryKey));
    // Clearing/disabling system proxy must NOT turn this browser into a direct client.
    if (!next.Valid()) return;
    const auto self = L"127.0.0.1:" + std::to_wstring(port);
    const auto localSelf = L"localhost:" + std::to_wstring(port);
    if (next.http == self || next.https == self || next.http == localSelf || next.https == localSelf ||
        next.http == L"[::1]:" + std::to_wstring(port) || next.https == L"[::1]:" + std::to_wstring(port)) return;
    std::lock_guard lock(routeMutex);
    if (route == next && poolMode == modeBefore) { Status(); return; }
    route = std::move(next);
    std::string cache = "{\"poolMode\":" + std::string(poolMode ? "true" : "false") + ",\"http\":"; json::AppendQuoted(cache, ToUtf8(route.http)); cache += ",\"https\":"; json::AppendQuoted(cache, ToUtf8(route.https)); cache += "}";
    EnsureDirectory(profile); WriteFileAtomic(profile + L"\\proxy-binding-last.json", cache, false);
    Status();
  }
  void Relay(SOCKET client) {
    Socket incoming(client);
    struct Count { std::atomic<unsigned>& count; ~Count() { --count; } } count{connections};
    DWORD timeout = 5000;
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    // Read only the method to respect per-protocol ProxyServer strings; forward unchanged, never record request bytes.
    std::array<char, 8> method{};
    const int first = recv(client, method.data(), static_cast<int>(method.size()), MSG_WAITALL);
    if (first <= 0 || stop) return;
    std::wstring target;
    { std::lock_guard lock(routeMutex); target = std::string_view(method.data(), static_cast<size_t>(first)).starts_with("CONNECT ") ? route.https : route.http; }
    Socket upstream(target.empty() ? INVALID_SOCKET : Dial(target));
    if (upstream.value == INVALID_SOCKET) { Unavailable(client); return; }
    if (!SendAll(upstream.value, method.data(), first)) return;
    bool readClient = true, readUpstream = true;
    std::array<char, 32768> data{};
    while (!stop && (readClient || readUpstream)) {
      fd_set readable; FD_ZERO(&readable);
      if (readClient) FD_SET(client, &readable);
      if (readUpstream) FD_SET(upstream.value, &readable);
      timeval wait{0, 250000};
      const int ready = select(0, &readable, nullptr, nullptr, &wait);
      if (ready == SOCKET_ERROR) return;
      if (readClient && FD_ISSET(client, &readable)) {
        const int n = recv(client, data.data(), static_cast<int>(data.size()), 0);
        if (n < 0) return;
        if (n == 0) { readClient = false; shutdown(upstream.value, SD_SEND); }
        else if (!SendAll(upstream.value, data.data(), n)) return;
      }
      if (readUpstream && FD_ISSET(upstream.value, &readable)) {
        const int n = recv(upstream.value, data.data(), static_cast<int>(data.size()), 0);
        if (n < 0) return;
        if (n == 0) { readUpstream = false; shutdown(client, SD_SEND); }
        else if (!SendAll(client, data.data(), n)) return;
      }
    }
  }
  void Run() {
    const HANDLE election = CreateMutexW(nullptr, FALSE, mutexName.c_str());
    if (!election) return;
    while (!stop) {
      const DWORD acquired = WaitForSingleObject(election, 250);
      if (acquired != WAIT_OBJECT_0 && acquired != WAIT_ABANDONED) continue;
      Socket listener(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
      BOOL exclusive = TRUE; setsockopt(listener.value, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
      sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); address.sin_port = htons(static_cast<u_short>(port));
      if (listener.value == INVALID_SOCKET || bind(listener.value, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || listen(listener.value, 64) != 0) {
        Status("loopback-port-unavailable"); ReleaseMutex(election); Sleep(500); continue;
      }
      // All windows sharing a WebView2 environment use the same relay; another live window takes over if its owner exits.
      std::string cache; json::Value saved;
      if (ReadFileBytes(profile + L"\\proxy-binding-last.json", cache) && json::Parse(cache, saved)) {
        std::lock_guard lock(routeMutex);
        route = ParseLocalProxyRoute(L"http=" + FromUtf8(saved["http"].AsString()) + L";https=" + FromUtf8(saved["https"].AsString()));
        poolMode = saved["poolMode"].AsBool(false);
      }
      Refresh(); Status(route.Valid() ? "" : "no-valid-local-proxy");
      ULONGLONG refreshed = GetTickCount64();
      while (!stop) {
        if (GetTickCount64() - refreshed >= 1000) { Refresh(); refreshed = GetTickCount64(); }
        fd_set readable; FD_ZERO(&readable); FD_SET(listener.value, &readable);
        timeval wait{0, 250000};
        if (select(0, &readable, nullptr, nullptr, &wait) <= 0) continue;
        const SOCKET client = accept(listener.value, nullptr, nullptr);
        if (client == INVALID_SOCKET) continue;
        if (connections >= 128) { closesocket(client); continue; }
        ++connections;
        std::thread([self = shared_from_this(), client] { self->Relay(client); }).detach();
      }
      // Close the listener before releasing ownership; no new helper or visible process is started.
      closesocket(listener.value); listener.value = INVALID_SOCKET;
      ReleaseMutex(election);
    }
    CloseHandle(election);
  }
};

ProxyBinding::ProxyBinding(const std::wstring& profile, const std::wstring& key) : state_(std::make_shared<State>()) {
  static const bool initialized = [] { WSADATA data{}; return WSAStartup(MAKEWORD(2, 2), &data) == 0; }();
  state_->profile = profile; state_->registryKey = key;
  uint32_t hash = 2166136261u;
  for (wchar_t c : profile) { hash ^= static_cast<uint32_t>(towlower(c == L'/' ? L'\\' : c)); hash *= 16777619u; }
  state_->port = 38000 + hash % 18000;
  state_->mutexName = L"Local\\JisuProxyBinding-" + std::to_wstring(hash);
  if (initialized) state_->worker = std::thread([self = state_] { self->Run(); });
}
ProxyBinding::~ProxyBinding() {
  state_->stop = true;
  if (state_->worker.joinable()) state_->worker.join();
}
unsigned ProxyBinding::Port() const { return state_->port; }
std::wstring BrowserProxyArguments(const std::wstring& profile, const std::wstring& arguments, bool enabled) {
  if (!enabled || arguments.find(L"--proxy-") != std::wstring::npos || arguments.find(L"--no-proxy-server") != std::wstring::npos) return arguments;
  static ProxyBinding binding(profile);
  return arguments + L" --proxy-server=http://127.0.0.1:" + std::to_wstring(binding.Port()) +
      L" --proxy-bypass-list=\"<local>;localhost;127.0.0.1;[::1];10.0.0.0/8;172.16.0.0/12;192.168.0.0/16;100.64.0.0/10\"";
}
}  // namespace jisu
