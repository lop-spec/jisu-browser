#include "common.h"

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

}  // namespace jisu
