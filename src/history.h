#pragma once
#include "common.h"
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>
namespace jisu {
// Lazy worker: history never loads or flushes on the browser's startup/UI path.
// 改动在内存里立即生效（查询马上看得到）；写盘合并：最后一次改动 1.5 秒后，或第一次未存改动 8 秒内必写，
// 停止时写完。SPA 频繁改网址时不再每 200ms 整份重写 History.json
class HistoryStore {
 public:
  ~HistoryStore();
  void Start(const std::wstring& file);
  void Visit(std::wstring url, std::wstring title, int64_t now = 0);
  void Title(const std::wstring& url, const std::wstring& title);
  void Remove(int64_t id);
  void Clear();
  std::string Query(std::wstring query, size_t offset = 0, size_t limit = 100);
  bool WaitIdle(unsigned timeoutMs = 5000);
  void Flush();  // 关机/注销：把未写的改动马上写盘并等它写完
  static std::wstring SafeUrl(std::wstring url);
 private:
  struct Entry { int64_t id, at; std::wstring url, title, search; };  // search：小写「标题 网址」，查询不再逐条转换
  struct Operation { int kind; Entry entry; };
  void Queue(Operation op);
  void Run();
  void Apply(Operation& op);
  bool SaveLocked(std::unique_lock<std::mutex>& lock);
  std::wstring file_;
  std::vector<Entry> entries_;
  std::deque<Operation> pending_;
  std::thread worker_;
  std::mutex mutex_;
  std::condition_variable cv_;
  bool ready_ = false, busy_ = false, stop_ = false, diskError_ = false, dirty_ = false, flushRequested_ = false;
  ULONGLONG firstDirty_ = 0, lastChange_ = 0;
  uint64_t savedGeneration_ = 0, generation_ = 0;
  int64_t nextId_ = 1;
};
}  // namespace jisu
