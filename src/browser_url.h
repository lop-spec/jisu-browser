// 外壳页面与网址的小工具：BrowserWindow 的几个源文件共用
#pragma once

#include "common.h"

#include <algorithm>
#include <cwctype>
#include <string>

namespace jisu {

inline constexpr wchar_t kAppName[] = L"极速浏览器";
// 新标签页与书签管理页经虚拟主机映射到随包的 ntp\ 目录；.example 是保留域名，不会和真实网站冲突
inline constexpr wchar_t kNtpHost[] = L"newtab.jisu.example";
inline constexpr wchar_t kNtpOrigin[] = L"https://newtab.jisu.example/";
inline constexpr wchar_t kNtpUrl[] = L"https://newtab.jisu.example/index.html";
inline constexpr wchar_t kBookmarksUrl[] = L"https://newtab.jisu.example/bookmarks.html";
inline constexpr wchar_t kBookmarksDisplay[] = L"jisu://bookmarks";
inline constexpr wchar_t kExtensionsUrl[] = L"https://newtab.jisu.example/extensions.html";
inline constexpr wchar_t kSearchPrefix[] = L"https://www.bing.com/search?q=";

inline bool IsNewTabPageUrl(const std::wstring& url) {
  return url.empty() || url == L"about:blank" || url == kNtpOrigin || url.starts_with(kNtpUrl);
}

inline bool IsBookmarksPageUrl(const std::wstring& url) { return url.starts_with(kBookmarksUrl); }

inline bool IsWebUrl(const std::wstring& url) { return url.starts_with(L"http://") || url.starts_with(L"https://"); }

// 外壳自己的页面（新标签页、书签、历史、扩展管理、播放器）与图标缓存：只能由外壳打开，网页不能跳过去
inline bool IsInternalPage(const std::wstring& url) {
  return url.starts_with(kNtpOrigin) || url.starts_with(L"https://favicons.jisu.example/");
}

// 能收藏的页面：网页，不含外壳自己的新标签页与书签管理页
inline bool IsStarrable(const std::wstring& url) { return IsWebUrl(url) && !url.starts_with(kNtpOrigin); }

inline std::wstring DisplayUrl(const std::wstring& url) {
  if (url == L"https://newtab.jisu.example/history.html") return L"jisu://history";
  if (url == L"https://newtab.jisu.example/player.html") return L"jisu://player";
  if (url == kExtensionsUrl) return L"jisu://extensions";
  if (IsBookmarksPageUrl(url)) {
    return kBookmarksDisplay;
  }
  return IsNewTabPageUrl(url) ? std::wstring() : url;
}

inline std::wstring Trim(const std::wstring& text) {
  const size_t first = text.find_first_not_of(L" \t\r\n");
  if (first == std::wstring::npos) {
    return {};
  }
  const size_t last = text.find_last_not_of(L" \t\r\n");
  return text.substr(first, last - first + 1);
}

// 菜单项文字：& 要写成 &&，太长的截断（不切开代理对）
inline std::wstring MenuText(const std::wstring& text) {
  std::wstring shown = text.size() > 60 ? TruncateUtf16(text, 59) + L"…" : text;
  std::wstring out;
  for (const wchar_t c : shown) {
    if (c == L'&') {
      out += L"&&";
    } else if (c == L'\t' || c == L'\r' || c == L'\n') {
      out.push_back(L' ');
    } else {
      out.push_back(c);
    }
  }
  return out.empty() ? std::wstring(L"（无标题）") : out;
}

// 四段、每段 0–255 的点分 IPv4（"3.14"、"1.2" 这类数字不算，交给搜索）
inline bool IsDottedQuad(const std::wstring& host) {
  int parts = 0;
  size_t start = 0;
  while (start <= host.size()) {
    const size_t dot = host.find(L'.', start);
    const std::wstring part = host.substr(start, dot == std::wstring::npos ? std::wstring::npos : dot - start);
    if (part.empty() || part.size() > 3 || part.find_first_not_of(L"0123456789") != std::wstring::npos ||
        std::stoi(part) > 255) {
      return false;
    }
    ++parts;
    if (dot == std::wstring::npos) break;
    start = dot + 1;
  }
  return parts == 4;
}

// 本地路径（资源管理器打开 .html、命令行传文件）→ file:/// 网址
inline bool IsLocalPath(const std::wstring& input) {
  return (input.size() >= 3 && std::iswalpha(input[0]) && input[1] == L':' && (input[2] == L'\\' || input[2] == L'/')) ||
         input.starts_with(L"\\\\");
}

inline std::wstring FileUrl(const std::wstring& path) {
  std::wstring out = path.starts_with(L"\\\\") ? L"file:" : L"file:///";
  for (const wchar_t c : path) {
    switch (c) {
      case L'\\': out.push_back(L'/'); break;
      case L' ': out += L"%20"; break;
      case L'#': out += L"%23"; break;
      case L'%': out += L"%25"; break;
      case L'?': out += L"%3F"; break;
      default: out.push_back(c);
    }
  }
  return out;
}

inline std::wstring UrlEncode(const std::wstring& text) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::wstring out;
  for (const char ch : ToUtf8(text)) {
    const unsigned char c = static_cast<unsigned char>(ch);
    const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
                            c == '_' || c == '.' || c == '~';
    if (unreserved) {
      out.push_back(static_cast<wchar_t>(c));
    } else if (c == ' ') {
      out.push_back(L'+');
    } else {
      out.push_back(L'%');
      out.push_back(static_cast<wchar_t>(kHex[c >> 4]));
      out.push_back(static_cast<wchar_t>(kHex[c & 0x0F]));
    }
  }
  return out;
}

// 地址栏输入 → 网址：带协议的原样打开；像域名的补 https://；localhost 与 IPv4 补 http://；其余交给搜索
inline std::wstring ToNavigableUrl(const std::wstring& input) {
  std::wstring lower = input;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
  if (lower == kBookmarksDisplay || lower.starts_with(std::wstring(kBookmarksDisplay) + L"#")) {
    return kBookmarksUrl + input.substr(wcslen(kBookmarksDisplay));
  }
  if (lower == L"jisu://history") return L"https://newtab.jisu.example/history.html";
  if (lower == L"jisu://player") return L"https://newtab.jisu.example/player.html";
  if (lower == L"jisu://extensions") return kExtensionsUrl;
  if (lower == L"jisu://newtab") {
    return kNtpUrl;
  }
  static constexpr const wchar_t* kSchemes[] = {L"http://", L"https://",     L"file://", L"about:", L"data:",
                                                L"edge://", L"view-source:", L"chrome-extension://",
                                                L"mailto:", L"tel:"};
  for (const wchar_t* scheme : kSchemes) {
    if (lower.starts_with(scheme)) {
      return input;
    }
  }
  if (IsLocalPath(input)) {
    return FileUrl(input);
  }
  if (input.find_first_of(L" \t") == std::wstring::npos) {
    // [::1]:8080 这类 IPv6 字面地址
    if (lower.starts_with(L"[")) {
      const size_t close = lower.find(L']');
      if (close != std::wstring::npos && close > 1 && lower.substr(1, close - 1).find(L':') != std::wstring::npos &&
          lower.substr(1, close - 1).find_first_not_of(L"0123456789abcdef:.%") == std::wstring::npos) {
        return L"http://" + input;
      }
    }
    const std::wstring host = lower.substr(0, lower.find_first_of(L":/?#"));
    if (host == L"localhost" || IsDottedQuad(host)) {
      return L"http://" + input;
    }
    // 像域名：至少一个点，首尾不是点，最后一段不全是数字（"1.5" "3.14" 走搜索）
    const size_t lastDot = host.rfind(L'.');
    if (lastDot != std::wstring::npos && host.front() != L'.' && host.back() != L'.' &&
        host.substr(lastDot + 1).find_first_not_of(L"0123456789") != std::wstring::npos) {
      return L"https://" + input;
    }
  }
  return kSearchPrefix + UrlEncode(input);
}

}  // namespace jisu
