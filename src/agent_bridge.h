// 智能体桥：原生实现 Pi Background 扩展那一侧，让 Pi 的 browser 工具（Playwright MCP 扩展模式）驱动本浏览器。
// 流程与 Pi 改版 Playwright Extension 相同：轮询本机 broker（127.0.0.1:18773）取连接票据 → 从标签池租一个后台标签 →
// 连 MCP relay（WebSocket）→ 宣布标签并握手 → 把 chrome.debugger.* 请求转成 WebView2 的 CDP 调用，事件原样回传。
// 用同一份配对令牌，不开原生调试端口；断开后标签与页面留在池里，下一条连接接着用。
//
// 与扩展版的两处不同：
// 1. 每条连接用 Target.attachToTarget 单开一个 CDP 会话，断开时 detach。WebView2 自带的默认会话不随连接结束，
//    Playwright 注入的脚本、绑定、开启的域会一直累积；独立会话断开即清空，与 chrome.debugger.detach 等价。
// 2. 省电：执行命令时标签在屏幕外宿主窗口里可见（WebView2 隐藏控件不出帧，截图和 rAF 会卡住），
//    命令间隙超过 graceMs 就关掉焦点模拟并隐藏，页面定时器随即被节流（实测动画重页 47% → 0.2% 单核）。
#pragma once

#include "common.h"

#include <WebView2.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace jisu {

namespace net {
class WebSocket;
}

struct AgentBridgeConfig {
  bool enabled = false;
  std::wstring brokerUrl = L"http://127.0.0.1:18773";
  std::string token;      // 43 位配对令牌，只在内存里用
  DWORD pollMs = 1000;    // broker 轮询间隔（Pi 判定扩展在线要求 6 秒内有轮询）
  DWORD graceMs = 1500;   // 最后一条命令完成后保持可见多久，之后隐藏
};

// 读取 agent-bridge.json：{"broker": "...", "tokenFile": "...", "pollMs": 1000, "graceMs": 1500, "enabled": true}
// tokenFile 指向 Pi 的 extension-auth.json（{"token": "..."}）。文件不存在时 enabled=false 且无错误。
AgentBridgeConfig LoadAgentBridgeConfig(const std::wstring& file, std::string* error);

// 由浏览器窗口实现：智能体标签池
class AgentHost {
 public:
  virtual ~AgentHost() = default;
  using LeaseCallback = std::function<void(uint32_t tabId, ICoreWebView2* webview, const std::string& error)>;
  // 租一个空闲的智能体标签（没有就新建一个后台标签）；控件就绪后回调，可能同步回调
  virtual void LeaseAgentTab(LeaseCallback done) = 0;
  virtual void ReturnAgentTab(uint32_t tabId) = 0;
  // 执行命令前唤醒（屏幕外可见），空闲后休眠（隐藏）；用户正看着这个标签时由窗口自己保持可见
  virtual void SetAgentTabAwake(uint32_t tabId, bool awake) = 0;
  virtual std::string AgentTabInfo(uint32_t tabId) = 0;  // chrome.tabs.Tab 风格的 JSON
};

class AgentBridge {
 public:
  static constexpr UINT kMsgTicket = WM_APP + 20;
  static constexpr UINT kMsgRelayOpen = WM_APP + 21;
  static constexpr UINT kMsgRelayMessage = WM_APP + 22;
  static constexpr UINT kMsgRelayClosed = WM_APP + 23;
  static constexpr UINT_PTR kIdleTimer = 0xA6E7;

  AgentBridge(HWND hwnd, AgentHost& host, AgentBridgeConfig config);
  ~AgentBridge();
  AgentBridge(const AgentBridge&) = delete;
  AgentBridge& operator=(const AgentBridge&) = delete;

  void Start();
  // UI 线程：桥自己的消息（含空闲计时器）处理掉并返回 true
  bool HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);
  // 智能体标签被关掉或渲染进程退出
  void OnAgentTabGone(uint32_t tabId);
  size_t ConnectionCount() const { return connections_.size(); }

 private:
  struct Connection {
    uint32_t id = 0;
    std::string ticketId;
    std::vector<std::string> extraTickets;  // 同一 relay 的重复邀请，连上后一并确认
    std::wstring relayUrl;
    std::shared_ptr<net::WebSocket> socket;
    uint32_t tabId = 0;
    ComPtr<ICoreWebView2> webview;
    bool open = false;
    bool closing = false;
    // CDP 会话
    bool attached = false;
    bool attaching = false;
    std::vector<std::string> attachWaiters;  // attach 完成前到达的 attach 请求（relay 消息 id）
    std::string rootSession;                 // 本连接自己的页面会话
    std::set<std::string> childSessions;     // 自动附加的 iframe / worker 会话
    std::vector<std::pair<ComPtr<ICoreWebView2DevToolsProtocolEventReceiver>, EventRegistrationToken>> subscriptions;
    // 唤醒 / 休眠
    int pending = 0;
    ULONGLONG lastActivity = 0;
    bool awake = false;
    bool focusDesired = false;  // Playwright 要求的焦点模拟
    bool focusApplied = false;
    bool screencast = false;    // 录屏期间一直出帧，不休眠
  };
  struct BrokerState;

  Connection* Find(uint32_t connectionId);
  void OnTicket(const std::string& ticketId, const std::string& url);
  void OnTabLeased(uint32_t connectionId, uint32_t tabId, ICoreWebView2* webview, const std::string& error);
  void OnRelayOpen(uint32_t connectionId, const std::string& error);
  void OnRelayMessage(uint32_t connectionId, const std::string& text);
  void Attach(Connection& connection, const std::string& idJson);
  void FinishAttach(uint32_t connectionId, const std::string& sessionId, const std::string& error);
  void Execute(Connection& connection, const std::string& idJson, const std::string& session,
               const std::string& method, const std::string& paramsJson);
  void OnCommandDone(uint32_t connectionId, const std::string& idJson, const std::string& method, HRESULT result,
                     LPCWSTR resultJson);
  // 桥自己发的 CDP 命令（不回给 relay）
  void Internal(Connection& connection, const std::string& session, const char* method, const std::string& paramsJson,
                std::function<void(HRESULT, const std::string&)> done = nullptr);
  void Wake(Connection& connection);
  void Sleep(Connection& connection);
  void OnIdleTimer();
  void OnCdpEvent(uint32_t connectionId, const std::string& method,
                  ICoreWebView2DevToolsProtocolEventReceivedEventArgs* args);
  void Subscribe(Connection& connection);
  void Unsubscribe(Connection& connection);
  void ReleaseSession(Connection& connection);
  void Reply(Connection& connection, const std::string& idJson, const std::string& resultJson);
  void Fail(Connection& connection, const std::string& idJson, const std::string& reason);
  void Send(Connection& connection, std::string message);
  void Ack(const std::string& ticketId, bool success, uint32_t tabId, const std::string& error);
  void Finish(uint32_t connectionId);

  HWND hwnd_;
  AgentHost& host_;
  AgentBridgeConfig config_;
  std::shared_ptr<BrokerState> broker_;
  std::map<uint32_t, std::unique_ptr<Connection>> connections_;
  uint32_t nextConnectionId_ = 1;
  bool idleTimerRunning_ = false;
  // 桥销毁后 WebView2 仍可能回调：回调里先看它是否还在
  std::shared_ptr<int> alive_ = std::make_shared<int>(0);
};

}  // namespace jisu
