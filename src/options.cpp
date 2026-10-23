#include "options.h"

#include "common.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace jisu {
namespace {

// 按 MSVC 运行库的规则拆命令行。不用 CommandLineToArgvW：它在 shell32.dll 里，加载这个 DLL 会拖慢首帧。
std::vector<std::wstring> SplitCommandLine(std::wstring_view line) {
  std::vector<std::wstring> args;
  std::wstring current;
  bool inQuotes = false;
  bool hasToken = false;
  size_t i = 0;
  while (i < line.size()) {
    const wchar_t c = line[i];
    if (c == L'\\') {
      size_t count = 0;
      while (i < line.size() && line[i] == L'\\') {
        ++count;
        ++i;
      }
      if (i < line.size() && line[i] == L'"') {
        current.append(count / 2, L'\\');
        if (count % 2 == 1) {
          current.push_back(L'"');
          ++i;
        }
      } else {
        current.append(count, L'\\');
      }
      hasToken = true;
      continue;
    }
    if (c == L'"') {
      inQuotes = !inQuotes;
      hasToken = true;
      ++i;
      continue;
    }
    if ((c == L' ' || c == L'\t') && !inQuotes) {
      if (hasToken) {
        args.push_back(std::move(current));
        current.clear();
        hasToken = false;
      }
      ++i;
      continue;
    }
    current.push_back(c);
    hasToken = true;
    ++i;
  }
  if (hasToken) {
    args.push_back(std::move(current));
  }
  return args;
}

std::wstring DefaultUserDataFolder() {
  wchar_t buffer[MAX_PATH];
  const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, MAX_PATH);
  if (length == 0 || length >= MAX_PATH) {
    return {};  // 交给 WebView2 用默认位置
  }
  return std::wstring(buffer, length) + L"\\JisuBrowser\\User Data";
}

}  // namespace

Options ParseOptions(const wchar_t* commandLine) {
  Options options;
  options.commandLine = commandLine ? commandLine : L"";
  const std::vector<std::wstring> args = SplitCommandLine(options.commandLine);
  // args[0] 是程序路径
  for (size_t i = 1; i < args.size(); ++i) {
    const std::wstring& arg = args[i];
    auto next = [&]() -> std::wstring { return i + 1 < args.size() ? args[++i] : std::wstring(); };
    if (arg == L"--udf") {
      options.userDataFolder = next();
    } else if (arg == L"--metrics") {
      options.metricsPath = next();
    } else if (arg == L"--exit-after-metrics") {
      options.exitAfterMetrics = true;
    } else if (arg == L"--no-activate") {
      options.noActivate = true;
    } else if (arg == L"--bounds") {
      const std::wstring value = next();
      options.hasBounds = swscanf_s(value.c_str(), L"%d,%d,%d,%d", &options.x, &options.y, &options.width,
                                    &options.height) == 4 &&
                          options.width > 0 && options.height > 0;
    } else if (arg == L"--rt") {
      options.renderTarget = next() == L"sw" ? RenderTargetKind::kSoftware : RenderTargetKind::kHardware;
    } else if (arg == L"--warm-d3d") {
      options.warmD3D = next() != L"0";
    } else if (arg == L"--env") {
      options.envStart = next() == L"late" ? EnvStart::kLate : EnvStart::kEarly;
    } else if (arg == L"--ntp") {
      options.ntp = next() == L"inline" ? NtpSource::kInline : NtpSource::kFolder;
    } else if (arg == L"--no-spare") {
      options.spare = false;
    } else if (arg == L"--tracking-prevention") {
      options.trackingPrevention = next() != L"0";
    } else if (arg == L"--browser-args") {
      options.browserArgs = next();
    } else if (arg == L"--preheat") {
      const int seconds = _wtoi(next().c_str());
      options.preheatSeconds = seconds < 0 ? 0 : (seconds > 86400 ? 86400 : seconds);
    } else if (!arg.starts_with(L"--") && options.startUrl.empty()) {
      options.startUrl = arg;
    }
  }
  if (options.userDataFolder.empty()) {
    options.userDataFolder = DefaultUserDataFolder();
  }
  return options;
}

}  // namespace jisu
