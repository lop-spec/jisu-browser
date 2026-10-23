# 极速浏览器 Windows 外壳

C++20 + Win32 + 官方 WebView2 SDK。标签条、工具栏用 Direct2D 自绘，地址栏是系统 EDIT 控件；进程一启动先出窗口首帧，WebView2 环境与之并行创建，首个标签页画出后再预建一个隐藏的备用控件。

## 构建

只走 GitHub Actions（`.github/workflows/build.yml`，windows-latest + MSVC）。WebView2 SDK 从 nuget.org 下载并校验 SHA256，加载器静态链接，CRT 静态链接，不需要 VC++ 运行库和 WebView2Loader.dll。

产物 `jisu-browser-win-x64-<提交>`：`jisu-browser/`（`jisu-browser.exe` + `ntp/`，即运行包）、`symbols/jisu-browser.pdb`、`SHA256SUMS.txt`、`ci-smoke.json`（CI 机器上的一次冒烟时间线）。

## 运行

`jisu-browser.exe [网址] [选项]`

| 选项 | 作用 |
|---|---|
| `--udf <目录>` | 用户数据目录，默认 `%LOCALAPPDATA%\JisuBrowser\User Data` |
| `--metrics <文件>` | 写启动时间线 JSON（各时间点为距进程创建的毫秒数） |
| `--exit-after-metrics` | 写完时间线即退出 |
| `--no-activate` | 显示窗口但不激活 |
| `--bounds x,y,w,h` | 窗口位置与大小（物理像素） |
| `--rt sw\|hw` | Direct2D 软件 / 硬件渲染目标，默认 sw（硬件路径建 D3D 设备太慢，首帧过不了 70ms） |
| `--warm-d3d 0\|1` | 后台线程提前建一个 D3D 设备（软件为 WARP，硬件为显卡）；默认软件预热、硬件不预热 |
| `--env early\|late` | WebView2 环境在首帧前 / 首帧后发起，默认 early |
| `--ntp folder\|inline` | 新标签页走随包 `ntp/` 目录 / 内置字符串，默认 folder |
| `--no-spare` | 不预建备用控件 |
| `--tracking-prevention 0\|1` | WebView2 跟踪防护，默认 1 |
| `--browser-args "<参数>"` | 传给内核的附加参数 |
| `--preheat <秒>` | 不显示窗口，只持有内核进程（模拟开机预热） |

快捷键：Ctrl+T 新标签，Ctrl+W 关闭，Ctrl+Tab 切换，Ctrl+L / Alt+D / F6 地址栏，F5 / Ctrl+R 刷新，Alt+← / Alt+→ 后退前进。

## 测量 G1 / G2

```
powershell -NoProfile -ExecutionPolicy Bypass -File measure\run-g1g2.ps1 -Exe <jisu-browser.exe> -OutDir <结果目录> [-Runs 20] [-Preheat] [-ShellArgs "--rt hw"]
```

外部计时复用可行性原型（不在本仓库）里 `jisu-probe.exe` 的 launch 模式（CreateProcess 前打点，WinEventHook 捕获窗口显示），用 `-Probe` 指定路径；内部时间线零点为进程创建时间。

- G1：`firstFramePresented`，首帧 EndDraw 后由后台线程 DwmFlush 等到 DWM 合成的时刻，≤ 70ms。
- G2：新标签页内容首帧，≤ 500ms，预热时 ≤ 300ms。每轮取较晚者：`ntpFirstFrameHost`（rAF 之后的下一个任务里回报、外壳收到的时刻，与可行性原型的 `webFirstFrameMs` 同口径）与 `ntpFcp`（Chromium 记录的首个内容帧呈现时刻）。
- `-WaitUserIdle <秒>`：每轮都等键鼠空闲满这么久再跑，有人在用机器时不弹测试窗口；`-AbortOnInput`：开跑后一有输入就整批放弃（采 ETW 用）。
