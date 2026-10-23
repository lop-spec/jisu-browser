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
  RenderTargetKind renderTarget = RenderTargetKind::kHardware;  // --rt hw|sw
  bool warmD3D = true;             // --warm-d3d 0|1：后台线程提前建 D3D 设备以加载显卡驱动
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
