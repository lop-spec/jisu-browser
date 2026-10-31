#include "common.h"

#include <atomic>

namespace jisu {
namespace {

ULONGLONG g_zero = 0;  // 进程创建时刻，FILETIME 单位（100ns）

ULONGLONG ToTicks(const FILETIME& ft) {
  return (static_cast<ULONGLONG>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

}  // namespace

void ProcessClock::Init() {
  FILETIME creation{}, exitTime{}, kernel{}, user{};
  if (GetProcessTimes(GetCurrentProcess(), &creation, &exitTime, &kernel, &user)) {
    g_zero = ToTicks(creation);
  } else {
    FILETIME now{};
    GetSystemTimePreciseAsFileTime(&now);
    g_zero = ToTicks(now);
  }
}

double ProcessClock::NowMs() {
  FILETIME now{};
  GetSystemTimePreciseAsFileTime(&now);
  return static_cast<double>(static_cast<LONGLONG>(ToTicks(now) - g_zero)) / 10000.0;
}

double ProcessClock::ZeroEpochMs() {
  constexpr ULONGLONG kUnixEpoch = 116444736000000000ULL;  // 1970-01-01 的 FILETIME
  return static_cast<double>(g_zero - kUnixEpoch) / 10000.0;
}

std::string ToUtf8(std::wstring_view text) {
  if (text.empty()) {
    return {};
  }
  const int length = static_cast<int>(text.size());
  const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), length, nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.data(), length, out.data(), size, nullptr, nullptr);
  return out;
}

std::wstring FromUtf8(std::string_view text) {
  if (text.empty()) {
    return {};
  }
  const int length = static_cast<int>(text.size());
  const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), length, nullptr, 0);
  std::wstring out(static_cast<size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), length, out.data(), size);
  return out;
}

bool ReadFileBytes(const std::wstring& path, std::string& out) {
  out.clear();
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return false;
  }
  LARGE_INTEGER size{};
  bool ok = GetFileSizeEx(file, &size) && size.QuadPart < (64LL << 20);
  if (ok && size.QuadPart > 0) {
    out.resize(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    ok = ReadFile(file, out.data(), static_cast<DWORD>(out.size()), &read, nullptr) && read == out.size();
  }
  CloseHandle(file);
  if (!ok) {
    out.clear();
    return false;
  }
  if (out.size() >= 3 && static_cast<unsigned char>(out[0]) == 0xEF && static_cast<unsigned char>(out[1]) == 0xBB &&
      static_cast<unsigned char>(out[2]) == 0xBF) {
    out.erase(0, 3);
  }
  return true;
}

bool WriteFileAtomic(const std::wstring& path, const std::string& bytes, bool durable) {
  // 每次写用自己的临时名：两个线程/窗口同时写同一文件不会互相截断，失败也不留下别人的半截文件
  static std::atomic<unsigned> sequence{0};
  const std::wstring temp = path + L".tmp" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                            std::to_wstring(sequence.fetch_add(1, std::memory_order_relaxed));
  HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return false;
  }
  DWORD written = 0;
  const BOOL ok = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
  if (durable) {
    FlushFileBuffers(file);
  }
  CloseHandle(file);
  if (!ok || written != bytes.size() ||
      !MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | (durable ? MOVEFILE_WRITE_THROUGH : 0))) {
    DeleteFileW(temp.c_str());
    return false;
  }
  return true;
}

std::wstring TruncateUtf16(std::wstring text, size_t limit) {
  if (text.size() <= limit) {
    return text;
  }
  // 截断点落在高位代理之后就再退一位
  if (limit > 0 && text[limit - 1] >= 0xD800 && text[limit - 1] <= 0xDBFF) {
    --limit;
  }
  text.resize(limit);
  return text;
}

bool EnsureDirectory(const std::wstring& path) {
  if (path.empty()) {
    return false;
  }
  const DWORD attributes = GetFileAttributesW(path.c_str());
  if (attributes != INVALID_FILE_ATTRIBUTES) {
    return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
  }
  const std::wstring parent = ParentDirectory(path);
  if (!parent.empty() && parent != path && !EnsureDirectory(parent)) {
    return false;
  }
  return CreateDirectoryW(path.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

std::wstring ParentDirectory(const std::wstring& path) {
  size_t end = path.size();
  while (end > 0 && (path[end - 1] == L'\\' || path[end - 1] == L'/')) {
    --end;
  }
  const size_t slash = path.find_last_of(L"\\/", end == 0 ? 0 : end - 1);
  if (slash == std::wstring::npos || slash == 0) {
    return {};
  }
  // 「D:\」这样的根目录保留结尾反斜杠
  if (slash == 2 && path[1] == L':') {
    return path.substr(0, 3);
  }
  return path.substr(0, slash);
}

}  // namespace jisu
