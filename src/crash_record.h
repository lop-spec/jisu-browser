#pragma once
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cwchar>
#include <string>
#ifndef JISU_VERSION
#define JISU_VERSION "local"
#endif
#ifndef JISU_COMMIT
#define JISU_COMMIT "local"
#endif
namespace jisu {
inline wchar_t crashRecordFile[32768]{};
inline LONG WINAPI RecordShellCrash(EXCEPTION_POINTERS* info) {
  if (!info || !info->ExceptionRecord || !crashRecordFile[0]) return EXCEPTION_CONTINUE_SEARCH;
  HMODULE module = nullptr; wchar_t path[1024]{};
  const auto address = info->ExceptionRecord->ExceptionAddress;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCWSTR>(address), &module);
  GetModuleFileNameW(module, path, 1024);
  const auto* leaf = wcsrchr(path, L'\\'); leaf = leaf ? leaf + 1 : path;
  SYSTEMTIME time{}; GetSystemTime(&time);
  char row[2048]{};
  const int n = snprintf(row, sizeof(row), "%04u-%02u-%02uT%02u:%02u:%02uZ pid=%lu version=%s commit=%s exception=0x%08lX module=%ls offset=0x%llX\r\n",
      time.wYear,time.wMonth,time.wDay,time.wHour,time.wMinute,time.wSecond,GetCurrentProcessId(),JISU_VERSION,JISU_COMMIT,
      info->ExceptionRecord->ExceptionCode,leaf,static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(address)-reinterpret_cast<uintptr_t>(module)));
  HANDLE file = CreateFileW(crashRecordFile, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file != INVALID_HANDLE_VALUE) { DWORD written; WriteFile(file,row,static_cast<DWORD>(n>0 && n<sizeof(row)?n:sizeof(row)-1),&written,nullptr); CloseHandle(file); }
  return EXCEPTION_CONTINUE_SEARCH;
}
inline void InstallCrashRecord(const std::wstring& profile) {
  const auto path = profile + L"\\Crash.log";
  if (path.size() < 32768) wcscpy_s(crashRecordFile, path.c_str());
  SetUnhandledExceptionFilter(RecordShellCrash);
}
} // namespace jisu
