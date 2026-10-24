#include "net.h"

#include <vector>

namespace jisu::net {
namespace {

struct UrlParts {
  std::wstring host;
  INTERNET_PORT port = 0;
  std::wstring object;  // 路径 + 查询串
  bool secure = false;
};

bool CrackUrl(std::wstring url, UrlParts& out) {
  // WinHttpCrackUrl 只认 http/https：ws/wss 先换成对应的 http/https
  if (url.rfind(L"ws://", 0) == 0) {
    url = L"http://" + url.substr(5);
  } else if (url.rfind(L"wss://", 0) == 0) {
    url = L"https://" + url.substr(6);
  }
  URL_COMPONENTS parts{};
  parts.dwStructSize = static_cast<DWORD>(sizeof(parts));
  parts.dwSchemeLength = static_cast<DWORD>(-1);
  parts.dwHostNameLength = static_cast<DWORD>(-1);
  parts.dwUrlPathLength = static_cast<DWORD>(-1);
  parts.dwExtraInfoLength = static_cast<DWORD>(-1);
  if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts) || !parts.lpszHostName) {
    return false;
  }
  out.host.assign(parts.lpszHostName, parts.dwHostNameLength);
  out.port = parts.nPort;
  if (parts.lpszUrlPath) {
    out.object.assign(parts.lpszUrlPath, parts.dwUrlPathLength);
  }
  if (parts.lpszExtraInfo) {
    out.object.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
  }
  if (out.object.empty()) {
    out.object = L"/";
  }
  out.secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
  return true;
}

HINTERNET OpenSession(DWORD timeoutMs, bool infiniteReceive) {
  HINTERNET session = WinHttpOpen(L"JisuBrowser", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
                                  WINHTTP_NO_PROXY_BYPASS, 0);
  if (session) {
    const int timeout = static_cast<int>(timeoutMs);
    // WebSocket 两次消息之间可能隔很久（等模型思考），接收不能超时
    WinHttpSetTimeouts(session, timeout, timeout, timeout, infiniteReceive ? 0 : timeout);
  }
  return session;
}

}  // namespace

HttpResult Request(const wchar_t* method, const std::wstring& url, const std::string& body, const std::string& bearer,
                   DWORD timeoutMs) {
  HttpResult result;
  UrlParts parts;
  if (!CrackUrl(url, parts)) {
    result.error = "invalid url";
    return result;
  }
  HINTERNET session = OpenSession(timeoutMs, false);
  if (!session) {
    result.error = "WinHttpOpen failed " + std::to_string(GetLastError());
    return result;
  }
  HINTERNET connection = WinHttpConnect(session, parts.host.c_str(), parts.port, 0);
  HINTERNET request = connection ? WinHttpOpenRequest(connection, method, parts.object.c_str(), nullptr,
                                                      WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                      parts.secure ? WINHTTP_FLAG_SECURE : 0)
                                 : nullptr;
  if (request) {
    std::wstring headers = L"Content-Type: application/json\r\n";
    if (!bearer.empty()) {
      headers += L"Authorization: Bearer " + FromUtf8(bearer) + L"\r\n";
    }
    LPVOID data = body.empty() ? WINHTTP_NO_REQUEST_DATA : static_cast<LPVOID>(const_cast<char*>(body.data()));
    const DWORD length = static_cast<DWORD>(body.size());
    BOOL ok = WinHttpSendRequest(request, headers.c_str(), static_cast<DWORD>(-1), data, length, length, 0);
    if (ok) {
      ok = WinHttpReceiveResponse(request, nullptr);
    }
    if (ok) {
      DWORD status = 0;
      DWORD size = static_cast<DWORD>(sizeof(status));
      WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                          &status, &size, WINHTTP_NO_HEADER_INDEX);
      result.status = status;
      for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available) || available == 0) {
          break;
        }
        const size_t offset = result.body.size();
        result.body.resize(offset + available);
        DWORD read = 0;
        if (!WinHttpReadData(request, result.body.data() + offset, available, &read)) {
          result.body.resize(offset);
          break;
        }
        result.body.resize(offset + read);
        if (read == 0 || result.body.size() > (8u << 20)) {
          break;
        }
      }
    } else {
      result.error = "WinHTTP request failed " + std::to_string(GetLastError());
    }
    WinHttpCloseHandle(request);
  } else {
    result.error = "WinHTTP connect failed " + std::to_string(GetLastError());
  }
  if (connection) {
    WinHttpCloseHandle(connection);
  }
  WinHttpCloseHandle(session);
  return result;
}

WebSocket::~WebSocket() {
  Close();
  if (writer_.joinable()) {
    // 最后一个引用可能恰好在写线程上释放：自己不能 join 自己
    if (writer_.get_id() == std::this_thread::get_id()) {
      writer_.detach();
    } else {
      writer_.join();
    }
  }
}

bool WebSocket::Connect(const std::wstring& url, DWORD timeoutMs, std::string* error) {
  UrlParts parts;
  if (!CrackUrl(url, parts)) {
    if (error) {
      *error = "invalid url";
    }
    return false;
  }
  HINTERNET session = OpenSession(timeoutMs, true);
  HINTERNET connection = session ? WinHttpConnect(session, parts.host.c_str(), parts.port, 0) : nullptr;
  HINTERNET request = connection ? WinHttpOpenRequest(connection, L"GET", parts.object.c_str(), nullptr,
                                                      WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                      parts.secure ? WINHTTP_FLAG_SECURE : 0)
                                 : nullptr;
  HINTERNET socket = nullptr;
  DWORD failure = 0;
  if (request && WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0) &&
      WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
      WinHttpReceiveResponse(request, nullptr)) {
    DWORD status = 0;
    DWORD size = static_cast<DWORD>(sizeof(status));
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &size, WINHTTP_NO_HEADER_INDEX);
    if (status == 101) {
      socket = WinHttpWebSocketCompleteUpgrade(request, 0);
    } else {
      failure = status;
    }
  }
  if (!socket && failure == 0) {
    failure = GetLastError();
  }
  if (request) {
    WinHttpCloseHandle(request);
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (socket && !closed_) {
      session_ = session;
      connection_ = connection;
      socket_ = socket;
      writer_ = std::thread(&WebSocket::WriterMain, this);
      return true;
    }
  }
  if (socket) {
    WinHttpCloseHandle(socket);
  }
  if (connection) {
    WinHttpCloseHandle(connection);
  }
  if (session) {
    WinHttpCloseHandle(session);
  }
  if (error) {
    *error = "WebSocket connect failed " + std::to_string(failure);
  }
  return false;
}

bool WebSocket::Send(std::string text) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_ || !socket_) {
      return false;
    }
    outbox_.push_back(std::move(text));
  }
  wake_.notify_one();
  return true;
}

void WebSocket::WriterMain() {
  for (;;) {
    std::string message;
    HINTERNET socket = nullptr;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      wake_.wait(lock, [this] { return closed_ || !outbox_.empty(); });
      if (closed_) {
        return;
      }
      message = std::move(outbox_.front());
      outbox_.pop_front();
      socket = socket_;
    }
    // Close() 关掉句柄时这里以错误返回
    if (WinHttpWebSocketSend(socket, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                             static_cast<PVOID>(message.data()), static_cast<DWORD>(message.size())) != NO_ERROR) {
      return;
    }
  }
}

bool WebSocket::Receive(std::string& message) {
  HINTERNET socket = nullptr;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_ || !socket_) {
      return false;
    }
    socket = socket_;
  }
  message.clear();
  std::vector<char> buffer(64 * 1024);
  for (;;) {
    DWORD read = 0;
    WINHTTP_WEB_SOCKET_BUFFER_TYPE type = WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE;
    // Close() 关掉句柄时这里会以错误返回
    const DWORD result = WinHttpWebSocketReceive(socket, buffer.data(), static_cast<DWORD>(buffer.size()), &read, &type);
    if (result != NO_ERROR || type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
      return false;
    }
    message.append(buffer.data(), read);
    if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE || type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE) {
      return true;
    }
    if (message.size() > (512u << 20)) {
      return false;
    }
  }
}

void WebSocket::Close() {
  HINTERNET socket = nullptr;
  HINTERNET connection = nullptr;
  HINTERNET session = nullptr;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) {
      return;
    }
    closed_ = true;
    outbox_.clear();
    socket = socket_;
    connection = connection_;
    session = session_;
    socket_ = nullptr;
    connection_ = nullptr;
    session_ = nullptr;
  }
  wake_.notify_all();
  if (socket) {
    WinHttpWebSocketShutdown(socket, static_cast<USHORT>(WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS), nullptr, 0);
    WinHttpCloseHandle(socket);
  }
  if (connection) {
    WinHttpCloseHandle(connection);
  }
  if (session) {
    WinHttpCloseHandle(session);
  }
}

}  // namespace jisu::net
