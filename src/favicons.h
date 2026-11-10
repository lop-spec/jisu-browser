// Local-first favicon cache. No cookies, credentials or third-party favicon service.
// 同一资料的所有窗口共用一份：读写盘与WIC解码都在后台；不探测未访问的站点。
#pragma once
#include "common.h"
#include <objidl.h>
#include <cstdint>
#include <memory>
#include <vector>

namespace jisu {
std::wstring FaviconOrigin(const std::wstring& url);
uint64_t FaviconHash(const std::wstring& url);  // 0 = 不是 http(s) 网址
std::wstring FaviconKey(const std::wstring& url);
std::wstring FaviconKeyFromHash(uint64_t hash);
struct FaviconPixels {
  UINT width = 0, height = 0, stride = 0;
  DWORD decoderThread = 0;
  std::vector<unsigned char> bytes;
};
std::shared_ptr<const FaviconPixels> DecodedFavicon(const std::wstring& file);
class FaviconCache {
 public:
  ~FaviconCache();
  // 图标写好后给 window 发 message，wParam 为该图标的 FaviconHash
  void Start(const std::wstring& profile, HWND window, UINT message);
  void SetWindow(HWND window);  // 收通知的窗口关了，交给还开着的窗口
  // 后台线程持有自己的状态，退出最多等300ms，不阻塞界面。
  void Stop();
  bool Started() const { return state_ != nullptr; }
  std::wstring Directory() const;
  // 已缓存并在后台解码的图标；未命中返回空，不联网、不解析站点DNS。
  std::wstring Request(const std::wstring& url);
  // 页面给出的图标：和上次一样就不写；变了交给后台线程写盘
  void Store(const std::wstring& url, IStream* stream);

 private:
  struct State;
  std::shared_ptr<State> state_;
};
}  // namespace jisu
