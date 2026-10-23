// 命令行选项。测量用的开关都在这里，一份构建即可在本机做对照实验。
#pragma once

#include <string>

namespace jisu {

enum class RenderTargetKind { kHardware, kSoftware };
enum class EnvStart { kEarly, kLate };
enum class NtpSource { kFolder, kInline };

struct Options {
  std::wstring userDataFolder;     // --udf <目录>，默认 %LOCALAPPDATA%\JisuBrowser\User Data
  std::wstring metricsPath;        // --metrics <文件>：写启动时间线 JSON
  bool exitAfterMetrics = false;   // --exit-after-metrics：写完时间线即退出
  bool noActivate = false;         // --no-activate：显示窗口但不激活、不抢焦点
  bool hasBounds = false;          // --bounds x,y,w,h（物理像素）
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
  // --rt sw|hw。默认软件：实测本机（RX 6900 XT）每建一个硬件 D3D 设备要 45–70ms，硬件路径首帧约 150ms，
  // 软件路径约 64ms；标签条、工具栏这种静态界面用软件光栅化足够
  RenderTargetKind renderTarget = RenderTargetKind::kSoftware;
  // --warm-d3d 0|1：后台线程提前建一个 D3D 设备（软件模式为 WARP，硬件模式为显卡）。
  // 不指定时软件模式预热、硬件模式不预热：硬件预热实测是净亏（渲染目标还会自己再建一个设备）
  int warmD3D = -1;
  EnvStart envStart = EnvStart::kEarly;  // --env early|late：WebView2 环境在首帧前还是首帧后发起
  NtpSource ntp = NtpSource::kFolder;    // --ntp folder|inline：新标签页走随包目录还是内置字符串
  bool spare = true;               // --no-spare：不预建备用控件
  bool trackingPrevention = true;  // --tracking-prevention 0|1
  std::wstring browserArgs;        // --browser-args "<参数>"：传给内核的附加参数
  int preheatSeconds = 0;          // --preheat <秒>：不显示窗口，只持有内核进程（模拟开机预热）
  std::wstring startUrl;           // 第一个非选项参数：启动时打开的网址
  std::wstring commandLine;        // 原始命令行，写进时间线
};

Options ParseOptions(const wchar_t* commandLine);

}  // namespace jisu
