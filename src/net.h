// 本机 HTTP / WebSocket 客户端（WinHTTP，同步调用，放在后台线程里用）。只连 127.0.0.1，不走代理。
#pragma once

#include "common.h"

#include <winhttp.h>

#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace jisu::net {

struct HttpResult {
  DWORD status = 0;
  std::string body;
  std::string error;
};

// method 为 L"GET"/L"POST"；bearer 非空时带 Authorization: Bearer 头。不带 Origin 头。
HttpResult Request(const wchar_t* method, const std::wstring& url, const std::string& body, const std::string& bearer,
                   DWORD timeoutMs);

class WebSocket {
 public:
  WebSocket() = default;
  ~WebSocket();
  WebSocket(const WebSocket&) = delete;
  WebSocket& operator=(const WebSocket&) = delete;

  bool Connect(const std::wstring& url, DWORD timeoutMs, std::string* error);  // ws:// 或 wss://
  // 放进发送队列立即返回，由连接自己的写线程按顺序发出：界面线程不会被大消息（截图）或对端读得慢卡住
  bool Send(std::string text);
  bool Receive(std::string& message);  // 阻塞到一条完整消息；连接关闭或出错返回 false
  void Close();                        // 任意线程；让阻塞中的 Receive 返回，写线程丢弃未发消息后退出

 private:
  void WriterMain();

  std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<std::string> outbox_;
  std::thread writer_;
  HINTERNET session_ = nullptr;
  HINTERNET connection_ = nullptr;
  HINTERNET socket_ = nullptr;
  bool closed_ = false;
};

}  // namespace jisu::net
