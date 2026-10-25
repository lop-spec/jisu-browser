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

快捷键：Ctrl+T 新标签，Ctrl+W 关闭，Ctrl+Tab 切换，Ctrl+L / Alt+D / F6 地址栏，F5 / Ctrl+R 刷新，Alt+← / Alt+→ 后退前进，Ctrl+D 添加 / 编辑书签，Ctrl+Shift+B 书签栏，Ctrl+Shift+O 书签管理器。

工具栏最右的菜单：书签管理器、显示书签栏、导入书签、扩展管理、设为默认浏览器（在当前用户下登记后打开系统「默认应用」页，由用户自己选）、关于。

标签条同时承担窗口拖动、双击最大化和最小化／最大化／关闭，不再单独显示重复的系统标题栏。

## 书签

与 Chromium 的 `Bookmarks` 文件同一格式，存放在用户数据目录下，其他 Chromium 系浏览器可以直接读。

- 地址框右端星标 / Ctrl+D：弹出收藏窗口，可改名称与网址、选择任意文件夹、当场新建子文件夹；记住上次保存位置。再次点击星标进入编辑，可移动位置或移除。取消不会创建书签或文件夹。
- 书签栏：点网址打开（Ctrl+点击、中键为新标签），点文件夹弹出菜单（文件夹里有两个以上网址时可「全部打开」），右键编辑 / 删除，放不下的收进「»」。按住拖动可排序，拖到文件夹中间并松开即可移入；高亮框表示移入，竖线表示插入，Esc 或拖出书签栏取消。
- 书签管理器（`jisu://bookmarks`）：文件夹树、搜索、添加、重命名、编辑网址、删除、从本机 Quark / Thorium / Chrome / Edge 等导入、导出 HTML。编辑窗口也能选择保存位置、新建文件夹；拖到列表中的文件夹或左侧目录即可移入，拖到网址行前可排序。不允许把文件夹拖进自己或自己的子文件夹。
- 第一次用默认用户数据目录启动时，若系统默认浏览器是 Chromium 系，自动从它导入一次书签。
- 标签页、书签栏和管理器显示网站图标：优先用本机缓存，访问页面后通过 WebView2 更新；缺图标时后台尝试站点自身的 `/favicon.ico`，不交给第三方图标服务，不带 Cookie 或认证信息。

## 指定扩展与密码

`node tools/import-profile-assets.mjs --apply` 只处理本机能找到的 **Anime4K WebExtension、Auto Close Old Tabs、Authenticator** 程序文件，并导入本机网站图标缓存。扩展源码、账号、验证码种子、保险库和扩展存储不随浏览器发布；第三方程序不进仓库。

- 扩展通过 WebView2 的正式扩展 API 加载；已启用的扩展默认显示在地址栏右侧，悬停显示名称，点击在按钮下方打开独立弹层，再点一次、Esc 或点击外部关闭；不占用网页标签，不借用智能体标签。
- 「扩展程序」拼图按钮列出全部扩展，可打开、固定到工具栏／取消固定、进入设置或管理；管理页也有固定开关。固定状态写入本机 `extensions.json` 的 `pinned`，重启保留；取消固定不等于停用，停用的扩展不显示按钮。加载成功不代表所有 Chrome API 都兼容。
- Auto Close Old Tabs 的界面沿用原扩展，普通标签关闭由外壳原生适配：默认上限 10 个，按创建顺序关闭最早的非当前普通标签，保护当前页、智能体租用标签和扩展页面。上限可在扩展「打开」里改为 1–99。
- 安装清单写在用户资料目录的 `extensions.json`；指定 `removeUnlisted: true` 后，启动时会通过正式 API 移除未列出的扩展，结果在 `extensions-status.json` 与 `extensions-cleanup.json`。
- 开启 WebView2 原生密码保存和自动填充。但 SDK 没有跨浏览器密码导入接口；不会解密或直接搬运 `Login Data`、Cookie、登录会话或身份验证器种子。已有账号要在极速里登录；Authenticator 的保险库迁移需由用户通过其正式导入流程完成。

## 智能体桥（CDP）

Pi 的 `browser` 工具走 Playwright MCP 的扩展模式：MCP 起一个本机 relay，经本机 broker（`127.0.0.1:18773`）发连接邀请，由浏览器里的扩展连上 relay 并用 `chrome.debugger` 转发 CDP。极速浏览器在外壳里原生实现扩展这一侧（`src/agent_bridge.cpp`），协议与 Pi 改版扩展一致，Pi 侧不用改：

- 轮询 broker 取邀请，校验配对令牌、协议版本与 relay 地址（只接受本机）。
- 从标签池租一个智能体标签（没有空闲的就新建一个后台标签，不激活），连上 relay，宣布标签并握手；断开后标签与页面留在池里，下一条连接接着用。用户正在看的标签不会被租用。
- 每条连接用 `Target.attachToTarget` 单开一个 CDP 会话，断开时 detach：Playwright 开过的域、注入的脚本随会话清掉（WebView2 自带的默认会话不随连接结束）。
- 与 Pi 扩展相同的单标签约束：拒绝新建 / 关闭标签，忽略置前请求。
- 省电：执行命令时智能体标签在屏幕外宿主窗口里可见（WebView2 隐藏的控件不出帧，截图和 rAF 会卡住）；命令间隙超过 `graceMs` 就关掉焦点模拟并隐藏，页面定时器随即被节流，再过 `pageFreezeMs` 冻结。实测动画重页单核占用：可见 47%、隐藏但保留焦点模拟 15%、隐藏并关焦点模拟 0.2%。
- 唤醒后先等页面出一帧（rAF → setTimeout 0，最多 300ms）再转发命令，给积下的页面更新一个处理机会，降低读到旧内容的风险。这不是「页面已完成更新」的保证：冻结会暂停定时器与客户端逻辑，依赖这些逻辑的轮询、流式回答或后台任务可能延迟；要求持续运行时，把 `pageFreezeMs` 和 `freezeMs` 都设为 0。用户正在看的标签不冻结。
- 智能体标签在标签条里画一个圈（执行命令时实心）；点开可以直接看，也能接手操作。它们的脚本弹框按 Playwright 默认处理（离开页面确认放行，其余取消），权限请求拒绝。

配置 `agent-bridge.json`（不存在就不开桥）：

```json
{
  "broker": "http://127.0.0.1:18773",
  "tokenFile": "<Pi 数据目录>\\browser-agent\\extension-auth.json",
  "pollMs": 1000,
  "graceMs": 400,
  "yieldTo": ["thorium.exe"]
}
```

网页模型桥使用独立的 `127.0.0.1:18774`，日常 Thorium `browser` 工具仍用 18773，两者不抢邀请。两台机器各用各自的本机配对令牌；模型桥配置将 `pageFreezeMs`、`freezeMs` 设为 0，避免暂停长回答、后台请求或轮询。登录与网站年龄确认仍由用户在极速中完成。

令牌只在内存里用。`yieldTo`：列出的进程（例如装着 Pi Background 扩展的 Thorium）在运行时桥暂停领邀请，关掉它后几秒内自动接手——两边同时领同一张邀请时，落败方的失败确认会把已经建立的连接标成失败。

智能体标签的省电设置（同一个文件里，括号内为默认值）：

| 键 | 作用 |
|---|---|
| `graceMs`（400） | 最后一条命令完成后保持可见多久，之后关焦点模拟并隐藏 |
| `freezeMs`（30000） | 隐藏后再过多久挂起页面（定时器、脚本全停），下一条命令到达时先恢复；每次挂起约有 150–400ms 的 CPU 尖峰，只在长间隔里划算；0 关闭 |
| `pageFreezeMs`（1000） | 隐藏后再过多久用 CDP 冻结页面（`Page.setWebLifecycleState frozen`，同 Chrome 冻结后台标签：脚本、定时器停住，网络照收），下一条命令前先解冻；比 `freezeMs` 的 TrySuspend 便宜；0 关闭 |
| `renderOnDemand`（false） | 按需出帧：只在截图、输入事件前唤醒；导航、读页面、快照在隐藏状态下执行（同后台标签），等待 Promise 的 evaluate 超过 `renderWaitMs`（200）仍未返回才唤醒 |
| `rasterScale`（1.0） | 在宿主里的光栅化比例：CSS 视口不变、像素更少，`srcset` 也会挑小图；0 跟随显示器 |
| `blockAutoplay`（true） | 媒体不自动播放：`play()` 像被浏览器自动播放策略拒绝一样返回 `NotAllowedError` |
| `blockImages`（false） | 不加载图片；用户点开这个标签看时不拦 |
| `blockedUrls`（内置清单） | 在智能体的 CDP 会话上用 `Network.setBlockedURLs` 拦截的网址模式，默认只含广告投放与统计上报；给 `[]` 关闭 |

## 安装

用 CI 运行包安装到当前用户（不需要管理员）：

```
powershell -NoProfile -ExecutionPolicy Bypass -File tools\install.ps1 -Package <运行包里的 jisu-browser 目录> -ShortcutDir <快捷方式目录> -StartMenu -AgentTokenFile <Pi 的 extension-auth.json> -YieldTo thorium.exe
```

程序装到 `%LOCALAPPDATA%\Programs\JisuBrowser`，覆盖前旧程序留在 `_历史版本\`；目标程序正在运行时脚本会停下，不会结束它。新写的智能体配置只包含 `broker`、`tokenFile`、`yieldTo`，省电项跟随程序默认值；已有配置默认保留，只有 `-ForceAgentConfig` 才备份并重写。

## 智能体 CPU 对比

`measure/agent-bench.mjs` 用 Pi 同一套 Playwright MCP 扩展模式（进程内测试 broker + relay，打 Pi 的两个补丁）在同一组站点上按「执行一步 → 等待」驱动浏览器；`measure/jobrun.cs` 把浏览器整棵进程树放进 Job Object 记账 CPU（已退出的渲染进程也算）。

最终程序 `d10c6f8`、默认省电设置的本机对照（13 个站点 × 导航、文本、快照、滚动，共 52 步）：

| 命令间隔 | Thorium CPU 秒 | 极速 CPU 秒 | Thorium / 极速 | CPU 减少 |
|---|---:|---:|---:|---:|
| 按 Pi 日志抽样的真实间隔（单次上限 120 秒） | 177.8 | 71.5 | 2.49× | 59.8% |
| 固定 10 秒 | 99.8 | 69.5 | 1.44× | 30.4% |

这些数字仅对应 `d10c6f8` 的无扩展基线，不代表当前带扩展、且关闭冻结的网页模型桥。它们是整棵进程树累计 CPU 时间，不是网页速度或瓦特实测；**整体 CPU 降到 1/3 的目标尚未达到**。真实间隔组的执行期 CPU 为 49.2 → 46.3 秒，间隙为 128.6 → 25.1 秒，收益主要来自等待时少运行。

两组都是实时网络单轮对照，测试期间有用户输入，不能当成稳定性能保证；两边均有一次导航超时，10 秒组 Thorium 还多一次固定标签协议错误，数字包含失败步骤。较早构建的空闲态结果不混入最终程序的整体成绩。

默认不禁图片、不启用按需出帧；禁图片会影响图像识别，按需出帧可能延长重型 SPA 初始化。默认禁自动播放只作用于智能体标签，媒体自动播放行为会改变。两边都用临时配置目录，窗口放在屏幕外不激活。

## 测量 G1 / G2

```
powershell -NoProfile -ExecutionPolicy Bypass -File measure\run-g1g2.ps1 -Exe <jisu-browser.exe> -OutDir <结果目录> [-Runs 20] [-Preheat] [-ShellArgs "--rt hw"]
```

外部计时复用可行性原型（不在本仓库）里 `jisu-probe.exe` 的 launch 模式（CreateProcess 前打点，WinEventHook 捕获窗口显示），用 `-Probe` 指定路径；内部时间线零点为进程创建时间。

- G1：`firstFramePresented`，首帧 EndDraw 后由后台线程 DwmFlush 等到 DWM 合成的时刻，≤ 70ms。
- G2：新标签页内容首帧，≤ 500ms，预热时 ≤ 300ms。每轮取较晚者：`ntpFirstFrameHost`（rAF 之后的下一个任务里回报、外壳收到的时刻，与可行性原型的 `webFirstFrameMs` 同口径）与 `ntpFcp`（Chromium 记录的首个内容帧呈现时刻）。
- `-WaitUserIdle <秒>`：每轮都等键鼠空闲满这么久再跑，有人在用机器时不弹测试窗口；`-AbortOnInput`：开跑后一有输入就整批放弃（采 ETW 用）。
