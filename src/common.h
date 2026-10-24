// 公共头：Windows/COM 基础设施与几个小工具。
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wrl.h>

#include <string>
#include <string_view>

namespace jisu {

using Microsoft::WRL::ComPtr;

// 以本进程创建时间为零点的毫秒计时；口径与可行性原型 jisu-probe 的 Clock 相同。
class ProcessClock {
 public:
  static void Init();
  static double NowMs();
  // 零点对应的 Unix 纪元毫秒：页面上报的 performance.timeOrigin 减去它即换算到同一零点。
  static double ZeroEpochMs();
};

std::string ToUtf8(std::wstring_view text);
std::wstring FromUtf8(std::string_view text);

// 小文件读写：读出原始字节（去掉 UTF-8 BOM）；写入先写临时文件再替换，读取方不会读到半截
bool ReadFileBytes(const std::wstring& path, std::string& out);
bool WriteFileAtomic(const std::wstring& path, const std::string& bytes);
bool EnsureDirectory(const std::wstring& path);  // 逐级创建
std::wstring ParentDirectory(const std::wstring& path);

// WebView2 getter 返回的 CoTaskMem 字符串。
class CoTaskString {
 public:
  CoTaskString() = default;
  CoTaskString(const CoTaskString&) = delete;
  CoTaskString& operator=(const CoTaskString&) = delete;
  ~CoTaskString() { CoTaskMemFree(value_); }

  LPWSTR* Put() {
    CoTaskMemFree(value_);
    value_ = nullptr;
    return &value_;
  }
  std::wstring Str() const { return value_ ? std::wstring(value_) : std::wstring(); }

 private:
  LPWSTR value_ = nullptr;
};

}  // namespace jisu
