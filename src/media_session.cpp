#include "media_session.h"
#include "json.h"
#include <chrono>
#include <algorithm>
#include <vector>
namespace jisu {
MediaSession::~MediaSession() { Stop(); }
bool MediaSession::Running() const { return process_ && WaitForSingleObject(process_, 0) == WAIT_TIMEOUT; }
bool MediaSession::Start(HWND surface, const std::wstring& directory, const std::wstring& profile) {
  if (process_) return Running();
  SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE}; HANDLE childIn = nullptr, childOut = nullptr;
  if (!CreatePipe(&childIn, &input_, &security, 65536) || !CreatePipe(&output_, &childOut, &security, 65536)) {
    if(childIn)CloseHandle(childIn); if(childOut)CloseHandle(childOut); Stop(); return false;
  }
  SetHandleInformation(input_, HANDLE_FLAG_INHERIT, 0); SetHandleInformation(output_, HANDLE_FLAG_INHERIT, 0);
  SIZE_T size = 0; InitializeProcThreadAttributeList(nullptr, 1, 0, &size); std::vector<BYTE> memory(size);
  STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof(startup); startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(memory.data());
  bool attributes = InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &size) != FALSE;
  HANDLE handles[]{childIn, childOut};
  attributes = attributes && UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles, sizeof(handles), nullptr, nullptr);
  startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
  startup.StartupInfo.wShowWindow = SW_HIDE; startup.StartupInfo.hStdInput = childIn; startup.StartupInfo.hStdOutput = childOut; startup.StartupInfo.hStdError = childOut;
  const auto exe = directory + L"\\jisu-media.exe";
  std::wstring command = L"\"" + exe + L"\" " + std::to_wstring(reinterpret_cast<uintptr_t>(surface));
  PROCESS_INFORMATION process{};
  // Job assignment occurs before the child resumes: no orphan on a parent crash.
  job_ = CreateJobObjectW(nullptr, nullptr);
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{}; limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  const bool jobReady = job_ && SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
  const bool created = jobReady && attributes && CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, TRUE,
      CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, nullptr, directory.c_str(), &startup.StartupInfo, &process);
  if (attributes) DeleteProcThreadAttributeList(startup.lpAttributeList);
  CloseHandle(childIn); CloseHandle(childOut);
  if (!created) { Stop(); return false; }
  if (!AssignProcessToJobObject(job_, process.hProcess)) { TerminateProcess(process.hProcess, 1); CloseHandle(process.hThread); CloseHandle(process.hProcess); Stop(); return false; }
  process_ = process.hProcess; ResumeThread(process.hThread); CloseHandle(process.hThread);
  transport_ = std::thread([this] { Run(); });
  auto init=json::Value::MakeObject();init.Set("op",json::Value::String("init"));init.Set("profile",json::Value::String(ToUtf8(profile)));Send(json::Serialize(init));return true;
}
void MediaSession::Send(std::string json) {
  if (json.size() > 60000) return;
  { std::lock_guard lock(mutex_); if (stop_ || commands_.size() >= 64) return; commands_.push_back(std::move(json)); } cv_.notify_all();
}
std::deque<std::string> MediaSession::Poll() { std::lock_guard lock(mutex_); std::deque<std::string> out; out.swap(events_); return out; }
void MediaSession::Run() {
  std::string partial; char bytes[16384];
  for (;;) {
    std::deque<std::string> send;
    { std::unique_lock lock(mutex_); cv_.wait_for(lock, std::chrono::milliseconds(50), [this] { return stop_ || !commands_.empty(); }); if (stop_) break; send.swap(commands_); }
    bool ok = true;
    for (auto& item : send) { item += '\n'; DWORD wrote = 0; if (!WriteFile(input_, item.data(), static_cast<DWORD>(item.size()), &wrote, nullptr) || wrote != item.size()) { ok = false; break; } }
    DWORD available = 0;
    while (ok && PeekNamedPipe(output_, nullptr, 0, nullptr, &available, nullptr) && available) {
      DWORD read = 0; if (!ReadFile(output_, bytes, (std::min)(available, DWORD(sizeof(bytes))), &read, nullptr)) { ok = false; break; }
      partial.append(bytes, read); if (partial.size() > 262144) { ok = false; break; }
      size_t newline;
      while ((newline = partial.find('\n')) != std::string::npos) {
        auto line = partial.substr(0, newline); partial.erase(0, newline + 1);
        std::lock_guard lock(mutex_); if (events_.size() >= 32) events_.pop_front(); events_.push_back(std::move(line));
      }
    }
    if (!ok || !Running()) break;
  }
  std::lock_guard lock(mutex_); events_.push_back("{\"type\":\"player.stopped\"}");
}
void MediaSession::Stop() {
  if(Running()&&transport_.joinable()){Send("{\"op\":\"stop\"}");WaitForSingleObject(process_,200);}
  { std::lock_guard lock(mutex_); stop_ = true; } cv_.notify_all();
  // Only the dedicated media child is terminated, never the browser or unrelated players.
  if (job_) { CloseHandle(job_); job_ = nullptr; }
  if (transport_.joinable()) { CancelSynchronousIo(transport_.native_handle()); transport_.join(); }
  for (auto handle : {input_, output_, process_}) if (handle) CloseHandle(handle);
  input_ = output_ = process_ = nullptr;
}
}  // namespace jisu
