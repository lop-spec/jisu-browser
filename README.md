# 极速浏览器 Windows 外壳

C++20 + Win32 + 官方 WebView2 SDK。标签条、工具栏、书签栏用 Direct2D 自绘，地址栏是系统 EDIT 控件；进程一启动先出窗口首帧，WebView2 环境与之并行创建，首个标签页画出后再预建一个隐藏的备用控件。

## 构建

只走 GitHub Actions（`.github/workflows/build.yml`，windows-latest + MSVC）。WebView2 SDK 从 nuget.org 下载并校验 SHA256，加载器静态链接，CRT 静态链接，不需要 VC++ 运行库和 WebView2Loader.dll。

产物 `jisu-browser-win-x64-<提交>`：`jisu-browser/`（`jisu-browser.exe` + `ntp/`，即运行包，整个目录放到任意位置即可运行）、`symbols/jisu-browser.pdb`、`SHA256SUMS.txt`、`ci-smoke.json`（CI 机器上的一次冒烟时间线）。

图标由 `res/make-icon.mjs` 生成（`node res/make-icon.mjs` 重写 `res/app.ico`）。

## 运行

`jisu-browser.exe [网址] [选项]`

同一用户数据目录只开一个主实例：再次启动（双击图标、别的程序打开链接）会把网址交给已开的窗口新开标签，然后退出。

| 选项 | 作用 |
|---|---|
| `--udf <目录>` | 用户数据目录，默认 `%LOCALAPPDATA%\JisuBrowser\User Data`（书签、设置也在这里） |
| `--agent-config <文件>` | 智能体桥配置，默认 `%LOCALAPPDATA%\JisuBrowser\agent-bridge.json`，文件不存在则不开桥 |
| `--no-agent` | 不开智能体桥 |
| `--metrics <文件>` | 写启动时间线 JSON（各时间点为距进程创建的毫秒数）；测量运行默认不开智能体桥、不导入书签 |
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

快捷键：Ctrl+T 新标签，Ctrl+W 关闭，Ctrl+Tab 切换，Ctrl+L / Alt+D / F6 地址栏，F5 / Ctrl+R 刷新，Alt+← / Alt+→ 后退前进，Ctrl+D 收藏 / 取消收藏，Ctrl+Shift+B 书签栏，Ctrl+Shift+O 书签管理器。

工具栏最右的菜单：书签管理器、显示书签栏、导入书签、设为默认浏览器（在当前用户下登记后打开系统「默认应用」页，由用户自己选）、关于。

## 书签

与 Chromium 的 `Bookmarks` 文件同一格式，存放在用户数据目录下，其他 Chromium 系浏览器可以直接读。

- 地址框右端星标 / Ctrl+D：收藏到书签栏末尾；已收藏时可移除或编辑。
- 书签栏：点网址打开（Ctrl+点击、中键为新标签），点文件夹弹出菜单（文件夹里有两个以上网址时可「全部打开」），右键编辑 / 删除，放不下的收进「»」。
- 书签管理器（`jisu://bookmarks`）：文件夹树、搜索、添加、重命名、编辑网址、删除、拖动排序与移入文件夹、从本机 Quark / Thorium / Chrome / Edge 等导入、导出 HTML。
- 第一次用默认用户数据目录启动时，若系统默认浏览器是 Chromium 系，自动从它导入一次书签。

## 智能体桥（CDP）

Pi 的 `browser` 工具走 Playwright MCP 的扩展模式：MCP 起一个本机 relay，经本机 broker（`127.0.0.1:18773`）发连接邀请，由浏览器里的扩展连上 relay 并用 `chrome.debugger` 转发 CDP。极速浏览器在外壳里原生实现扩展这一侧（`src/agent_bridge.cpp`），协议与 Pi 改版扩展一致，Pi 侧不用改：

- 轮询 broker 取邀请，校验配对令牌、协议版本与 relay 地址（只接受本机）。
- 从标签池租一个智能体标签（没有空闲的就新建一个后台标签，不激活），连上 relay，宣布标签并握手；断开后标签与页面留在池里，下一条连接接着用。用户正在看的标签不会被租用。
- 每条连接用 `Target.attachToTarget` 单开一个 CDP 会话，断开时 detach：Playwright 开过的域、注入的脚本随会话清掉（WebView2 自带的默认会话不随连接结束）。
- 与 Pi 扩展相同的单标签约束：拒绝新建 / 关闭标签，忽略置前请求。
- 省电：执行命令时智能体标签在屏幕外宿主窗口里可见（WebView2 隐藏的控件不出帧，截图和 rAF 会卡住）；命令间隙超过 `graceMs` 就关掉焦点模拟并隐藏，页面定时器随即被节流。实测动画重页单核占用：可见 47%、隐藏但保留焦点模拟 15%、隐藏并关焦点模拟 0.2%。
- 智能体标签在标签条里画一个圈（执行命令时实心）；点开可以直接看，也能接手操作。它们的脚本弹框按 Playwright 默认处理（离开页面确认放行，其余取消），权限请求拒绝。

配置 `agent-bridge.json`（不存在就不开桥）：

```json
{
  "broker": "http://127.0.0.1:18773",
  "tokenFile": "<Pi 数据目录>\\browser-agent\\extension-auth.json",
  "pollMs": 1000,
  "graceMs": 1500
}
```

令牌只在内存里用。与装了 Pi Background 扩展的 Thorium 同时开着时，两边都会去领同一张邀请，日常只开其中一个。

## 测量 G1 / G2

```
powershell -NoProfile -ExecutionPolicy Bypass -File measure\run-g1g2.ps1 -Exe <jisu-browser.exe> -OutDir <结果目录> [-Runs 20] [-Preheat] [-ShellArgs "--rt hw"]
```

外部计时复用可行性原型（不在本仓库）里 `jisu-probe.exe` 的 launch 模式（CreateProcess 前打点，WinEventHook 捕获窗口显示），用 `-Probe` 指定路径；内部时间线零点为进程创建时间。

- G1：`firstFramePresented`，首帧 EndDraw 后由后台线程 DwmFlush 等到 DWM 合成的时刻，≤ 70ms。
- G2：新标签页内容首帧，≤ 500ms，预热时 ≤ 300ms。每轮取较晚者：`ntpFirstFrameHost`（rAF 之后的下一个任务里回报、外壳收到的时刻，与可行性原型的 `webFirstFrameMs` 同口径）与 `ntpFcp`（Chromium 记录的首个内容帧呈现时刻）。
- `-WaitUserIdle <秒>`：每轮都等键鼠空闲满这么久再跑，有人在用机器时不弹测试窗口；`-AbortOnInput`：开跑后一有输入就整批放弃（采 ETW 用）。
