#include "media_session.h"
#include "json.h"
#include <iostream>
#include <cstdlib>
using namespace jisu;
void require(bool ok,const char* message){if(!ok){std::cerr<<"FAIL media host: "<<message<<std::endl;std::exit(1);}}
int wmain(int argc,wchar_t** argv){
  if(argc!=2&&argc!=3){std::cerr<<"media-host-test <runtime directory> [http fixture URL]\n";return 2;}
  WNDCLASSW wc{};wc.lpfnWndProc=DefWindowProcW;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"JisuMediaHiddenGate";require(RegisterClassW(&wc)!=0,"register host");
  HWND window=CreateWindowExW(WS_EX_NOACTIVATE,wc.lpszClassName,L"",WS_OVERLAPPEDWINDOW,0,0,1024,768,nullptr,nullptr,wc.hInstance,nullptr);
  HWND surface=CreateWindowExW(0,L"STATIC",L"",WS_CHILD|WS_CLIPCHILDREN,0,0,960,540,window,nullptr,wc.hInstance,nullptr);
  require(window&&surface&&!IsWindowVisible(window),"hidden host");
  wchar_t temp[MAX_PATH];GetTempPathW(MAX_PATH,temp);const auto profile=std::wstring(temp)+L"jisu-media-host-"+std::to_wstring(GetCurrentProcessId());EnsureDirectory(profile);
  MediaSession media;require(!media.Running(),"lazy no process");require(media.Start(surface,argv[1],profile),"start isolated GUI child");
  if(argc==3){const auto url=ToUtf8(argv[2]);auto load=json::Value::MakeObject();load.Set("op",json::Value::String("load"));load.Set("url",json::Value::String(url));load.Set("title",json::Value::String("synthetic fixture"));media.Send(json::Serialize(load));}
  bool loaded=false,progress=false,paused=false,sought=false,speed=false;json::Value last;int stage=0;const auto deadline=GetTickCount64()+30000;
  while(GetTickCount64()<deadline){
    MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}
    for(const auto& text:media.Poll()){
      json::Value e;if(!json::Parse(text,e))continue;
      if(e["type"].AsString()=="player.error"){std::cerr<<text<<std::endl;if(e["fatal"].AsBool())return 3;}
      if(e["type"].AsString()=="player.ready"&&argc==2){stage=4;break;}
      if(e["type"].AsString()=="player.loaded")loaded=true;
      if(e["type"].AsString()!="player.status")continue;last=e;
      if(stage==0&&loaded&&e["time-pos"].AsNumber()>0.5&&e["video-params"]["w"].AsNumber()>0){progress=true;media.Send("{\"op\":\"pause\"}");stage=1;}
      else if(stage==1&&e["pause"].AsBool()){paused=true;media.Send("{\"op\":\"seek\",\"value\":1.5,\"absolute\":true}");stage=2;}
      else if(stage==2&&e["time-pos"].AsNumber()>1.3){sought=true;media.Send("{\"op\":\"set\",\"key\":\"speed\",\"value\":\"1.5\"}");stage=3;}
      else if(stage==3&&e["speed"].AsNumber()==1.5){speed=true;stage=4;}
    }
    require(!IsWindowVisible(window),"host never shown");
    DWORD foregroundPid=0;GetWindowThreadProcessId(GetForegroundWindow(),&foregroundPid);require(foregroundPid!=GetCurrentProcessId(),"test host never activated");
    if(stage==4)break;Sleep(25);
  }
  require(argc==2?stage==4:loaded&&progress&&paused&&sought&&speed,"ready / decode progress pause seek speed");
  std::cout<<"PASS native embedded media; "<<json::Serialize(last)<<std::endl;
  media.Stop();require(!media.Running(),"process released");DestroyWindow(window);
  DeleteFileW((profile+L"\\PlayerResume.json").c_str());RemoveDirectoryW(profile.c_str());
  std::cout<<"PASS hidden/no-activate; no browser launched; bounded IPC; child lifecycle; resume saved outside ordinary browser history\n";
}
