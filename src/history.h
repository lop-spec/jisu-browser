#pragma once
#include "common.h"
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>
namespace jisu {
// Lazy worker: history never loads or flushes on the browser's startup/UI path.
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
  static std::wstring SafeUrl(std::wstring url);
 private:
  struct Entry { int64_t id, at; std::wstring url, title; };
  struct Operation { int kind; Entry entry; };
  void Queue(Operation op);
  void Run();
  std::wstring file_;
  std::vector<Entry> entries_;
  std::deque<Operation> pending_;
  std::thread worker_;
  std::mutex mutex_;
  std::condition_variable cv_;
  bool ready_ = false, busy_ = false, stop_ = false;
  int64_t nextId_ = 1;
};
}  // namespace jisu
