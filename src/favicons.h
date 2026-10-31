// Local-first favicon cache. No cookies, credentials or third-party favicon service.
// 同一资料的所有窗口共用一份：界面线程只查内存表，读写盘与抓取都在后台线程。
#pragma once
#include "common.h"
#include <objidl.h>
#include <cstdint>
#include <memory>

namespace jisu {
std::wstring FaviconOrigin(const std::wstring& url);
uint64_t FaviconHash(const std::wstring& url);  // 0 = 不是 http(s) 网址
std::wstring FaviconKey(const std::wstring& url);
std::wstring FaviconKeyFromHash(uint64_t hash);
class FaviconCache {
 public:
  ~FaviconCache();
  // 图标写好后给 window 发 message，wParam 为该图标的 FaviconHash
  void Start(const std::wstring& profile, HWND window, UINT message);
  void SetWindow(HWND window);  // 收通知的窗口关了，交给还开着的窗口
  // 不等进行中的网络请求：后台线程持有自己的状态，最多等 300ms 就放手，退出不再被抓图标的超时拖住
  void Stop();
  bool Started() const { return state_ != nullptr; }
  std::wstring Directory() const;
  // 已缓存的图标文件；没有时排队在后台尝试站点自己的 /favicon.ico，先返回空
  std::wstring Request(const std::wstring& url);
  // 页面给出的图标：和上次一样就不写；变了交给后台线程写盘
  void Store(const std::wstring& url, IStream* stream);

 private:
  struct State;
  std::shared_ptr<State> state_;
};
}  // namespace jisu
