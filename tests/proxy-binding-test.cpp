#include <winsock2.h>
#include <ws2tcpip.h>
#include "proxy_binding.h"
#include "common.h"
#include <atomic>
#include <iostream>
#include <thread>
#include <vector>
using namespace jisu;
namespace {
void Check(bool ok, const char* label) { if (!ok) { std::cerr << "FAIL " << label << std::endl; ExitProcess(1); } }
void Send(SOCKET s, const std::string& value) {
  size_t offset = 0;
  while (offset < value.size()) { const int n = send(s, value.data() + offset, static_cast<int>(value.size() - offset), 0); Check(n > 0, "send"); offset += n; }
}
SOCKET Connect(unsigned port) {
  const SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  DWORD timeout = 6000;
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
  sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); address.sin_port = htons(static_cast<u_short>(port));
  if (connect(s, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) return s;
  closesocket(s); return INVALID_SOCKET;
}
struct MockProxy {
  SOCKET listener = INVALID_SOCKET;
  unsigned port = 0;
  char marker;
  std::atomic<bool> stop{false};
  std::thread worker;
  std::vector<std::thread> clients;
  explicit MockProxy(char id) : marker(id) {
    listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    Check(bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 && listen(listener, 8) == 0, "mock-listen");
    int size = sizeof(address); getsockname(listener, reinterpret_cast<sockaddr*>(&address), &size); port = ntohs(address.sin_port);
    worker = std::thread([this] {
      while (!stop) {
        fd_set reads; FD_ZERO(&reads); FD_SET(listener, &reads); timeval wait{0, 100000};
        if (select(0, &reads, nullptr, nullptr, &wait) <= 0) continue;
        const SOCKET client = accept(listener, nullptr, nullptr);
        if (client == INVALID_SOCKET) continue;
        clients.emplace_back([this, client] {
          DWORD timeout = 6000; setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
          std::string request; char buffer[4096];
          while (request.find("\r\n\r\n") == std::string::npos) { const int n = recv(client, buffer, sizeof(buffer), 0); if (n <= 0) { closesocket(client); return; } request.append(buffer, n); }
          if (request.starts_with("CONNECT ")) {
            Send(client, "HTTP/1.1 200 Connection established\r\n\r\n");
            for (;;) { const int n = recv(client, buffer, sizeof(buffer), 0); if (n <= 0) break; Send(client, std::string(1, marker) + std::string(buffer, n)); }
          } else Send(client, "HTTP/1.1 200 OK\r\nContent-Length: 1\r\nConnection: close\r\n\r\n" + std::string(1, marker));
          shutdown(client, SD_BOTH); closesocket(client);
        });
      }
    });
  }
  ~MockProxy() { stop = true; worker.join(); closesocket(listener); for (auto& c : clients) c.join(); }
  std::wstring Endpoint() const { return L"127.0.0.1:" + std::to_wstring(port); }
};
std::string Request(unsigned port) {
  SOCKET client = Connect(port); if (client == INVALID_SOCKET) return {};
  Send(client, "GET http://must-not-go-direct.invalid/probe HTTP/1.1\r\nHost: must-not-go-direct.invalid\r\nConnection: close\r\n\r\n");
  std::string reply; char buffer[1024];
  for (;;) { const int n = recv(client, buffer, sizeof(buffer), 0); if (n <= 0) break; reply.append(buffer, n); }
  closesocket(client); return reply;
}
void AwaitMarker(unsigned port, char marker, const char* label) {
  for (int n = 0; n < 50; ++n) { const auto reply = Request(port); if (reply.ends_with(std::string("\r\n\r\n") + marker)) return; Sleep(150); }
  Check(false, label);
}
}  // namespace
int main() {
  WSADATA data{}; Check(WSAStartup(MAKEWORD(2, 2), &data) == 0, "winsock");
  Check(ParseLocalProxyRoute(L" http://localhost:10808 ") == ProxyRoute{L"localhost:10808", L"localhost:10808"}, "single-http");
  Check(ParseLocalProxyRoute(L"HTTPS=[::1]:65535;http=127.0.0.1:10808").https == L"[::1]:65535", "per-scheme-ipv6");
  for (const auto* value : {L"", L"127.0.0.1:0", L"127.0.0.1:65536", L"127.0.0.1:8/x", L"socks5://127.0.0.1:10808", L"user:pass@127.0.0.1:10808", L"untrusted.example:10808"}) Check(!ParseLocalProxyRoute(value).Valid(), "reject-invalid-or-nonlocal");
  const std::wstring key = L"Software\\JisuBrowserProxyTest-" + std::to_wstring(GetCurrentProcessId());
  HKEY registry = nullptr; Check(RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &registry, nullptr) == ERROR_SUCCESS, "test-registry");
  DWORD disabled = 0; RegSetValueExW(registry, L"ProxyEnable", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&disabled), sizeof(disabled));
  auto Set = [&](const std::wstring& value) { Check(RegSetValueExW(registry, L"ProxyServer", 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()), static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS, "set-test-route"); };
  wchar_t temporary[MAX_PATH]; GetTempPathW(MAX_PATH, temporary);
  const std::wstring profile = std::wstring(temporary) + L"jisu-proxy-test-" + std::to_wstring(GetCurrentProcessId());
  EnsureDirectory(profile);
  MockProxy a('A'), b('B'); Set(a.Endpoint());
  auto first = std::make_unique<ProxyBinding>(profile, key);
  const unsigned port = first->Port();
  AwaitMarker(port, 'A', "disabled-switch-still-proxies");
  const SOCKET tunnel = Connect(port); Check(tunnel != INVALID_SOCKET, "tunnel");
  Send(tunnel, "CONNECT must-not-go-direct.invalid:443 HTTP/1.1\r\nHost: must-not-go-direct.invalid:443\r\n\r\n");
  char buffer[256]; int n = recv(tunnel, buffer, sizeof(buffer), 0); Check(n > 0 && std::string(buffer, n).starts_with("HTTP/1.1 200"), "connect-preserved");
  Set(b.Endpoint()); AwaitMarker(port, 'B', "dynamic-address-change");
  Send(tunnel, "opaque-tls-stream"); n = recv(tunnel, buffer, sizeof(buffer), 0);
  Check(n > 0 && std::string(buffer, n) == "Aopaque-tls-stream", "existing-stream-not-migrated-or-interrupted"); closesocket(tunnel);
  Set(L""); Sleep(1250); AwaitMarker(port, 'B', "cleared-value-keeps-last");
  Set(L"user:secret@127.0.0.1:2"); Sleep(1250); AwaitMarker(port, 'B', "invalid-does-not-replace-last");
  auto second = std::make_unique<ProxyBinding>(profile, key); Check(second->Port() == port, "shared-profile-stable-port");
  Sleep(300); first.reset(); AwaitMarker(port, 'B', "owner-exit-takeover-and-persistent-cache");
  Set(L"http=" + a.Endpoint() + L";https=" + b.Endpoint()); Sleep(1250); AwaitMarker(port, 'A', "per-protocol-http");
  second.reset();
  ProxyBinding restarted(profile, key); AwaitMarker(port, 'A', "restart");
  const auto args = BrowserProxyArguments(profile, L"--disable-features=Something", false);
  Check(args == L"--disable-features=Something", "metrics-opt-out");
  Check(BrowserProxyArguments(profile, L"--proxy-server=http://127.0.0.1:9", true) == L"--proxy-server=http://127.0.0.1:9", "explicit-override");
  DWORD type = 0, bytes = sizeof(disabled); disabled = 7;
  RegQueryValueExW(registry, L"ProxyEnable", nullptr, &type, reinterpret_cast<BYTE*>(&disabled), &bytes);
  Check(disabled == 0, "system-switch-not-written");
  // A valid but non-listening port must return 502, never reach the destination directly.
  const SOCKET reserved = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  Check(bind(reserved, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "reserved-port");
  int size = sizeof(address); getsockname(reserved, reinterpret_cast<sockaddr*>(&address), &size);
  Set(L"127.0.0.1:" + std::to_wstring(ntohs(address.sin_port))); Sleep(1250);
  Check(Request(port).starts_with("HTTP/1.1 502"), "fail-closed-no-direct-fallback"); closesocket(reserved);
  RegCloseKey(registry); RegDeleteTreeW(HKEY_CURRENT_USER, key.c_str());
  std::cout << "PASS proxy binding: switch independent, dynamic address, CONNECT stream, cache, takeover, per-protocol, fail-closed, no system writes" << std::endl;
  return 0;
}
