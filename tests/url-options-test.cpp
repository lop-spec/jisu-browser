// 地址栏输入规整、--single-argument、后台写盘、UTF-16 截断：不开浏览器、不建窗口。
#include "browser_url.h"
#include "file_writer.h"
#include "options.h"

#include <cstdio>
#include <stdexcept>
#include <string>

using namespace jisu;
namespace {
int failures = 0;
void Check(bool ok, const char* label) {
  if (!ok) {
    std::printf("FAIL %s\n", label);
    ++failures;
  }
}
bool IsSearch(const std::wstring& url) { return url.starts_with(kSearchPrefix); }
}  // namespace

int main() {
  // 数字不是 IPv4：搜索；四段合法 IPv4、localhost、IPv6 字面地址：http
  Check(IsSearch(ToNavigableUrl(L"3.14")), "3.14 searches");
  Check(IsSearch(ToNavigableUrl(L"1.2")), "1.2 searches");
  Check(IsSearch(ToNavigableUrl(L"1.2.3.256")), "out-of-range octet searches");
  Check(ToNavigableUrl(L"192.168.1.1:8080/x") == L"http://192.168.1.1:8080/x", "dotted quad");
  Check(ToNavigableUrl(L"localhost:3000") == L"http://localhost:3000", "localhost");
  Check(ToNavigableUrl(L"[::1]:8080/a") == L"http://[::1]:8080/a", "ipv6 literal");
  Check(ToNavigableUrl(L"example.com/a?b") == L"https://example.com/a?b", "domain");
  Check(ToNavigableUrl(L"example.com?q=1") == L"https://example.com?q=1", "domain with query");
  Check(IsSearch(ToNavigableUrl(L"hello world")), "words search");
  // 本地文件与其他可直接打开的地址
  Check(ToNavigableUrl(L"C:\\Users\\a b\\x#1.html") == L"file:///C:/Users/a%20b/x%231.html", "windows path");
  Check(ToNavigableUrl(L"\\\\server\\share\\x.html") == L"file://server/share/x.html", "unc path");
  Check(ToNavigableUrl(L"chrome-extension://abc/popup.html") == L"chrome-extension://abc/popup.html", "extension url");
  Check(ToNavigableUrl(L"mailto:a@example.com") == L"mailto:a@example.com", "mailto");
  Check(ToNavigableUrl(L"jisu://extensions") == kExtensionsUrl && DisplayUrl(kExtensionsUrl) == L"jisu://extensions",
        "jisu://extensions round trip");
  Check(ToNavigableUrl(L"jisu://bookmarks#import") == std::wstring(kBookmarksUrl) + L"#import", "bookmarks fragment");
  Check(IsInternalPage(kNtpUrl) && IsInternalPage(L"https://favicons.jisu.example/x.png") &&
            !IsInternalPage(L"https://example.com/"),
        "internal pages");

  // --single-argument：之后的全部原文是一个网址，夹带的启动参数不生效
  const Options injected = ParseOptions(
      L"\"C:\\jisu\\jisu-browser.exe\" --single-argument https://a.example/x\" --browser-args \"--remote-debugging-port=9222");
  Check(injected.singleArgument && injected.browserArgs.empty() && injected.userDataFolder.find(L"JisuBrowser") != std::wstring::npos,
        "single-argument blocks option injection");
  Check(injected.startUrl == L"https://a.example/x\" --browser-args \"--remote-debugging-port=9222", "single-argument keeps raw text");
  const Options path = ParseOptions(L"\"C:\\jisu\\jisu-browser.exe\" --no-activate --single-argument C:\\My Pages\\a.html");
  Check(path.noActivate && path.startUrl == L"C:\\My Pages\\a.html", "single-argument file path with spaces");
  const Options quoted = ParseOptions(L"jisu.exe --single-argument \"https://b.example/\"");
  Check(quoted.startUrl == L"https://b.example/", "single-argument strips one pair of quotes");
  const Options normal = ParseOptions(L"jisu.exe --udf \"D:\\p\" https://c.example/");
  Check(!normal.singleArgument && normal.startUrl == L"https://c.example/" && normal.userDataFolder == L"D:\\p", "plain arguments");

  // 截断不切开代理对
  const std::wstring emoji = L"ab\U0001F600cd";
  Check(TruncateUtf16(emoji, 3) == L"ab", "truncate before surrogate pair");
  Check(TruncateUtf16(emoji, 4) == L"ab\U0001F600", "truncate after surrogate pair");
  Check(MenuText(std::wstring(59, L'x') + L"\U0001F600" + L"tail") == std::wstring(59, L'x') + L"…", "menu text keeps pairs whole");

  // 后台写盘：同一文件只留最新一份，Flush 后读得到
  wchar_t temp[MAX_PATH]{};
  GetTempPathW(MAX_PATH, temp);
  const std::wstring file = std::wstring(temp) + L"jisu-writer-test-" + std::to_wstring(GetCurrentProcessId()) + L"\\a.json";
  {
    FileWriter writer;
    for (int i = 0; i < 20; ++i) writer.Write(file, "{\"n\":" + std::to_string(i) + "}", i % 2 == 0);
    Check(writer.Flush(5000), "writer flush");
    std::string bytes;
    Check(ReadFileBytes(file, bytes) && bytes == "{\"n\":19}", "latest content wins");
  }
  DeleteFileW(file.c_str());
  RemoveDirectoryW(ParentDirectory(file).c_str());

  if (!failures) std::puts("PASS address-bar normalization; --single-argument; surrogate-safe truncation; background writer");
  return failures ? 1 : 0;
}
