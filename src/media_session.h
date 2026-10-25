#pragma once
#include "common.h"
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
namespace jisu {
// One optional child process per player tab. No process, pipe or thread when unused.
class MediaSession {
 public:
  ~MediaSession();
  bool Start(HWND surface, const std::wstring& directory, const std::wstring& profile);
  void Send(std::string json);
  std::deque<std::string> Poll();
  void Stop();
  bool Running() const;
 private:
  void Run();
  HANDLE process_ = nullptr, job_ = nullptr, input_ = nullptr, output_ = nullptr;
  std::thread transport_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<std::string> commands_, events_;
  bool stop_ = false;
};
}  // namespace jisu
