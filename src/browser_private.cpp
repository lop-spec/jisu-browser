#include "browser_window.h"
#include "browser_url.h"
namespace jisu {
namespace {
std::wstring Quote(const std::wstring& text) {
  std::wstring out=L"\"";size_t slashes=0;
  for(wchar_t c:text){if(c==L'\\'){++slashes;continue;}out.append(c==L'"'?slashes*2+1:slashes,L'\\');out.push_back(c);slashes=0;}
  out.append(slashes*2,L'\\');out.push_back(L'"');return out;
}
}
void BrowserWindow::OpenPrivateWindow() {
  const auto exe=ExeDirectory()+L"\\jisu-browser.exe";
  std::wstring command=Quote(exe)+L" --inprivate --udf "+Quote(profileDir_);
  if(!options_.browserArgs.empty())command+=L" --browser-args "+Quote(options_.browserArgs);
  if(!options_.trackingPrevention)command+=L" --tracking-prevention 0";
  const bool foreground=GetForegroundWindow()==hwnd_;
  if(!foreground)command+=L" --no-activate";
  STARTUPINFOW startup{};startup.cb=sizeof(startup);startup.dwFlags=STARTF_USESHOWWINDOW;
  startup.wShowWindow=foreground?SW_SHOWNORMAL:SW_SHOWNOACTIVATE;
  PROCESS_INFORMATION process{};
  if(!CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,ExeDirectory().c_str(),&startup,&process)){
    if(foreground)ShowNotice(L"无法启动无痕窗口，请重试。");return;
  }
  CloseHandle(process.hThread);CloseHandle(process.hProcess);
}
}  // namespace jisu
