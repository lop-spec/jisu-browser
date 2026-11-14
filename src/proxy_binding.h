// Stable loopback HTTP-proxy relay. Reads ProxyServer, NEVER ProxyEnable.
// No TLS interception, credentials, node selection, system writes or extra process.
#pragma once
#include <memory>
#include <string>

namespace jisu {
struct ProxyRoute {
  std::wstring http;
  std::wstring https;
  bool operator==(const ProxyRoute&) const = default;
  bool Valid() const { return !http.empty() && !https.empty(); }
};
ProxyRoute ParseLocalProxyRoute(const std::wstring& value);
class ProxyBinding {
 public:
  explicit ProxyBinding(const std::wstring& profile,
      const std::wstring& registryKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings");
  ~ProxyBinding();
  ProxyBinding(const ProxyBinding&) = delete;
  ProxyBinding& operator=(const ProxyBinding&) = delete;
  unsigned Port() const;
 private:
  struct State;
  std::shared_ptr<State> state_;
};
// Explicit --proxy-* arguments retain their normal meaning; metrics don't opt in.
std::wstring BrowserProxyArguments(const std::wstring& profile, const std::wstring& arguments, bool enabled);
}  // namespace jisu
