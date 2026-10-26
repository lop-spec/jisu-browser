#include <windows.h>
#include <cassert>
#include <string>
#include "options.h"

static HWND Find(DWORD pid) {
  struct Search { DWORD pid; HWND hwnd; } search{pid, nullptr};
  EnumWindows([](HWND hwnd, LPARAM data)->BOOL {
    auto& s=*reinterpret_cast<Search*>(data); DWORD owner=0; GetWindowThreadProcessId(hwnd,&owner);
    wchar_t cls[128]{}; GetClassNameW(hwnd,cls,128);
    if(owner==s.pid && std::wstring(cls).starts_with(L"Jisu")) {s.hwnd=hwnd;return FALSE;} return TRUE;
  },reinterpret_cast<LPARAM>(&search));return search.hwnd;
}
static PROCESS_INFORMATION Start(const std::wstring& exe, const std::wstring& args) {
  STARTUPINFOW si{sizeof(si)};si.dwFlags=STARTF_USESHOWWINDOW;si.wShowWindow=SW_SHOWNOACTIVATE;
  PROCESS_INFORMATION pi{};std::wstring command=L"\""+exe+L"\" "+args;
  if(!CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi)) ExitProcess(2);
  return pi;
}
static HWND Await(PROCESS_INFORMATION& pi) {for(int i=0;i<150;i++){if(HWND w=Find(pi.dwProcessId))return w;Sleep(100);}return nullptr;}
static void Close(PROCESS_INFORMATION& pi, HWND hwnd) {
  if(hwnd)PostMessageW(hwnd,WM_CLOSE,0,0);
  if(WaitForSingleObject(pi.hProcess,10000)!=WAIT_OBJECT_0)TerminateProcess(pi.hProcess,3);
  CloseHandle(pi.hThread);CloseHandle(pi.hProcess);
}
int wmain(int argc,wchar_t** argv) {
  if(argc!=2)return 2;
  const auto parsed=jisu::ParseOptions(L"jisu.exe --app=http://127.0.0.1:30141/ --app-id pi-web --app-title \"Pi Web\" --no-activate");
  if(!parsed.appMode||!parsed.noAgent||parsed.spare||parsed.appId!=L"pi-web"||parsed.startUrl!=L"http://127.0.0.1:30141/"||!parsed.noActivate)return 3;
  const auto daily=jisu::ParseOptions(L"jisu.exe https://example.com");
  if(daily.appMode||daily.InstanceKey()==parsed.InstanceKey())return 4;
  wchar_t temp[MAX_PATH]{};GetTempPathW(MAX_PATH,temp);
  const std::wstring udf=std::wstring(temp)+L"jisu-app-test-"+std::to_wstring(GetCurrentProcessId());
  const std::wstring common=L" --no-activate --no-agent --no-spare --udf \""+udf+L"\"";
  auto browser=Start(argv[1],common);HWND bw=Await(browser);
  auto app=Start(argv[1],common+L" --app about:blank --app-id ci-pi-web --app-title \"Pi Web\"");HWND aw=Await(app);
  bool ok=bw&&aw&&bw!=aw&&GetPropW(aw,L"JisuBrowser.AppMode")&&!GetPropW(bw,L"JisuBrowser.AppMode");
  wchar_t title[128]{};GetWindowTextW(aw,title,128);ok=ok&&std::wstring(title)==L"Pi Web";
  ok=ok&&FindWindowExW(aw,nullptr,L"EDIT",nullptr)==nullptr&&FindWindowExW(bw,nullptr,L"EDIT",nullptr)!=nullptr;
  ok=ok&&(GetWindowLongW(aw,GWL_STYLE)&WS_CAPTION)==WS_CAPTION;
  auto again=Start(argv[1],common+L" --app about:blank --app-id ci-pi-web --app-title \"Pi Web\"");
  ok=ok&&WaitForSingleObject(again.hProcess,15000)==WAIT_OBJECT_0&&IsWindow(aw)&&!Find(again.dwProcessId);
  CloseHandle(again.hThread);CloseHandle(again.hProcess);
  Close(app,aw);ok=ok&&IsWindow(bw);Close(browser,bw);
  return ok?0:5;
}
