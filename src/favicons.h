// Local-first favicon cache. No cookies, credentials or third-party favicon service.
#pragma once
#include "common.h"
#include <objidl.h>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace jisu {
std::wstring FaviconOrigin(const std::wstring& url);
std::wstring FaviconKey(const std::wstring& url);
class FaviconCache {
 public:
  ~FaviconCache();
  void Start(const std::wstring& profile, HWND window, UINT message);
  void Stop();
  std::wstring Request(const std::wstring& url);
  void Store(const std::wstring& url, IStream* stream);
 private:
  void Worker();
  std::wstring directory_;
  HWND window_ = nullptr;
  UINT message_ = 0;
  std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<std::wstring> queue_;
  std::set<std::wstring> requested_;
  bool stop_ = false;
  std::thread worker_;
};
}  // namespace jisu
