// 后台写盘：书签、会话这类在界面线程上改、要写回磁盘的文件交给它。同一文件只保留最新一份内容，
// 写在后台线程上（含 fsync），界面线程不再为每次拖动、重命名卡一次磁盘；Flush 等全部写完（退出、关机前）。
#pragma once
#include "common.h"

#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace jisu {

class FileWriter {
 public:
  FileWriter() = default;
  FileWriter(const FileWriter&) = delete;
  FileWriter& operator=(const FileWriter&) = delete;
  ~FileWriter() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stop_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();  // 停止前把排着的都写完
  }

  void Write(const std::wstring& path, std::string bytes, bool durable) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stop_) return;
      pending_[path] = Item{std::move(bytes), durable};
      if (!worker_.joinable()) worker_ = std::thread([this] { Run(); });
    }
    cv_.notify_all();
  }

  // 等排着的写完（最多 timeoutMs）
  bool Flush(unsigned timeoutMs = 5000) {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this] { return pending_.empty() && !busy_; });
  }

 private:
  struct Item {
    std::string bytes;
    bool durable = true;
  };

  void Run() {
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
      cv_.wait(lock, [this] { return stop_ || !pending_.empty(); });
      if (pending_.empty()) {
        if (stop_) return;
        continue;
      }
      auto it = pending_.begin();
      const std::wstring path = it->first;
      Item item = std::move(it->second);
      pending_.erase(it);
      busy_ = true;
      lock.unlock();
      EnsureDirectory(ParentDirectory(path));
      WriteFileAtomic(path, item.bytes, item.durable);
      lock.lock();
      busy_ = false;
      cv_.notify_all();
    }
  }

  std::mutex mutex_;
  std::condition_variable cv_;
  std::map<std::wstring, Item> pending_;
  std::thread worker_;
  bool busy_ = false;
  bool stop_ = false;
};

}  // namespace jisu
