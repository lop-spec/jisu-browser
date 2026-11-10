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
  const bool foreground=GetForegroundWindow()==hwnd_;
  wchar_t module[32768]{};const DWORD length=GetModuleFileNameW(nullptr,module,static_cast<DWORD>(std::size(module)));
  if(!length||length>=std::size(module)){if(foreground)ShowNotice(L"无法取得程序路径，未启动无痕窗口。");return;}
  const std::wstring exe(module,length);const auto directory=ParentDirectory(exe);
  std::wstring command=Quote(exe)+L" --inprivate --udf "+Quote(profileDir_);
  if(!options_.browserArgs.empty())command+=L" --browser-args "+Quote(options_.browserArgs);
  if(!options_.trackingPrevention)command+=L" --tracking-prevention 0";
  if(!foreground)command+=L" --no-activate";
  STARTUPINFOW startup{};startup.cb=sizeof(startup);startup.dwFlags=STARTF_USESHOWWINDOW;
  startup.wShowWindow=foreground?SW_SHOWNORMAL:SW_SHOWNOACTIVATE;
  PROCESS_INFORMATION process{};
  if(!CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,directory.c_str(),&startup,&process)){
    if(foreground)ShowNotice(L"无法启动无痕窗口，请重试。");return;
  }
  CloseHandle(process.hThread);CloseHandle(process.hProcess);
}
}  // namespace jisu
