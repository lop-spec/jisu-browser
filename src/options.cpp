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

std::wstring AppDataRoot() {
  wchar_t buffer[MAX_PATH];
  const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, MAX_PATH);
  if (length == 0 || length >= MAX_PATH) {
    return {};  // 交给 WebView2 用默认位置
  }
  return std::wstring(buffer, length) + L"\\JisuBrowser";
}

}  // namespace

Options ParseOptions(const wchar_t* commandLine) {
  Options options;
  options.commandLine = commandLine ? commandLine : L"";
  const std::vector<std::wstring> args = SplitCommandLine(options.commandLine);
  bool agentConfigExplicit = false;
  // args[0] 是程序路径
  for (size_t i = 1; i < args.size(); ++i) {
    const std::wstring& arg = args[i];
    auto next = [&]() -> std::wstring { return i + 1 < args.size() ? args[++i] : std::wstring(); };
    if (arg == L"--app" || arg.starts_with(L"--app=")) {
      options.appMode = true;
      options.startUrl = arg == L"--app" ? next() : arg.substr(6);
    } else if (arg == L"--app-id") {
      options.appId = next();
    } else if (arg == L"--app-title") {
      options.appTitle = next();
    } else if (arg == L"--app-icon") {
      options.appIcon = next();
    } else if (arg == L"--udf") {
      options.userDataFolder = next();
      options.defaultUserData = false;
    } else if (arg == L"--agent-config") {
      options.agentConfigPath = next();
      agentConfigExplicit = true;
    } else if (arg == L"--no-agent") {
      options.noAgent = true;
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
      options.renderTarget = next() == L"hw" ? RenderTargetKind::kHardware : RenderTargetKind::kSoftware;
    } else if (arg == L"--warm-d3d") {
      options.warmD3D = next() != L"0" ? 1 : 0;
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
  if (options.appMode) {
    options.noAgent = true; // Never competes with the daily browser's broker leases.
    options.spare = false;
    if (options.appId.empty()) options.appId = L"web-app";
  }
  const std::wstring root = AppDataRoot();
  if (options.userDataFolder.empty() && !root.empty()) {
    options.userDataFolder = root + L"\\User Data";
  }
  if (options.agentConfigPath.empty() && !root.empty()) {
    options.agentConfigPath = root + L"\\agent-bridge.json";
  }
  if (!options.metricsPath.empty() && !agentConfigExplicit) {
    options.noAgent = true;  // 测量运行与日常实例隔离：不去抢 broker 的连接票据
  }
  if (options.warmD3D < 0) {
    options.warmD3D = options.renderTarget == RenderTargetKind::kSoftware ? 1 : 0;
  }
  return options;
}

}  // namespace jisu
