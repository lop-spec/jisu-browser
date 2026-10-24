#include "agent_bridge.h"

#include "cdp_events.h"
#include "json.h"
#include "net.h"

#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>

using Microsoft::WRL::Callback;

namespace jisu {
namespace {

const json::Value kNull;

// 智能体会话默认拦截的网址：广告投放与统计上报。只收「拦了页面照常工作」的，登录、验证码、风控、支付一概不碰
constexpr const char* kDefaultBlockedUrls[] = {
    "*://*.doubleclick.net/*",
    "*://*.googlesyndication.com/*",
    "*://*.googleadservices.com/*",
    "*://*.google-analytics.com/*",
    "*://*.googletagmanager.com/*",
    "*://*.googletagservices.com/*",
    "*://*.adnxs.com/*",
    "*://*.criteo.com/*",
    "*://*.criteo.net/*",
    "*://*.taboola.com/*",
    "*://*.outbrain.com/*",
    "*://*.amazon-adsystem.com/*",
    "*://*.scorecardresearch.com/*",
    "*://*.hotjar.com/*",
    "*://*.clarity.ms/*",
    "*://bat.bing.com/*",
    "*://hm.baidu.com/*",
    "*://pos.baidu.com/*",
    "*://cpro.baidustatic.com/*",
    "*://*.cnzz.com/*",
    "*://*.umeng.com/*",
    "*://*.umengcloud.com/*",
    "*://mcs.zijieapi.com/*",
    "*://mon.zijieapi.com/*",
    "*://*.ibytedapm.com/*",
    "*://*.mediav.com/*",
    "*://*.tanx.com/*",
    "*://*.mmstat.com/*",
};

bool IsValidToken(const std::string& token) {
  if (token.size() != 43) {
    return false;
  }
  for (const char c : token) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    if (!ok) {
      return false;
    }
  }
  return true;
}

int HexValue(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

// 与 URLSearchParams 相同：'+' 解成空格，%XX 解成字节
std::string PercentDecode(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (c == '+') {
      out.push_back(' ');
    } else if (c == '%' && i + 2 < text.size() && HexValue(text[i + 1]) >= 0 && HexValue(text[i + 2]) >= 0) {
      out.push_back(static_cast<char>(HexValue(text[i + 1]) * 16 + HexValue(text[i + 2])));
      i += 2;
    } else {
      out.push_back(c);
    }
  }
  return out;
}

std::map<std::string, std::string> ParseQuery(std::string_view url) {
  std::map<std::string, std::string> result;
  const size_t question = url.find('?');
  if (question == std::string_view::npos) {
    return result;
  }
  std::string_view query = url.substr(question + 1);
  const size_t hash = query.find('#');
  if (hash != std::string_view::npos) {
    query = query.substr(0, hash);
  }
  while (!query.empty()) {
    const size_t amp = query.find('&');
    const std::string_view pair = query.substr(0, amp);
    const size_t equals = pair.find('=');
    const std::string key = PercentDecode(pair.substr(0, equals));
    const std::string value = equals == std::string_view::npos ? std::string() : PercentDecode(pair.substr(equals + 1));
    if (!key.empty() && !result.count(key)) {
      result[key] = value;
    }
    if (amp == std::string_view::npos) {
      break;
    }
    query = query.substr(amp + 1);
  }
  return result;
}

DWORD ClampMs(double value, DWORD low, DWORD high) {
  if (value < low) {
    return low;
  }
  if (value > high) {
    return high;
  }
  return static_cast<DWORD>(value);
}

void PostString(HWND hwnd, UINT msg, WPARAM wParam, std::string text) {
  auto* payload = new std::string(std::move(text));
  if (!PostMessageW(hwnd, msg, wParam, reinterpret_cast<LPARAM>(payload))) {
    delete payload;
  }
}

std::string HresultText(const std::string& prefix, HRESULT hr) {
  char text[32];
  snprintf(text, sizeof(text), " (0x%08X)", static_cast<unsigned>(hr));
  return prefix + text;
}

// 有没有正在运行的进程叫这些名字（不区分大小写）
bool AnyProcessRunning(const std::vector<std::wstring>& names) {
  if (names.empty()) {
    return false;
  }
  const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) {
    return false;
  }
  PROCESSENTRY32W entry{};
  entry.dwSize = static_cast<DWORD>(sizeof(entry));
  bool found = false;
  for (BOOL ok = Process32FirstW(snapshot, &entry); ok && !found; ok = Process32NextW(snapshot, &entry)) {
    for (const std::wstring& name : names) {
      if (_wcsicmp(entry.szExeFile, name.c_str()) == 0) {
        found = true;
        break;
      }
    }
  }
  CloseHandle(snapshot);
  return found;
}

}  // namespace

struct AgentBridge::BrokerState {
  std::mutex mutex;
  std::condition_variable wake;
  std::deque<std::string> acks;
  bool stopping = false;
  std::atomic<bool> yielding{false};
};

AgentBridgeConfig LoadAgentBridgeConfig(const std::wstring& file, std::string* error) {
  AgentBridgeConfig config;
  std::string text;
  if (!ReadFileBytes(file, text)) {
    return config;  // 没有配置文件：桥不开，不算错误
  }
  json::Value root;
  std::string parseError;
  if (!json::Parse(text, root, &parseError)) {
    if (error) {
      *error = "agent-bridge.json: " + parseError;
    }
    return config;
  }
  if (root["broker"].IsString()) {
    config.brokerUrl = FromUtf8(root["broker"].AsString());
  }
  while (!config.brokerUrl.empty() && config.brokerUrl.back() == L'/') {
    config.brokerUrl.pop_back();
  }
  if (config.brokerUrl.rfind(L"http://127.0.0.1:", 0) != 0 && config.brokerUrl.rfind(L"http://[::1]:", 0) != 0) {
    if (error) {
      *error = "agent bridge broker must be a loopback http URL";
    }
    return config;
  }
  if (root["pollMs"].IsNumber()) {
    config.pollMs = ClampMs(root["pollMs"].AsNumber(), 200, 5000);
  }
  if (root["graceMs"].IsNumber()) {
    config.graceMs = ClampMs(root["graceMs"].AsNumber(), 200, 600000);
  }
  for (const json::Value& name : root["yieldTo"].Items()) {
    if (name.IsString() && !name.AsString().empty()) {
      config.yieldTo.push_back(FromUtf8(name.AsString()));
    }
  }
  if (root["freezeMs"].IsNumber()) {
    config.freezeMs = root["freezeMs"].AsNumber() <= 0 ? 0 : ClampMs(root["freezeMs"].AsNumber(), 500, 3600000);
  }
  if (root["rasterScale"].IsNumber()) {
    const double scale = root["rasterScale"].AsNumber();
    config.rasterScale = scale <= 0 ? 0 : (scale < 0.5 ? 0.5 : (scale > 4 ? 4 : scale));
  }
  config.blockAutoplay = root["blockAutoplay"].AsBool(config.blockAutoplay);
  config.blockImages = root["blockImages"].AsBool(config.blockImages);
  config.renderOnDemand = root["renderOnDemand"].AsBool(config.renderOnDemand);
  if (root["renderWaitMs"].IsNumber()) {
    config.renderWaitMs = ClampMs(root["renderWaitMs"].AsNumber(), 50, 5000);
  }
  if (root["blockedUrls"].IsArray()) {
    for (const json::Value& pattern : root["blockedUrls"].Items()) {
      if (pattern.IsString() && !pattern.AsString().empty()) {
        config.blockedUrls.push_back(pattern.AsString());
      }
    }
  } else {
    // 默认只拦广告与统计上报：不拦登录、验证码、风控、支付相关的域名
    for (const char* pattern : kDefaultBlockedUrls) {
      config.blockedUrls.push_back(pattern);
    }
  }
  std::string token;
  if (root["tokenFile"].IsString()) {
    std::string tokenText;
    json::Value tokenRoot;
    if (ReadFileBytes(FromUtf8(root["tokenFile"].AsString()), tokenText) && json::Parse(tokenText, tokenRoot)) {
      token = tokenRoot["token"].AsString();
    }
  }
  if (!IsValidToken(token)) {
    if (error) {
      *error = "agent bridge pairing token is unavailable or invalid";
    }
    return config;
  }
  config.token = token;
  config.enabled = !root["enabled"].IsBool() || root["enabled"].AsBool();
  return config;
}

AgentBridge::AgentBridge(HWND hwnd, AgentHost& host, AgentBridgeConfig config)
    : hwnd_(hwnd), host_(host), config_(std::move(config)) {}

AgentBridge::~AgentBridge() {
  alive_.reset();  // 之后到达的 WebView2 回调都不再进来
  if (broker_) {
    {
      std::lock_guard<std::mutex> lock(broker_->mutex);
      broker_->stopping = true;
    }
    broker_->wake.notify_all();
  }
  if (idleTimerRunning_) {
    KillTimer(hwnd_, kIdleTimer);
  }
  for (auto& item : connections_) {
    Unsubscribe(*item.second);
    if (item.second->socket) {
      item.second->socket->Close();
    }
  }
  connections_.clear();  // 标签由浏览器窗口统一关闭
}

void AgentBridge::Start() {
  if (!config_.enabled || broker_) {
    return;
  }
  broker_ = std::make_shared<BrokerState>();
  // broker 线程：轮询票据、发送确认。只持有共享状态与配置副本，桥对象销毁后自行退出。
  std::thread([state = broker_, hwnd = hwnd_, config = config_]() {
    std::map<std::string, ULONGLONG> seen;
    const std::wstring pollUrl = config.brokerUrl + L"/poll";
    const std::wstring ackUrl = config.brokerUrl + L"/ack";
    ULONGLONG lastYieldCheck = 0;
    for (;;) {
      std::deque<std::string> acks;
      {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->stopping) {
          return;
        }
        acks.swap(state->acks);
      }
      for (const std::string& body : acks) {
        net::Request(L"POST", ackUrl, body, config.token, 3000);
      }
      // 让行：配置里列出的浏览器（例如装着 Pi Background 扩展的 Thorium）在运行时不领邀请，
      // 免得两边抢同一张票、落败方的失败确认把已建立的连接标成失败
      if (!config.yieldTo.empty() && GetTickCount64() - lastYieldCheck >= 5000) {
        state->yielding = AnyProcessRunning(config.yieldTo);
        lastYieldCheck = GetTickCount64();
      }
      net::HttpResult poll;
      if (!state->yielding) {
        poll = net::Request(L"GET", pollUrl, std::string(), config.token, 3000);
      }
      json::Value root;
      DWORD wait = config.pollMs;
      if (state->yielding) {
        wait = 5000;
      } else if (poll.status == 200 && json::Parse(poll.body, root)) {
        const ULONGLONG now = GetTickCount64();
        for (const json::Value& ticket : root["tickets"].Items()) {
          const std::string& id = ticket["id"].AsString();
          const std::string& url = ticket["url"].AsString();
          if (id.empty() || url.empty() || seen.count(id)) {
            continue;
          }
          seen[id] = now;
          json::Value message = json::Value::MakeObject();
          message.Set("id", json::Value::String(id));
          message.Set("url", json::Value::String(url));
          PostString(hwnd, kMsgTicket, 0, json::Serialize(message));
        }
        for (auto it = seen.begin(); it != seen.end();) {
          it = now - it->second > 300000 ? seen.erase(it) : std::next(it);
        }
      } else {
        wait = 5000;  // broker 没开（Pi 没在用浏览器）：与扩展一样退到 5 秒一次
      }
      std::unique_lock<std::mutex> lock(state->mutex);
      state->wake.wait_for(lock, std::chrono::milliseconds(wait),
                           [&] { return state->stopping || !state->acks.empty(); });
      if (state->stopping) {
        return;
      }
    }
  }).detach();
}

bool AgentBridge::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
  switch (msg) {
    case kMsgTicket: {
      std::unique_ptr<std::string> payload(reinterpret_cast<std::string*>(lParam));
      json::Value ticket;
      if (payload && json::Parse(*payload, ticket)) {
        OnTicket(ticket["id"].AsString(), ticket["url"].AsString());
      }
      return true;
    }
    case kMsgRelayOpen: {
      std::unique_ptr<std::string> error(reinterpret_cast<std::string*>(lParam));
      OnRelayOpen(static_cast<uint32_t>(wParam), error ? *error : std::string("relay state lost"));
      return true;
    }
    case kMsgRelayMessage: {
      std::unique_ptr<std::string> text(reinterpret_cast<std::string*>(lParam));
      if (text) {
        OnRelayMessage(static_cast<uint32_t>(wParam), *text);
      }
      return true;
    }
    case kMsgRelayClosed:
      Finish(static_cast<uint32_t>(wParam));
      return true;
    case WM_TIMER:
      if (wParam != kIdleTimer) {
        return false;
      }
      OnIdleTimer();
      return true;
    default:
      return false;
  }
}

bool AgentBridge::Yielding() const { return broker_ && broker_->yielding.load(); }

AgentBridge::Connection* AgentBridge::Find(uint32_t connectionId) {
  const auto it = connections_.find(connectionId);
  return it == connections_.end() ? nullptr : it->second.get();
}

// ---------------------------------------------------------------------------
// 连接建立

void AgentBridge::OnTicket(const std::string& ticketId, const std::string& url) {
  const auto query = ParseQuery(url);
  auto get = [&query](const char* key) {
    const auto it = query.find(key);
    return it == query.end() ? std::string() : it->second;
  };
  const std::string relay = get("mcpRelayUrl");
  std::string reason;
  if (get("protocolVersion") != "2") {
    reason = "Unsupported extension protocol version";
  } else if (get("token") != config_.token) {
    reason = "Invitation token does not match this machine";
  } else if (relay.rfind("ws://127.0.0.1:", 0) != 0 && relay.rfind("ws://[::1]:", 0) != 0) {
    reason = "Rejected non-local extension connection invitation";
  }
  if (!reason.empty()) {
    Ack(ticketId, false, 0, reason);
    return;
  }
  const std::wstring relayUrl = FromUtf8(relay);
  // 同一个 relay 的重复邀请沿用已有连接（与 Pi 扩展一致）
  for (auto& item : connections_) {
    Connection& existing = *item.second;
    if (existing.relayUrl == relayUrl && !existing.closing) {
      if (existing.open) {
        Ack(ticketId, true, existing.tabId, std::string());
      } else {
        existing.extraTickets.push_back(ticketId);
      }
      return;
    }
  }
  auto connection = std::make_unique<Connection>();
  connection->id = nextConnectionId_++;
  connection->ticketId = ticketId;
  connection->relayUrl = relayUrl;
  const uint32_t id = connection->id;
  connections_[id] = std::move(connection);
  std::weak_ptr<int> alive = alive_;
  host_.LeaseAgentTab([this, alive, id](uint32_t tabId, ICoreWebView2* webview, const std::string& error) {
    if (alive.expired()) {
      return;
    }
    OnTabLeased(id, tabId, webview, error);
  });
}

void AgentBridge::OnTabLeased(uint32_t connectionId, uint32_t tabId, ICoreWebView2* webview,
                              const std::string& error) {
  Connection* connection = Find(connectionId);
  if (!connection) {
    if (tabId) {
      host_.ReturnAgentTab(tabId);
    }
    return;
  }
  if (!error.empty() || !webview || !tabId) {
    const std::string reason = error.empty() ? std::string("No background tab is available") : error;
    Ack(connection->ticketId, false, 0, reason);
    for (const std::string& extra : connection->extraTickets) {
      Ack(extra, false, 0, reason);
    }
    if (tabId) {
      host_.ReturnAgentTab(tabId);
    }
    connections_.erase(connectionId);
    return;
  }
  connection->tabId = tabId;
  connection->webview = webview;
  connection->socket = std::make_shared<net::WebSocket>();
  // 读线程：连上 relay 后把收到的每条消息转给 UI 线程；断开时通知一次
  std::thread([hwnd = hwnd_, connectionId, socket = connection->socket, url = connection->relayUrl]() {
    std::string error;
    if (!socket->Connect(url, 5000, &error)) {
      PostString(hwnd, kMsgRelayOpen, connectionId, error.empty() ? std::string("WebSocket connect failed") : error);
      return;
    }
    PostString(hwnd, kMsgRelayOpen, connectionId, std::string());
    std::string message;
    while (socket->Receive(message)) {
      PostString(hwnd, kMsgRelayMessage, connectionId, std::move(message));
      message.clear();
    }
    PostMessageW(hwnd, kMsgRelayClosed, connectionId, 0);
  }).detach();
}

void AgentBridge::OnRelayOpen(uint32_t connectionId, const std::string& error) {
  Connection* connection = Find(connectionId);
  if (!connection) {
    return;
  }
  if (!error.empty()) {
    const std::string reason = "Failed to connect to MCP relay: " + error;
    Ack(connection->ticketId, false, 0, reason);
    for (const std::string& extra : connection->extraTickets) {
      Ack(extra, false, 0, reason);
    }
    const uint32_t tabId = connection->tabId;
    connections_.erase(connectionId);  // 读线程已退出，不会再发关闭通知
    if (tabId) {
      host_.ReturnAgentTab(tabId);
    }
    return;
  }
  connection->open = true;
  // 宣布本连接独占的后台标签，再握手；relay 收到后会 attach 并开始发 CDP
  Send(*connection, "{\"method\":\"chrome.tabs.onCreated\",\"params\":[" + host_.AgentTabInfo(connection->tabId) + "]}");
  Send(*connection, "{\"method\":\"extension.initialized\",\"params\":[]}");
  Ack(connection->ticketId, true, connection->tabId, std::string());
  for (const std::string& extra : connection->extraTickets) {
    Ack(extra, true, connection->tabId, std::string());
  }
  connection->extraTickets.clear();
}

// ---------------------------------------------------------------------------
// relay 指令

void AgentBridge::OnRelayMessage(uint32_t connectionId, const std::string& text) {
  Connection* found = Find(connectionId);
  if (!found || found->closing) {
    return;
  }
  Connection& connection = *found;
  json::Value message;
  if (!json::Parse(text, message)) {
    Send(connection, "{\"error\":{\"code\":-32700,\"message\":\"Error parsing message\"}}");
    return;
  }
  const std::string idJson = json::Serialize(message["id"]);
  const std::string& method = message["method"].AsString();
  const json::Array& params = message["params"].Items();

  if (method == "chrome.tabs.create" || method == "chrome.tabs.remove") {
    Fail(connection, idJson,
         "Single fixed-tab mode: use navigation to reuse the current tab; disconnect instead of closing it");
    return;
  }
  if (method != "chrome.debugger.attach" && method != "chrome.debugger.detach" &&
      method != "chrome.debugger.sendCommand") {
    Fail(connection, idJson, "Unknown method: " + method);
    return;
  }
  const json::Value& target = params.empty() ? kNull : params[0];
  if (target["tabId"].AsNumber(-1) != static_cast<double>(connection.tabId)) {
    Fail(connection, idJson, "Background command must target this connection's owned tab");
    return;
  }
  if (method == "chrome.debugger.attach") {
    Attach(connection, idJson);
    return;
  }
  if (method == "chrome.debugger.detach") {
    ReleaseSession(connection);
    Reply(connection, idJson, "{}");
    return;
  }
  if (!connection.attached) {
    Fail(connection, idJson, "Debugger is not attached to the tab with id: " + std::to_string(connection.tabId) + ".");
    return;
  }
  const std::string cdpMethod = params.size() > 1 ? params[1].AsString() : std::string();
  if (cdpMethod.empty()) {
    Fail(connection, idJson, "Missing CDP method");
    return;
  }
  if (cdpMethod == "Target.createTarget" || cdpMethod == "Target.closeTarget" || cdpMethod == "Page.close") {
    Fail(connection, idJson, "Single fixed-tab mode: creating or closing tabs is disabled");
    return;
  }
  if (cdpMethod == "Page.bringToFront" || cdpMethod == "Target.activateTarget") {
    Reply(connection, idJson, "{}");  // 后台模式：不把标签或窗口拉到前台
    return;
  }
  const std::string childSession = target["sessionId"].AsString();
  if (!childSession.empty() && !connection.childSessions.count(childSession)) {
    Fail(connection, idJson, "{\"code\":-32001,\"message\":\"Session with given id not found.\"}");
    return;
  }
  const std::string session = childSession.empty() ? connection.rootSession : childSession;
  const json::Value& cdpParams = params.size() > 2 ? params[2] : kNull;
  if (childSession.empty() && cdpMethod == "Emulation.setFocusEmulationEnabled") {
    // 焦点模拟由桥按唤醒状态开关：记下 Playwright 的意图，醒着时立即生效
    connection.focusDesired = cdpParams["enabled"].AsBool();
    if (!config_.renderOnDemand) {
      Wake(connection);
    }
    if (connection.awake && connection.focusApplied != connection.focusDesired) {
      Internal(connection, connection.rootSession, "Emulation.setFocusEmulationEnabled",
               connection.focusDesired ? "{\"enabled\":true}" : "{\"enabled\":false}");
      connection.focusApplied = connection.focusDesired;
    }
    Reply(connection, idJson, "{}");
    return;
  }
  if (cdpMethod == "Page.startScreencast") {
    connection.screencast = true;
  } else if (cdpMethod == "Page.stopScreencast") {
    connection.screencast = false;
  }
  // 要出帧的命令：截图、录屏、输入事件（命中测试与事件分发要用当前帧）
  const bool needsRender = cdpMethod.rfind("Input.", 0) == 0 || cdpMethod == "Page.captureScreenshot" ||
                           cdpMethod == "Page.startScreencast" || cdpMethod == "Page.screencastFrameAck" ||
                           cdpMethod == "Page.printToPDF";
  if (!config_.renderOnDemand || needsRender || connection.awake) {
    Wake(connection);
  } else {
    host_.ResumeAgentTab(connection.tabId);  // 隐藏执行：挂起了就先恢复，但不显示
    connection.lastActivity = GetTickCount64();
    const bool awaitsPromise = (cdpMethod == "Runtime.evaluate" || cdpMethod == "Runtime.callFunctionOn") &&
                               cdpParams["awaitPromise"].AsBool();
    if (awaitsPromise) {
      // 可能在等 rAF / 渲染：等 renderWaitMs 还没返回就唤醒
      connection.renderWatch[idJson] = GetTickCount64();
      EnsureIdleTimer();
    }
  }
  Execute(connection, idJson, session, cdpMethod, cdpParams.IsObject() ? json::Serialize(cdpParams) : "{}");
  if (cdpMethod == "Network.enable") {
    // 同一会话上按顺序执行：网址拦截紧跟在开启网络域之后生效（iframe 子会话各自开启，各自补上）
    ApplyBlockedUrls(connection, session);
  }
}

void AgentBridge::ApplyBlockedUrls(Connection& connection, const std::string& session) {
  if (config_.blockedUrls.empty() || session.empty()) {
    return;
  }
  std::string params = "{\"urls\":[";
  for (size_t i = 0; i < config_.blockedUrls.size(); ++i) {
    if (i) {
      params += ',';
    }
    json::AppendQuoted(params, config_.blockedUrls[i]);
  }
  params += "]}";
  Internal(connection, session, "Network.setBlockedURLs", params);
}

void AgentBridge::Attach(Connection& connection, const std::string& idJson) {
  if (connection.attached) {
    Reply(connection, idJson, "{}");
    return;
  }
  connection.attachWaiters.push_back(idJson);
  if (connection.attaching) {
    return;
  }
  connection.attaching = true;
  Subscribe(connection);  // 先订阅：附加成功后的第一批事件不能丢
  const uint32_t connectionId = connection.id;
  std::weak_ptr<int> alive = alive_;
  // 在默认会话上找到页面自己的 target，再给本连接单开一个扁平会话
  Internal(connection, std::string(), "Target.getTargetInfo", "{}",
           [this, alive, connectionId](HRESULT hr, const std::string& body) {
             if (alive.expired()) {
               return;
             }
             Connection* current = Find(connectionId);
             if (!current) {
               return;
             }
             json::Value info;
             const std::string targetId =
                 SUCCEEDED(hr) && json::Parse(body, info) ? info["targetInfo"]["targetId"].AsString() : std::string();
             if (targetId.empty()) {
               FinishAttach(connectionId, std::string(), "Target.getTargetInfo failed: " + body);
               return;
             }
             json::Value request = json::Value::MakeObject();
             request.Set("targetId", json::Value::String(targetId));
             request.Set("flatten", json::Value::Bool(true));
             Internal(*current, std::string(), "Target.attachToTarget", json::Serialize(request),
                      [this, alive, connectionId](HRESULT hr2, const std::string& body2) {
                        if (alive.expired()) {
                          return;
                        }
                        json::Value attached;
                        const std::string sessionId = SUCCEEDED(hr2) && json::Parse(body2, attached)
                                                          ? attached["sessionId"].AsString()
                                                          : std::string();
                        FinishAttach(connectionId, sessionId,
                                     sessionId.empty() ? "Target.attachToTarget failed: " + body2 : std::string());
                      });
           });
}

void AgentBridge::FinishAttach(uint32_t connectionId, const std::string& sessionId, const std::string& error) {
  Connection* found = Find(connectionId);
  if (!found) {
    return;
  }
  Connection& connection = *found;
  connection.attaching = false;
  std::vector<std::string> waiters;
  waiters.swap(connection.attachWaiters);
  if (!error.empty() || connection.closing) {
    Unsubscribe(connection);
    for (const std::string& idJson : waiters) {
      Fail(connection, idJson, error.empty() ? std::string("Connection is closing") : error);
    }
    return;
  }
  connection.rootSession = sessionId;
  connection.attached = true;
  for (const std::string& idJson : waiters) {
    Reply(connection, idJson, "{}");
  }
}

void AgentBridge::Execute(Connection& connection, const std::string& idJson, const std::string& session,
                          const std::string& method, const std::string& paramsJson) {
  ComPtr<ICoreWebView2_11> webview11;
  if (!connection.webview || FAILED(connection.webview.As(&webview11))) {
    Fail(connection, idJson, "This WebView2 runtime cannot address CDP sessions");
    return;
  }
  const uint32_t connectionId = connection.id;
  std::weak_ptr<int> alive = alive_;
  auto handler = Callback<ICoreWebView2CallDevToolsProtocolMethodCompletedHandler>(
      [this, alive, connectionId, idJson, method](HRESULT result, LPCWSTR resultJson) -> HRESULT {
        if (!alive.expired()) {
          OnCommandDone(connectionId, idJson, method, result, resultJson);
        }
        return S_OK;
      });
  const std::wstring wideSession = FromUtf8(session);
  const std::wstring wideMethod = FromUtf8(method);
  const std::wstring wideParams = FromUtf8(paramsJson);
  ++connection.pending;
  connection.lastActivity = GetTickCount64();
  const HRESULT hr = webview11->CallDevToolsProtocolMethodForSession(wideSession.c_str(), wideMethod.c_str(),
                                                                     wideParams.c_str(), handler.Get());
  if (FAILED(hr)) {
    --connection.pending;
    Fail(connection, idJson, HresultText("CDP call could not be issued", hr));
  }
}

void AgentBridge::OnCommandDone(uint32_t connectionId, const std::string& idJson, const std::string& method,
                                HRESULT result, LPCWSTR resultJson) {
  Connection* found = Find(connectionId);
  if (!found) {
    return;  // 连接已断开，标签也已归还
  }
  Connection& connection = *found;
  if (connection.pending > 0) {
    --connection.pending;
  }
  connection.renderWatch.erase(idJson);
  connection.lastActivity = GetTickCount64();
  std::string body = resultJson ? ToUtf8(resultJson) : std::string();
  if (SUCCEEDED(result)) {
    Reply(connection, idJson, body.empty() ? std::string("{}") : body);
    return;
  }
  // 失败时 WebView2 给的是 CDP 错误对象原文；chrome.debugger 也以这段 JSON 作为错误消息
  if (body.empty()) {
    body = HresultText("CDP command failed: " + method, result);
  }
  Fail(connection, idJson, body);
}

void AgentBridge::Internal(Connection& connection, const std::string& session, const char* method,
                           const std::string& paramsJson, std::function<void(HRESULT, const std::string&)> done) {
  if (!connection.webview) {
    if (done) {
      done(E_ABORT, std::string());
    }
    return;
  }
  auto handler = Callback<ICoreWebView2CallDevToolsProtocolMethodCompletedHandler>(
      [done](HRESULT result, LPCWSTR resultJson) -> HRESULT {
        if (done) {
          done(result, resultJson ? ToUtf8(resultJson) : std::string());
        }
        return S_OK;
      });
  const std::wstring wideMethod = FromUtf8(method);
  const std::wstring wideParams = FromUtf8(paramsJson);
  HRESULT hr = E_NOINTERFACE;
  if (session.empty()) {
    hr = connection.webview->CallDevToolsProtocolMethod(wideMethod.c_str(), wideParams.c_str(), handler.Get());
  } else {
    ComPtr<ICoreWebView2_11> webview11;
    if (SUCCEEDED(connection.webview.As(&webview11))) {
      const std::wstring wideSession = FromUtf8(session);
      hr = webview11->CallDevToolsProtocolMethodForSession(wideSession.c_str(), wideMethod.c_str(), wideParams.c_str(),
                                                           handler.Get());
    }
  }
  if (FAILED(hr) && done) {
    done(hr, std::string());
  }
}

// ---------------------------------------------------------------------------
// 唤醒与休眠

void AgentBridge::Wake(Connection& connection) {
  connection.lastActivity = GetTickCount64();
  if (connection.awake) {
    return;
  }
  connection.awake = true;
  host_.SetAgentTabAwake(connection.tabId, true);
  if (connection.attached && connection.focusDesired && !connection.focusApplied) {
    // 同一会话上的命令按顺序执行：焦点模拟一定先于随后转发的命令生效
    Internal(connection, connection.rootSession, "Emulation.setFocusEmulationEnabled", "{\"enabled\":true}");
    connection.focusApplied = true;
  }
  EnsureIdleTimer();
}

void AgentBridge::EnsureIdleTimer() {
  if (!idleTimerRunning_) {
    // 按需出帧要在 renderWaitMs 内发现卡住的 evaluate：间隔取两者较小者
    const UINT interval = config_.renderOnDemand ? std::min<UINT>(100, config_.renderWaitMs) : 250;
    idleTimerRunning_ = SetTimer(hwnd_, kIdleTimer, interval, nullptr) != 0;
  }
}

void AgentBridge::Sleep(Connection& connection) {
  if (!connection.awake) {
    return;
  }
  connection.awake = false;
  if (connection.attached && connection.focusApplied) {
    // 焦点模拟开着时页面会被当成可见，隐藏后定时器照样满速跑（实测 15% 单核）
    Internal(connection, connection.rootSession, "Emulation.setFocusEmulationEnabled", "{\"enabled\":false}");
    connection.focusApplied = false;
  }
  host_.SetAgentTabAwake(connection.tabId, false);
}

void AgentBridge::OnIdleTimer() {
  const ULONGLONG now = GetTickCount64();
  bool anyAwake = false;
  for (auto& item : connections_) {
    Connection& connection = *item.second;
    if (!connection.awake && !connection.renderWatch.empty()) {
      bool stuck = false;
      for (const auto& [id, since] : connection.renderWatch) {
        stuck = stuck || now - since >= config_.renderWaitMs;
      }
      if (stuck) {
        connection.renderWatch.clear();
        Wake(connection);  // evaluate 在等渲染：出帧后 rAF 才会回调
      } else {
        anyAwake = true;  // 计时器还得接着跑
      }
    }
    if (!connection.awake) {
      continue;
    }
    if (connection.pending == 0 && !connection.screencast && now - connection.lastActivity >= config_.graceMs) {
      Sleep(connection);
    } else {
      anyAwake = true;
    }
  }
  if (!anyAwake && idleTimerRunning_) {
    KillTimer(hwnd_, kIdleTimer);
    idleTimerRunning_ = false;
  }
}

// ---------------------------------------------------------------------------
// 事件

void AgentBridge::Subscribe(Connection& connection) {
  if (!connection.subscriptions.empty() || !connection.webview) {
    return;
  }
  const uint32_t connectionId = connection.id;
  std::weak_ptr<int> alive = alive_;
  for (const wchar_t* name : kCdpEvents) {
    ComPtr<ICoreWebView2DevToolsProtocolEventReceiver> receiver;
    if (FAILED(connection.webview->GetDevToolsProtocolEventReceiver(name, receiver.GetAddressOf()))) {
      continue;
    }
    const std::string method = ToUtf8(name);
    EventRegistrationToken token{};
    const HRESULT hr = receiver->add_DevToolsProtocolEventReceived(
        Callback<ICoreWebView2DevToolsProtocolEventReceivedEventHandler>(
            [this, alive, connectionId, method](ICoreWebView2*,
                                                ICoreWebView2DevToolsProtocolEventReceivedEventArgs* args) -> HRESULT {
              if (!alive.expired()) {
                OnCdpEvent(connectionId, method, args);
              }
              return S_OK;
            })
            .Get(),
        &token);
    if (SUCCEEDED(hr)) {
      connection.subscriptions.emplace_back(receiver, token);
    }
  }
}

void AgentBridge::Unsubscribe(Connection& connection) {
  for (auto& [receiver, token] : connection.subscriptions) {
    receiver->remove_DevToolsProtocolEventReceived(token);
  }
  connection.subscriptions.clear();
}

void AgentBridge::OnCdpEvent(uint32_t connectionId, const std::string& method,
                             ICoreWebView2DevToolsProtocolEventReceivedEventArgs* args) {
  Connection* found = Find(connectionId);
  if (!found || found->closing) {
    return;
  }
  Connection& connection = *found;
  std::string sessionId;
  ComPtr<ICoreWebView2DevToolsProtocolEventReceivedEventArgs2> args2;
  if (SUCCEEDED(args->QueryInterface(IID_PPV_ARGS(args2.GetAddressOf())))) {
    CoTaskString session;
    if (SUCCEEDED(args2->get_SessionId(session.Put()))) {
      sessionId = ToUtf8(session.Str());
    }
  }
  const bool fromRoot = connection.attached && !sessionId.empty() && sessionId == connection.rootSession;
  const bool fromChild = !fromRoot && !sessionId.empty() && connection.childSessions.count(sessionId) > 0;
  const bool targetEvent = method == "Target.attachedToTarget" || method == "Target.detachedFromTarget";
  if (!fromRoot && !fromChild && !(targetEvent && sessionId.empty())) {
    return;  // 默认会话或别的会话的事件，与本连接无关
  }
  CoTaskString parameters;
  args->get_ParameterObjectAsJson(parameters.Put());
  std::string body = ToUtf8(parameters.Str());
  if (targetEvent) {
    json::Value params;
    const std::string child = json::Parse(body, params) ? params["sessionId"].AsString() : std::string();
    if (sessionId.empty()) {
      // 默认会话上的 detachedFromTarget：本连接的页面会话被关掉了（渲染进程崩溃、页面关闭）
      if (method == "Target.detachedFromTarget" && connection.attached && child == connection.rootSession) {
        connection.attached = false;
        connection.rootSession.clear();
        connection.childSessions.clear();
        Send(connection, "{\"method\":\"chrome.debugger.onDetach\",\"params\":[{\"tabId\":" +
                             std::to_string(connection.tabId) + "},\"target_closed\"]}");
        if (connection.socket) {
          connection.socket->Close();  // 与扩展一致：最后一个受控标签脱离后关闭连接
        }
      }
      return;
    }
    if (!child.empty()) {
      if (method == "Target.attachedToTarget") {
        connection.childSessions.insert(child);
      } else {
        connection.childSessions.erase(child);
      }
    }
  }
  std::string out = "{\"method\":\"chrome.debugger.onEvent\",\"params\":[{\"tabId\":" + std::to_string(connection.tabId);
  if (fromChild) {
    out += ",\"sessionId\":";
    json::AppendQuoted(out, sessionId);
  }
  out += "},";
  json::AppendQuoted(out, method);
  out += ",";
  out += body.empty() ? std::string("{}") : body;
  out += "]}";
  Send(connection, std::move(out));
}

// ---------------------------------------------------------------------------
// 收尾

void AgentBridge::ReleaseSession(Connection& connection) {
  if (connection.awake) {
    connection.awake = false;
    host_.SetAgentTabAwake(connection.tabId, false);
  }
  connection.focusApplied = false;
  connection.focusDesired = false;
  connection.screencast = false;
  connection.renderWatch.clear();
  if (connection.attached && connection.webview && !connection.rootSession.empty()) {
    // 断开会话：Playwright 开过的域、注入的脚本、焦点模拟随会话一起清掉
    json::Value request = json::Value::MakeObject();
    request.Set("sessionId", json::Value::String(connection.rootSession));
    Internal(connection, std::string(), "Target.detachFromTarget", json::Serialize(request));
  }
  connection.attached = false;
  connection.rootSession.clear();
  connection.childSessions.clear();
  Unsubscribe(connection);
}

void AgentBridge::OnAgentTabGone(uint32_t tabId) {
  for (auto& item : connections_) {
    Connection& connection = *item.second;
    if (connection.tabId != tabId || connection.closing) {
      continue;
    }
    connection.closing = true;
    const std::string id = std::to_string(tabId);
    if (connection.attached) {
      Send(connection, "{\"method\":\"chrome.debugger.onDetach\",\"params\":[{\"tabId\":" + id + "},\"target_closed\"]}");
    }
    Send(connection, "{\"method\":\"chrome.tabs.onRemoved\",\"params\":[" + id +
                         ",{\"windowId\":1,\"isWindowClosing\":false}]}");
    Unsubscribe(connection);
    connection.attached = false;  // 页面已经没了，会话随之消失，不再 detach
    connection.rootSession.clear();
    connection.childSessions.clear();
    connection.awake = false;
    connection.webview.Reset();
    connection.tabId = 0;  // 标签已经关掉，断开时不再归还
    if (connection.socket) {
      connection.socket->Close();  // 读线程随后发来关闭通知，由 Finish 清理
    }
  }
}

void AgentBridge::Reply(Connection& connection, const std::string& idJson, const std::string& resultJson) {
  Send(connection, "{\"id\":" + idJson + ",\"result\":" + resultJson + "}");
}

void AgentBridge::Fail(Connection& connection, const std::string& idJson, const std::string& reason) {
  std::string out = "{\"id\":" + idJson + ",\"error\":";
  json::AppendQuoted(out, reason);
  out += "}";
  Send(connection, std::move(out));
}

void AgentBridge::Send(Connection& connection, std::string message) {
  if (connection.open && connection.socket) {
    connection.socket->Send(std::move(message));
  }
}

void AgentBridge::Ack(const std::string& ticketId, bool success, uint32_t tabId, const std::string& error) {
  if (!broker_ || ticketId.empty()) {
    return;
  }
  json::Value body = json::Value::MakeObject();
  body.Set("id", json::Value::String(ticketId));
  body.Set("success", json::Value::Bool(success));
  if (success) {
    body.Set("tabId", json::Value::Number(static_cast<double>(tabId)));
  } else {
    body.Set("error", json::Value::String(error.substr(0, 300)));
  }
  {
    std::lock_guard<std::mutex> lock(broker_->mutex);
    broker_->acks.push_back(json::Serialize(body));
  }
  broker_->wake.notify_one();
}

void AgentBridge::Finish(uint32_t connectionId) {
  Connection* found = Find(connectionId);
  if (!found) {
    return;
  }
  Connection& connection = *found;
  connection.closing = true;
  ReleaseSession(connection);
  if (connection.socket) {
    connection.socket->Close();
  }
  const uint32_t tabId = connection.tabId;
  connections_.erase(connectionId);
  if (tabId) {
    host_.ReturnAgentTab(tabId);  // 标签与页面留在池里，下一条连接接着用（与扩展的标签池一致）
  }
}

}  // namespace jisu
