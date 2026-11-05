// Cookie 守卫回收策略的用例；cookie-cap/planner.test.mjs 是同一组用例的参考实现版本。
#include "cookie_policy.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <set>
#include <string>
#include <vector>

using namespace jisu;

namespace {

void Require(bool ok, const char* what) {
  if (!ok) {
    std::cerr << "FAIL cookie policy: " << what << std::endl;
    std::exit(1);
  }
}

constexpr double kDay = 86400;
constexpr double kNow = 1793000000;

std::string Uuid(unsigned i) {
  char text[64];
  std::snprintf(text, sizeof(text), "%08x-aaf8-83ec-bcb0-801bbaf826e0", 0x6aeb0000u + i);
  return text;
}

CookieInfo Persistent(const std::string& name, const std::string& domain = "chatgpt.com", const std::string& path = "/",
                      double expires = kNow + 365 * kDay) {
  CookieInfo c;
  c.name = name;
  c.domain = domain;
  c.path = path;
  c.expires = expires;
  c.session = false;
  return c;
}

CookieInfo Session(const std::string& name, const std::string& domain = "chatgpt.com") {
  CookieInfo c;
  c.name = name;
  c.domain = domain;
  c.path = "/";
  c.expires = -1;
  c.session = true;
  return c;
}

// ChatGPT 眼下的样子：31 个固定名字的基础 cookie（含登录）+ units 个临时对话（每个两个 cookie，序号越大越新）。
std::vector<CookieInfo> ChatgptProfile(unsigned units) {
  std::vector<CookieInfo> list = {
      Persistent("__Secure-next-auth.session-token.0", ".chatgpt.com"),
      Persistent("__Secure-next-auth.session-token.1", ".chatgpt.com"),
      Session("__Host-next-auth.csrf-token"), Session("__Secure-next-auth.callback-url"), Session("_cfuvid", ".chatgpt.com"),
      Persistent("cf_clearance", ".chatgpt.com"), Persistent("__cf_bm", ".chatgpt.com", "/", kNow + 1800),
      Persistent("oai-did", ".chatgpt.com"), Persistent("oai-sc", ".chatgpt.com"), Persistent("_puid", ".chatgpt.com"),
      Persistent("oai-client-session-epoch", ".chatgpt.com"), Persistent("oai-did-first-seen", ".chatgpt.com"),
      Persistent("__oailb"), Persistent("__oailb", "chatgpt.com", "/backend-api/spaces"),
      Persistent("__oailb", "chatgpt.com", "/backend-api/pages"),
      Persistent("__cflb"), Persistent("__cflb", "ws.chatgpt.com"), Persistent("__cf_bm", ".ws.chatgpt.com"),
      Persistent("_uasid"), Persistent("_umsid"), Persistent("_dd_s_v2"), Persistent("g_state"), Persistent("oai-hlib"),
      Persistent("_account"), Persistent("_account_is_fedramp"), Persistent("oai-locale"), Persistent("codex_sidebar_width"),
      Persistent("codex_composer_mode"), Persistent("oai-tpp-model-settings"), Session("oai_pl_permission_state"),
      Session("oai-gn")};
  Require(list.size() == 31, "chatgpt base profile has 31 cookies");
  for (unsigned i = 0; i < units; ++i) {
    const double created = kNow - (units - i) * 3600.0;
    list.push_back(Persistent("conv_key_" + Uuid(i), "chatgpt.com", "/", created + 30 * kDay));
    list.push_back(Persistent("history_off_" + Uuid(i), "chatgpt.com", "/", created + 7 * kDay));
  }
  return list;
}

const CookieSiteReport* Find(const CookiePlan& plan, const std::string& site) {
  for (const auto& s : plan.sites) {
    if (s.site == site) return &s;
  }
  return nullptr;
}

bool Contains(const std::vector<size_t>& list, size_t value) {
  return std::find(list.begin(), list.end(), value) != list.end();
}

void TestSiteKey() {
  Require(CookieSiteKey("chatgpt.com") == "chatgpt.com", "plain domain");
  Require(CookieSiteKey(".chatgpt.com") == "chatgpt.com", "leading dot");
  Require(CookieSiteKey("ws.chatgpt.com") == "chatgpt.com", "subdomain");
  Require(CookieSiteKey("A.B.Example.CO.UK") == "example.co.uk", "two-label public suffix");
  Require(CookieSiteKey("x.github.io") == "x.github.io", "private suffix is its own site");
  Require(CookieSiteKey("www.baidu.com.cn") == "baidu.com.cn", "com.cn");
  Require(CookieSiteKey("127.0.0.1") == "127.0.0.1", "ipv4 kept");
  Require(CookieSiteKey("localhost") == "localhost", "single label kept");
  Require(CookieSiteKey("") == "", "empty");
}

void TestSplitName() {
  std::string family, token;
  const std::string u = Uuid(1);
  CookieSplitName("conv_key_" + u, family, token);
  Require(family == "conv_key_#" && token == u, "uuid token");
  std::string upper = "History_Off_" + u;
  for (auto& ch : upper) {
    if (ch >= 'a' && ch <= 'f') ch = static_cast<char>(ch - 32);
  }
  CookieSplitName(upper, family, token);
  Require(family == "history_off_#" && token == u, "case-insensitive uuid");
  CookieSplitName("__Secure-next-auth.session-token.0", family, token);
  Require(family == "__secure-next-auth.session-token.0" && token.empty(), "auth cookie has no token");
  CookieSplitName("_ga_ABCDE12345", family, token);
  Require(token.empty(), "10 mixed chars is not an id");
  CookieSplitName("x_12345", family, token);
  Require(token.empty(), "short digits");
  CookieSplitName("uid_1699999999", family, token);
  Require(token == "1699999999", "8+ digits");
  CookieSplitName("t_0123456789abcdef0", family, token);
  Require(token == "0123456789abcdef0", "16+ hex");
  CookieSplitName("deadbeef", family, token);
  Require(token.empty(), "8 hex letters is not an id");
}

void TestChatgptProfile() {
  const auto cookies = ChatgptProfile(59);
  Require(cookies.size() == 149, "149 cookies");
  const CookiePlan plan = PlanCookieEvictions(cookies, CookieGuardConfig{});
  Require(plan.evict.size() == 98, "evicts 49 oldest units");
  for (const size_t i : plan.evict) Require(i >= 31, "base and login cookies are never evicted");
  std::set<std::string> keptTokens;
  size_t kept = 0;
  for (size_t i = 0; i < cookies.size(); ++i) {
    if (Contains(plan.evict, i)) continue;
    ++kept;
    if (i >= 31) {
      std::string family, token;
      CookieSplitName(cookies[i].name, family, token);
      keptTokens.insert(token);
    }
  }
  Require(kept == 51, "31 base + 10 newest units");
  Require(keptTokens.size() == 10, "ten units kept as pairs");
  for (unsigned i = 49; i < 59; ++i) Require(keptTokens.count(Uuid(i)) == 1, "newest conversations kept");
  const auto* report = Find(plan, "chatgpt.com");
  Require(report && report->evictUnits == 49 && report->evictCookies == 98, "site report");
}

void TestDisabledAndWithinLimit() {
  const auto cookies = ChatgptProfile(5);
  Require(cookies.size() == 41, "41 cookies");
  CookieGuardConfig config;
  config.maxPerSite = 41;
  Require(PlanCookieEvictions(cookies, config).evict.empty(), "at the limit is fine");
  config.maxPerSite = 0;
  Require(PlanCookieEvictions(cookies, config).evict.empty(), "0 = unlimited");
  config = CookieGuardConfig{};
  config.enabled = false;
  Require(PlanCookieEvictions(cookies, config).evict.empty(), "disabled");
}

void TestKeepFloorAndStopAtLimit() {
  CookieGuardConfig config;
  config.maxPerSite = 5;
  config.keepUnits = 4;
  CookiePlan plan = PlanCookieEvictions(ChatgptProfile(12), config);
  Require(plan.evict.size() == 16, "base alone exceeds the limit: only old units go, newest 4 stay");
  for (const size_t i : plan.evict) Require(i >= 31, "only temporary cookies");
  config.maxPerSite = 80;
  config.keepUnits = 2;
  plan = PlanCookieEvictions(ChatgptProfile(30), config);
  Require(plan.evict.size() == 12, "stops once the site is back under the limit");
  const auto* report = Find(plan, "chatgpt.com");
  Require(report && report->evictUnits == 6, "six units");
}

void TestProtectedSessionAndIncomplete() {
  std::vector<CookieInfo> list;
  const std::string d = "example.com";
  for (unsigned i = 0; i < 30; ++i) list.push_back(Persistent("auth_token_" + Uuid(i), d));
  for (unsigned i = 0; i < 30; ++i) list.push_back(Session("tab_state_" + Uuid(i + 100), d));
  for (unsigned i = 0; i < 2; ++i) list.push_back(Persistent("rare_" + Uuid(i + 200), d));
  for (unsigned i = 0; i < 30; ++i) list.push_back(Persistent("trace_" + Uuid(i + 300), d, "/", kNow + (i + 1) * kDay));
  list.push_back(Session("trace_extra_" + Uuid(300), d));
  CookieGuardConfig config;
  config.maxPerSite = 20;
  config.keepUnits = 3;
  const CookiePlan plan = PlanCookieEvictions(list, config);
  for (const size_t i : plan.evict) {
    Require(list[i].name.rfind("trace_", 0) == 0, "only the trace_ family is evictable");
    Require(list[i].name != "trace_" + Uuid(300), "a unit with a session member is kept whole");
  }
  Require(plan.evict.size() == 26, "30 units - 1 incomplete - 3 newest = 26");
}

void TestSitesAreIndependent() {
  auto cookies = ChatgptProfile(59);
  const size_t chatgptCount = cookies.size();
  for (unsigned i = 0; i < 30; ++i) cookies.push_back(Persistent("plain_" + std::to_string(i), "feishu.cn"));
  const CookiePlan plan = PlanCookieEvictions(cookies, CookieGuardConfig{});
  for (const size_t i : plan.evict) Require(i < chatgptCount, "fixed-name cookies of other sites are untouched");
  const auto* feishu = Find(plan, "feishu.cn");
  Require(feishu && feishu->count == 30, "feishu counted separately");
}

void TestStableOrder() {
  std::vector<CookieInfo> list;
  for (unsigned i = 0; i < 8; ++i) list.push_back(Persistent("k_" + Uuid(i), "chatgpt.com", "/", kNow + 100 * kDay));
  CookieGuardConfig config;
  config.maxPerSite = 4;
  config.keepUnits = 2;
  const CookiePlan first = PlanCookieEvictions(list, config);
  std::vector<CookieInfo> reversed(list.rbegin(), list.rend());
  const CookiePlan second = PlanCookieEvictions(reversed, config);
  std::set<std::string> a, b;
  for (const size_t i : first.evict) a.insert(list[i].name);
  for (const size_t i : second.evict) b.insert(reversed[i].name);
  Require(first.evict.size() == 4 && a == b, "same expiry: ordered by id, independent of input order");
}

void TestDefaults() {
  const CookieGuardConfig d;
  Require(d.enabled && !d.dryRun && d.maxPerSite == 20 && d.keepUnits == 10 && d.intervalSeconds == 300, "defaults");
}

}  // namespace

int main() {
  TestSiteKey();
  TestSplitName();
  TestChatgptProfile();
  TestDisabledAndWithinLimit();
  TestKeepFloorAndStopAtLimit();
  TestProtectedSessionAndIncomplete();
  TestSitesAreIndependent();
  TestStableOrder();
  TestDefaults();
  std::cout << "PASS cookie policy: site key; id tokens; oldest units first; base/login/session/protected names kept; newest units kept; stops at the limit\n";
}
