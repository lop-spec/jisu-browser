// Cookie 守卫的回收策略：纯逻辑，不依赖 Windows / WebView2；tests/cookie-policy-test.cpp 直接编它。
//
// 同一站点（eTLD+1）的 cookie 超过上限时，只回收「带 ID 的一次性 cookie」：名字里有 UUID / 16 位以上十六进制 /
// 8 位以上纯数字（如 conv_key_<uuid>），同一个 ID 的全部 cookie 算一个单元，从最早的单元开始回收。
// 固定名字的基础 cookie（登录、偏好、风控）不动；名字带 session/token/auth 等的不动；会话 cookie 不动；
// 同类不足 3 个 ID 的不算「累积」；最新的 keepUnits 个单元一定保留（正在进行的对话用它们）。
// WebView2 不提供 cookie 的创建时间，用过期时间近似：同一类 cookie 寿命相同，先过期的就是最早建的。
// 参考实现与用例见 cookie-cap/planner.mjs（两边逐条对应）。
#pragma once

#include <algorithm>
#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace jisu {

struct CookieInfo {
  std::string name;
  std::string domain;
  std::string path;
  double expires = -1;  // Unix 秒；<= 0 = 会话 cookie
  bool session = true;
};

struct CookieGuardConfig {
  bool enabled = true;
  bool dryRun = false;       // 只算、只记录，不删
  size_t maxPerSite = 20;    // 0 = 不限制
  size_t keepUnits = 10;
  int intervalSeconds = 300;
};

struct CookieSiteReport {
  std::string site;
  size_t count = 0;
  size_t units = 0;        // 可回收的单元数（超限站点才统计）
  size_t evictUnits = 0;
  size_t evictCookies = 0;
};

struct CookiePlan {
  std::vector<size_t> evict;  // cookies 里的下标，升序
  std::vector<CookieSiteReport> sites;
};

namespace cookie_detail {

inline char LowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; }
inline bool IsHexChar(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }
inline bool IsDigitChar(char c) { return c >= '0' && c <= '9'; }

inline bool TwoLabelSuffix(std::string_view tail) {
  static constexpr std::string_view kSuffixes[] = {
      "co.uk", "org.uk", "ac.uk", "gov.uk", "com.cn", "net.cn", "org.cn", "gov.cn", "edu.cn", "com.hk", "com.tw",
      "org.tw", "co.jp", "ne.jp", "or.jp", "com.au", "net.au", "org.au", "co.kr", "co.nz", "co.in", "com.br",
      "com.mx", "com.sg", "com.my", "com.tr", "com.ar", "co.za", "github.io", "gitlab.io", "blogspot.com",
      "vercel.app", "netlify.app", "pages.dev", "workers.dev", "herokuapp.com", "appspot.com", "web.app",
      "firebaseapp.com", "azurewebsites.net", "cloudfront.net"};
  for (const std::string_view suffix : kSuffixes) {
    if (suffix == tail) return true;
  }
  return false;
}

inline bool UuidAt(const std::string& s, size_t i) {
  if (i + 36 > s.size()) return false;
  for (size_t k = 0; k < 36; ++k) {
    const char c = s[i + k];
    if (k == 8 || k == 13 || k == 18 || k == 23) {
      if (c != '-') return false;
    } else if (!IsHexChar(c)) {
      return false;
    }
  }
  return true;
}

inline bool HasProtectedWord(const std::string& family) {
  static constexpr std::string_view kWords[] = {"session", "token", "auth", "login", "passport",
                                                "jwt", "csrf", "sso", "credential", "secret"};
  for (const std::string_view word : kWords) {
    if (family.find(word) != std::string::npos) return true;
  }
  return false;
}

}  // namespace cookie_detail

// 站点键：小写、去掉前导点，取 eTLD+1（内置常见的两段式后缀）；IP 与单段主机原样返回。
inline std::string CookieSiteKey(std::string_view domain) {
  std::string host;
  host.reserve(domain.size());
  for (const char c : domain) host.push_back(cookie_detail::LowerAscii(c));
  size_t start = 0;
  while (start < host.size() && host[start] == '.') ++start;
  host.erase(0, start);
  if (host.empty() || host.find(':') != std::string::npos) return host;
  bool ip = true;
  size_t dots = 0;
  std::vector<size_t> dotAt;
  for (size_t i = 0; i < host.size(); ++i) {
    if (host[i] == '.') {
      ++dots;
      dotAt.push_back(i);
    } else if (!cookie_detail::IsDigitChar(host[i])) {
      ip = false;
    }
  }
  if (ip && dots == 3) return host;
  if (dotAt.size() < 2) return host;  // 最多两段
  const std::string tail = host.substr(dotAt[dotAt.size() - 2] + 1);
  if (!cookie_detail::TwoLabelSuffix(tail)) return tail;
  if (dotAt.size() < 3) return host;
  return host.substr(dotAt[dotAt.size() - 3] + 1);
}

// 名字小写后，把 ID 换成 '#' 得到 family；token 是第一个 ID（没有则空）。
inline void CookieSplitName(std::string_view name, std::string& family, std::string& token) {
  std::string s;
  s.reserve(name.size());
  for (const char c : name) s.push_back(cookie_detail::LowerAscii(c));
  family.clear();
  token.clear();
  size_t i = 0;
  while (i < s.size()) {
    if (cookie_detail::IsHexChar(s[i]) && cookie_detail::UuidAt(s, i)) {
      if (token.empty()) token = s.substr(i, 36);
      family.push_back('#');
      i += 36;
      continue;
    }
    if (cookie_detail::IsHexChar(s[i])) {
      size_t j = i;
      bool digitsOnly = true;
      while (j < s.size() && cookie_detail::IsHexChar(s[j])) {
        if (!cookie_detail::IsDigitChar(s[j])) digitsOnly = false;
        ++j;
      }
      const size_t length = j - i;
      if (length >= 16 || (length >= 8 && digitsOnly)) {
        if (token.empty()) token = s.substr(i, length);
        family.push_back('#');
      } else {
        family.append(s, i, length);
      }
      i = j;
      continue;
    }
    family.push_back(s[i]);
    ++i;
  }
}

inline CookiePlan PlanCookieEvictions(const std::vector<CookieInfo>& cookies, const CookieGuardConfig& config) {
  constexpr size_t kMinFamilyTokens = 3;
  CookiePlan plan;
  if (!config.enabled || config.maxPerSite == 0) return plan;

  std::map<std::string, std::vector<size_t>> bySite;
  for (size_t i = 0; i < cookies.size(); ++i) bySite[CookieSiteKey(cookies[i].domain)].push_back(i);

  struct Parts {
    std::string family;  // 只含名字，保护词只查这里
    std::string group;   // family + 域 + 路径：统计同类 ID 个数
    std::string token;
  };
  struct Unit {
    std::string token;
    std::vector<size_t> members;
    bool eligible = true;
    double age = -1;
  };

  for (const auto& [site, indexes] : bySite) {
    CookieSiteReport report;
    report.site = site;
    report.count = indexes.size();
    plan.sites.push_back(report);
    CookieSiteReport& current = plan.sites.back();
    if (indexes.size() <= config.maxPerSite) continue;

    std::map<size_t, Parts> parts;
    std::map<std::string, std::set<std::string>> familyTokens;
    for (const size_t i : indexes) {
      const CookieInfo& cookie = cookies[i];
      Parts p;
      CookieSplitName(cookie.name, p.family, p.token);
      p.group = p.family + "|";
      for (const char c : cookie.domain) p.group.push_back(cookie_detail::LowerAscii(c));
      p.group += "|" + cookie.path;
      if (!p.token.empty()) familyTokens[p.group].insert(p.token);
      parts.emplace(i, std::move(p));
    }

    std::map<std::string, Unit> units;
    for (const size_t i : indexes) {
      const Parts& p = parts[i];
      if (p.token.empty()) continue;
      const CookieInfo& cookie = cookies[i];
      Unit& unit = units[p.token];
      unit.token = p.token;
      unit.members.push_back(i);
      const bool accumulating = familyTokens[p.group].size() >= kMinFamilyTokens;
      if (!accumulating || cookie.session || !(cookie.expires > 0) || cookie_detail::HasProtectedWord(p.family)) {
        unit.eligible = false;
      } else {
        unit.age = std::max(unit.age, cookie.expires);
      }
    }

    std::vector<const Unit*> eligible;
    for (const auto& entry : units) {
      if (entry.second.eligible) eligible.push_back(&entry.second);
    }
    std::sort(eligible.begin(), eligible.end(), [](const Unit* a, const Unit* b) {
      if (a->age != b->age) return a->age < b->age;
      return a->token < b->token;
    });
    current.units = eligible.size();
    size_t total = indexes.size();
    const size_t removable = eligible.size() > config.keepUnits ? eligible.size() - config.keepUnits : 0;
    for (size_t k = 0; k < removable && total > config.maxPerSite; ++k) {
      const Unit& unit = *eligible[k];
      plan.evict.insert(plan.evict.end(), unit.members.begin(), unit.members.end());
      total -= unit.members.size();
      ++current.evictUnits;
      current.evictCookies += unit.members.size();
    }
  }
  std::sort(plan.evict.begin(), plan.evict.end());
  return plan;
}

}  // namespace jisu
