// 启动时间线：各阶段距进程创建的毫秒数，写成 JSON 交给 measure\ 脚本汇总。
#pragma once

#include "common.h"

#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace jisu {

class Metrics {
 public:
  // 第一次调用须在 UI 线程（wWinMain 开头），用于区分各时间点记在哪个线程。
  static Metrics& Get();

  void Mark(const char* name) { MarkAt(name, ProcessClock::NowMs()); }
  void MarkAt(const char* name, double ms);
  bool Has(const char* name) const;
  void SetText(const char* key, std::string utf8);
  void SetNumber(const char* key, double value);
  bool Write(const std::wstring& path) const;

 private:
  struct Entry {
    std::string name;
    double ms = 0;
    bool uiThread = true;
  };

  Metrics();

  mutable std::mutex mutex_;
  std::vector<Entry> marks_;
  std::vector<std::pair<std::string, std::string>> texts_;
  std::vector<std::pair<std::string, double>> numbers_;
  DWORD uiThread_ = 0;
};

}  // namespace jisu
