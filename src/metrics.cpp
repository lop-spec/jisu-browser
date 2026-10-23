#include "metrics.h"

#include <cmath>
#include <cstdio>

namespace jisu {
namespace {

void AppendString(std::string& out, std::string_view text) {
  out.push_back('"');
  for (const char ch : text) {
    const unsigned char c = static_cast<unsigned char>(ch);
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (c < 0x20) {
          char escaped[8];
          snprintf(escaped, sizeof(escaped), "\\u%04x", c);
          out += escaped;
        } else {
          out.push_back(ch);
        }
    }
  }
  out.push_back('"');
}

void AppendNumber(std::string& out, double value) {
  if (!std::isfinite(value)) {
    out += "null";
    return;
  }
  char buffer[40];
  const int length = snprintf(buffer, sizeof(buffer), "%.2f", value);
  if (length > 0) {
    out.append(buffer, static_cast<size_t>(length));
  }
}

}  // namespace

Metrics::Metrics() : uiThread_(GetCurrentThreadId()) {}

Metrics& Metrics::Get() {
  static Metrics instance;
  return instance;
}

void Metrics::MarkAt(const char* name, double ms) {
  const bool ui = GetCurrentThreadId() == uiThread_;
  std::lock_guard<std::mutex> lock(mutex_);
  marks_.push_back(Entry{name, ms, ui});
}

bool Metrics::Has(const char* name) const {
  std::lock_guard<std::mutex> lock(mutex_);
  for (const Entry& entry : marks_) {
    if (entry.name == name) {
      return true;
    }
  }
  return false;
}

void Metrics::SetText(const char* key, std::string utf8) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& item : texts_) {
    if (item.first == key) {
      item.second = std::move(utf8);
      return;
    }
  }
  texts_.emplace_back(key, std::move(utf8));
}

void Metrics::SetNumber(const char* key, double value) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& item : numbers_) {
    if (item.first == key) {
      item.second = value;
      return;
    }
  }
  numbers_.emplace_back(key, value);
}

bool Metrics::Write(const std::wstring& path) const {
  std::string json;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    json.reserve(8192);
    json += "{\n  \"schema\": 1";
    for (const auto& [key, value] : texts_) {
      json += ",\n  ";
      AppendString(json, key);
      json += ": ";
      AppendString(json, value);
    }
    for (const auto& [key, value] : numbers_) {
      json += ",\n  ";
      AppendString(json, key);
      json += ": ";
      AppendNumber(json, value);
    }
    // t：每个时间点第一次出现的值，方便脚本直接取；timeline：按发生顺序的全部记录
    json += ",\n  \"t\": {";
    std::vector<std::string> seen;
    for (const Entry& entry : marks_) {
      bool duplicate = false;
      for (const std::string& name : seen) {
        if (name == entry.name) {
          duplicate = true;
          break;
        }
      }
      if (duplicate) {
        continue;
      }
      json += seen.empty() ? "\n    " : ",\n    ";
      seen.push_back(entry.name);
      AppendString(json, entry.name);
      json += ": ";
      AppendNumber(json, entry.ms);
    }
    json += "\n  },\n  \"timeline\": [";
    for (size_t i = 0; i < marks_.size(); ++i) {
      const Entry& entry = marks_[i];
      json += i == 0 ? "\n    " : ",\n    ";
      json += "{\"name\": ";
      AppendString(json, entry.name);
      json += ", \"ms\": ";
      AppendNumber(json, entry.ms);
      json += entry.uiThread ? ", \"thread\": \"ui\"}" : ", \"thread\": \"worker\"}";
    }
    json += "\n  ]\n}\n";
  }

  // 先写临时文件再替换，读取方不会读到半截
  const std::wstring temp = path + L".tmp";
  HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return false;
  }
  DWORD written = 0;
  const BOOL ok = WriteFile(file, json.data(), static_cast<DWORD>(json.size()), &written, nullptr);
  CloseHandle(file);
  if (!ok || written != json.size()) {
    return false;
  }
  return MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
}

}  // namespace jisu
