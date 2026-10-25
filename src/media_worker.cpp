// GUI-subsystem, isolated libmpv host. The browser never loads FFmpeg/libmpv.
#include "common.h"
#include "json.h"
#include <mpv/client.h>
#include <shellapi.h>
#include <shlobj.h>
#include "fullscreen_window.h"
#include <commdlg.h>
#include <algorithm>
#include <atomic>
#include <bcrypt.h>
#include <chrono>
#include <cmath>
#include <thread>
#include <vector>
using namespace jisu;
namespace {
struct Api {
  HMODULE dll = nullptr;
#define MPV_API(name) decltype(&mpv_##name) name = nullptr
  MPV_API(create); MPV_API(initialize); MPV_API(terminate_destroy); MPV_API(set_option_string);
  MPV_API(command); MPV_API(get_property); MPV_API(set_property_string); MPV_API(get_property_string);
  MPV_API(wait_event); MPV_API(free); MPV_API(free_node_contents); MPV_API(error_string);
#undef MPV_API
  bool Load(const std::wstring& path) {
    dll = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!dll) return false;
#define MPV_LOAD(name) name = reinterpret_cast<decltype(name)>(GetProcAddress(dll, "mpv_" #name)); if (!name) return false
    MPV_LOAD(create); MPV_LOAD(initialize); MPV_LOAD(terminate_destroy); MPV_LOAD(set_option_string);
    MPV_LOAD(command); MPV_LOAD(get_property); MPV_LOAD(set_property_string); MPV_LOAD(get_property_string);
    MPV_LOAD(wait_event); MPV_LOAD(free); MPV_LOAD(free_node_contents); MPV_LOAD(error_string);
#undef MPV_LOAD
    return true;
  }
} api;
mpv_handle* player = nullptr;
HWND surface = nullptr, owner = nullptr, mini = nullptr;
FullscreenWindow miniFullscreen;
LRESULT CALLBACK MiniProc(HWND window,UINT message,WPARAM w,LPARAM l){
  if(message==WM_SIZE&&surface&&GetParent(surface)==window){RECT bounds{};GetClientRect(window,&bounds);SetWindowPos(surface,nullptr,0,0,bounds.right,bounds.bottom,SWP_NOACTIVATE|SWP_NOZORDER);return 0;}
  if(message==WM_CLOSE){miniFullscreen.Exit(window);SetParent(surface,owner);mini=nullptr;DestroyWindow(window);return 0;}
  return DefWindowProcW(window,message,w,l);
}
std::thread reader;
std::atomic<bool> quitting{false};
constexpr UINT kCommand = WM_APP + 1;
std::wstring directory;
std::string lastError;
std::string loadedTitle;
std::wstring resumeFile;
json::Value resumes=json::Value::MakeObject();
std::string resumeKey;
ULONGLONG resumeSaved=0;
double resumePosition=-1;
bool enhanced=false;
ULONGLONG qualityTick=0;
double qualityDrops=0;
bool watching=false;
ULONGLONG progressAt=0;
double progressPosition=-1;
int64_t expectedEntry=-1,eventEntry=-1;
double pendingStart = 0, skipIntro = 0, skipOutro = 0;
bool outroSent = false;
void Emit(const json::Value& value) {
  auto text = json::Serialize(value) + '\n'; DWORD wrote;
  WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text.data(), static_cast<DWORD>(text.size()), &wrote, nullptr);
}
void Event(const char* event) { auto v = json::Value::MakeObject(); v.Set("type", json::Value::String(event)); Emit(v); }
void Error(const std::string& text,bool fatal=false) { lastError = text; auto v=json::Value::MakeObject();v.Set("type",json::Value::String("player.error"));v.Set("error",json::Value::String(text));v.Set("fatal",json::Value::Bool(fatal));Emit(v); }
int Command(std::initializer_list<const char*> args) { std::vector<const char*> list(args); list.push_back(nullptr); return api.command(player, list.data()); }
json::Value Node(const mpv_node& n) {
  if (n.format == MPV_FORMAT_STRING) return json::Value::String(n.u.string ? n.u.string : "");
  if (n.format == MPV_FORMAT_FLAG) return json::Value::Bool(n.u.flag != 0);
  if (n.format == MPV_FORMAT_INT64) return json::Value::Number(static_cast<double>(n.u.int64));
  if (n.format == MPV_FORMAT_DOUBLE) return json::Value::Number(n.u.double_);
  if ((n.format == MPV_FORMAT_NODE_ARRAY || n.format == MPV_FORMAT_NODE_MAP) && n.u.list) {
    auto value = n.format == MPV_FORMAT_NODE_ARRAY ? json::Value::MakeArray() : json::Value::MakeObject();
    for (int i=0;i<n.u.list->num;++i) {
      if (n.format == MPV_FORMAT_NODE_ARRAY) value.Push(Node(n.u.list->values[i]));
      else value.Set(n.u.list->keys[i], Node(n.u.list->values[i]));
    }
    return value;
  }
  return {};
}
json::Value Get(const char* property) { mpv_node n{}; if (api.get_property(player, property, MPV_FORMAT_NODE, &n)<0) return {}; auto value=Node(n);api.free_node_contents(&n);return value; }
double Number(const char* property) { return Get(property).AsNumber(); }
void Status() {
  auto out=json::Value::MakeObject();out.Set("type",json::Value::String("player.status"));
  for(const char* key:{"time-pos","duration","pause","speed","volume","mute","aid","sid","secondary-sid","audio-delay","sub-delay","video-params","video-codec","audio-codec-name","hwdec-current","decoder-frame-drop-count","frame-drop-count","demuxer-cache-duration","track-list","eof-reached","deinterlace","chapter","chapter-list"}) out.Set(key,Get(key));
  out.Set("title",json::Value::String(loadedTitle));out.Set("enhanced",json::Value::Bool(enhanced)); Emit(out);
}
bool ValidNetwork(const std::string& url) {
  if(url.size()>=16384||!(url.starts_with("https://")||url.starts_with("http://"))||url.find_first_of("\r\n")!=std::string::npos)return false;
  const auto start=url.find("://")+3,end=url.find_first_of("/?#",start);
  return url.substr(start,end-start).find('@')==std::string::npos;
}
void SaveResume(bool force=false){
  if(resumeFile.empty()||resumeKey.empty())return;
  const double position=Number("time-pos"),duration=Number("duration");if(duration<=0||(!force&&std::abs(position-resumePosition)<1))return;
  auto item=json::Value::MakeObject();item.Set("position",json::Value::Number(position>=duration-5?0:position));
  item.Set("at",json::Value::Number(static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count())));
  resumes.Set(resumeKey,std::move(item));if(resumes.Members().size()>500)resumes.MutableMembers().erase(resumes.MutableMembers().begin());
  WriteFileAtomic(resumeFile,json::Serialize(resumes));resumePosition=position;resumeSaved=GetTickCount64();
}
std::string ResumeHash(const std::string& value){
  unsigned char bytes[32]{};if(BCryptHash(BCRYPT_SHA256_ALG_HANDLE,nullptr,0,reinterpret_cast<PUCHAR>(const_cast<char*>(value.data())),static_cast<ULONG>(value.size()),bytes,sizeof(bytes))<0)return {};
  std::string key;const char* hex="0123456789abcdef";for(auto b:bytes){key+=hex[b>>4];key+=hex[b&15];}return key;
}
void Load(const std::string& url, double position, const std::string& title, bool local) {
  if ((!local && !ValidNetwork(url)) || url.find('\0')!=std::string::npos) { Error("不支持的媒体地址");return; }
  SaveResume(true);resumeKey=ResumeHash(url);resumePosition=-1;
  loadedTitle=title;pendingStart=position>=0?position:resumes[resumeKey]["position"].AsNumber();outroSent=false;lastError.clear();watching=true;progressAt=GetTickCount64();progressPosition=-1;
  if (Command({"loadfile",url.c_str(),"replace"})<0)Error("无法打开媒体，请返回网页重试或选择其他线路",true);
  else expectedEntry=static_cast<int64_t>(Number("playlist/0/id"));
}
void Picker(bool subtitle) {
  wchar_t file[32768]{};OPENFILENAMEW dialog{sizeof(dialog)};dialog.hwndOwner=GetAncestor(owner,GA_ROOT);dialog.lpstrFile=file;dialog.nMaxFile=ARRAYSIZE(file);
  dialog.lpstrTitle=subtitle?L"选择外挂字幕":L"选择本地视频";
  dialog.lpstrFilter=subtitle?L"字幕\0*.ass;*.ssa;*.srt;*.vtt;*.sub\0所有文件\0*.*\0":L"媒体文件\0*.mp4;*.mkv;*.webm;*.avi;*.mov;*.ts;*.m2ts;*.flv;*.mp3;*.flac;*.wav;*.m3u8\0所有文件\0*.*\0";
  dialog.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;
  if(GetOpenFileNameW(&dialog)){
    const auto path=ToUtf8(file);if(subtitle)Command({"sub-add",path.c_str(),"select"});else Load(path,-1,ToUtf8(file),true);
  }
}
void Handle(const std::string& text) {
  json::Value c;if(!json::Parse(text,c))return;const auto type=c["op"].AsString();
  if(type=="init"){
    if(resumeFile.empty()){const auto profile=FromUtf8(c["profile"].AsString());if(!profile.empty()){resumeFile=profile+L"\\PlayerResume.json";std::string data;json::Value saved;if(ReadFileBytes(resumeFile,data)&&json::Parse(data,saved)&&saved.IsObject()&&saved.Members().size()<=500)resumes=std::move(saved);}}
  }else if(type=="load"){
    skipIntro=std::clamp(c["intro"].AsNumber(),0.0,600.0);skipOutro=std::clamp(c["outro"].AsNumber(),0.0,600.0);
    Load(c["url"].AsString(),c["start"].AsNumber(-1),c["title"].AsString(),false);
  } else if(type=="pause")Command({"cycle","pause"});
  else if(type=="seek"){progressAt=GetTickCount64();progressPosition=-1;const auto value=std::to_string(std::clamp(c["value"].AsNumber(),-86400.0,864000.0));Command({"seek",value.c_str(),c["absolute"].AsBool()?"absolute+exact":"relative+exact"});}
  else if(type=="set"){
    const auto key=c["key"].AsString(),value=c["value"].AsString();
    if(key=="speed"||key=="volume"||key=="audio-delay"||key=="sub-delay"){
      char* end=nullptr;const double n=strtod(value.c_str(),&end);if(!end||*end||!std::isfinite(n))return;
      if(key=="speed"&&(n<0.25||n>4))return;if(key=="volume"&&(n<0||n>150))return;if((key=="audio-delay"||key=="sub-delay")&&std::abs(n)>600)return;
    }else if(key=="sid"||key=="secondary-sid"||key=="aid"||key=="chapter"){
      if(value!="no"&&value!="auto"&&(value.empty()||value.find_first_not_of("0123456789")!=std::string::npos))return;
    }else if(key=="deinterlace"||key=="mute"){if(value!="yes"&&value!="no")return;}
    else if(key=="video-aspect-override"){if(value!="-1"&&value!="1.777778"&&value!="1.333333"&&value!="1")return;}
    else if(key=="hwdec"){if(value!="auto-safe"&&value!="no")return;}
    else return;
    if(api.set_property_string(player,key.c_str(),value.c_str())<0)Error("此设置不可用");
  }else if(type=="enhance"){
    const auto mode=c["value"].AsString();std::string shaders;
    if(mode=="anime4k"){
      for(const auto* file:{L"Anime4K_Clamp_Highlights.glsl",L"Anime4K_Restore_CNN_M.glsl",L"Anime4K_Upscale_CNN_x2_M.glsl"}){
        const auto path=directory+L"\\media\\shaders\\"+file;if(GetFileAttributesW(path.c_str())==INVALID_FILE_ATTRIBUTES){Error("超分组件缺失");return;}
        if(!shaders.empty())shaders+=';';shaders+=ToUtf8(path);
      }
    }else if(mode!="off")return;
    if(api.set_property_string(player,"glsl-shaders",shaders.c_str())<0)Error("超分着色器加载失败");else {enhanced=mode=="anime4k";qualityTick=GetTickCount64();qualityDrops=Number("frame-drop-count");}
  }else if(type=="pip"){
    if(mini)PostMessageW(mini,WM_CLOSE,0,0);
    else{WNDCLASSW wc{};wc.hInstance=GetModuleHandleW(nullptr);wc.lpfnWndProc=MiniProc;wc.lpszClassName=L"JisuMediaMini";wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);RegisterClassW(&wc);
      mini=CreateWindowExW(WS_EX_TOPMOST|WS_EX_TOOLWINDOW,wc.lpszClassName,L"极速播放器 · 空格暂停 / F 全屏 / Esc 退出",WS_OVERLAPPEDWINDOW,80,80,640,400,nullptr,nullptr,wc.hInstance,nullptr);
      if(mini){SetParent(surface,mini);ShowWindow(mini,SW_SHOWNOACTIVATE);}
    }
  }else if(type=="file")Picker(false);
  else if(type=="subtitle")Picker(true);
  else if(type=="screenshot"){
    wchar_t pictures[MAX_PATH];if(SUCCEEDED(SHGetFolderPathW(nullptr,CSIDL_MYPICTURES,nullptr,SHGFP_TYPE_CURRENT,pictures))){
      const auto path=ToUtf8(std::wstring(pictures)+L"\\jisu-"+std::to_wstring(GetTickCount64())+L".png");
      if(Command({"screenshot-to-file",path.c_str(),"subtitles"})<0)Error("截图失败");else Event("player.screenshot");
    }
  }else if(type=="frame")Command({"frame-step"});
  else if(type=="frame-back")Command({"frame-back-step"});
  else if(type=="stop"){watching=false;SaveResume(true);Command({"stop"});PostMessageW(surface,WM_CLOSE,0,0);}
  Status();
}
LRESULT CALLBACK Proc(HWND window,UINT message,WPARAM w,LPARAM l){
  if(message==kCommand){auto* text=reinterpret_cast<std::string*>(l);Handle(*text);delete text;return 0;}
  if(message==WM_LBUTTONDOWN){SetFocus(window);Command({"cycle","pause"});return 0;}
  if(message==WM_LBUTTONDBLCLK){if(mini){if(miniFullscreen.Active())miniFullscreen.Exit(mini);else miniFullscreen.Enter(mini);}else Event("player.fullscreen");return 0;}
  if(message==WM_KEYDOWN){
    const bool ctrl=GetKeyState(VK_CONTROL)<0,alt=GetKeyState(VK_MENU)<0,shift=GetKeyState(VK_SHIFT)<0;const char* shortcut=nullptr;
    if(ctrl&&!alt){if(w=='H')shortcut="history";else if(w=='L')shortcut="address";else if(w=='T')shortcut="new-tab";else if(w=='W')shortcut="close-tab";else if(w==VK_TAB)shortcut=shift?"prev-tab":"next-tab";else if(w=='R')shortcut="reload";else if(w=='D')shortcut="bookmark";else if(w=='M'&&shift)shortcut="media";}
    else if((alt&&w=='D')||w==VK_F6)shortcut="address";
    if(shortcut){if(!(l&(1LL<<30))){auto event=json::Value::MakeObject();event.Set("type",json::Value::String("player.shortcut"));event.Set("command",json::Value::String(shortcut));Emit(event);}return 0;}
    if(w==VK_SPACE)Command({"cycle","pause"});else if(w==VK_LEFT)Command({"seek","-10","relative"});else if(w==VK_RIGHT)Command({"seek","10","relative"});
    else if(w==VK_ESCAPE){if(mini&&miniFullscreen.Active())miniFullscreen.Exit(mini);else Event("player.escape");}else if(w=='F'||w==VK_F11){if(mini){if(miniFullscreen.Active())miniFullscreen.Exit(mini);else miniFullscreen.Enter(mini);}else Event("player.fullscreen");}else if(w==VK_OEM_PERIOD)Command({"frame-step"});return 0;
  }
  if(message==WM_TIMER){
    RECT r{};GetClientRect(mini?mini:owner,&r);SetWindowPos(window,nullptr,0,0,r.right,r.bottom,SWP_NOACTIVATE|SWP_NOZORDER);
    for(int i=0;i<100;++i){auto* e=api.wait_event(player,0);if(!e||e->event_id==MPV_EVENT_NONE)break;
      if(e->event_id==MPV_EVENT_START_FILE){auto* start=static_cast<mpv_event_start_file*>(e->data);if(start)eventEntry=start->playlist_entry_id;}
      if(e->event_id==MPV_EVENT_FILE_LOADED&&eventEntry==expectedEntry){if(pendingStart>=Number("duration")-5)pendingStart=0;const auto seek=std::to_string((std::max)(pendingStart,skipIntro));Command({"seek",seek.c_str(),"absolute+exact"});api.set_property_string(player,"pause","no");Event("player.loaded");}
      if(e->event_id==MPV_EVENT_END_FILE){auto* end=static_cast<mpv_event_end_file*>(e->data);if(end&&end->playlist_entry_id!=expectedEntry)continue;if(end&&end->reason==MPV_END_FILE_REASON_ERROR)Error(std::string("播放失败：")+api.error_string(end->error),true);else if(end&&end->reason==MPV_END_FILE_REASON_EOF){outroSent=true;Event("player.ended");}}
    }
    const double duration=Number("duration"),position=Number("time-pos");
    if(!outroSent&&skipOutro>0&&duration>skipOutro+60&&position>=duration-skipOutro){outroSent=true;Event("player.ended");}
    const bool paused=Get("pause").AsBool();
    if(paused||std::abs(position-progressPosition)>0.1){progressPosition=position;progressAt=GetTickCount64();}
    if(watching&&!paused&&!outroSent&&GetTickCount64()-progressAt>20000){watching=false;Error("片源连续 20 秒未推进，请重试或切换线路",true);Command({"stop"});}
    if(!outroSent&&Get("eof-reached").AsBool()){outroSent=true;Event("player.ended");}
    if(enhanced&&GetTickCount64()-qualityTick>3000){const double dropped=Number("frame-drop-count");if(dropped-qualityDrops>=12&&!Get("pause").AsBool()){api.set_property_string(player,"glsl-shaders","");enhanced=false;Event("player.enhancement-reduced");}qualityDrops=dropped;qualityTick=GetTickCount64();}
    if(GetTickCount64()-resumeSaved>2000)SaveResume();
    Status();return 0;
  }
  if(message==WM_CLOSE){DestroyWindow(window);return 0;}
  if(message==WM_DESTROY){PostQuitMessage(0);return 0;}
  return DefWindowProcW(window,message,w,l);
}
int Selftest(){
  player=api.create();if(!player)return 2;
  api.set_option_string(player,"config","no");api.set_option_string(player,"vo","null");api.set_option_string(player,"ao","null");api.set_option_string(player,"idle","yes");api.set_option_string(player,"load-scripts","no");api.set_option_string(player,"terminal","no");
  if(api.initialize(player)<0)return 3;
  const auto decoders=json::Serialize(Get("decoder-list"));
  for(const char* codec:{"h264","hevc","av1","vp9","flac","aac"})if(decoders.find(codec)==std::string::npos){Error(std::string("missing codec: ")+codec);return 4;}
  wchar_t temp[MAX_PATH];GetTempPathW(MAX_PATH,temp);auto sample=std::wstring(temp)+L"jisu-media-test-"+std::to_wstring(GetCurrentProcessId())+L".y4m";
  std::string bytes="YUV4MPEG2 W16 H16 F25:1 Ip A1:1 C420jpeg\n";for(int i=0;i<25;++i){bytes+="FRAME\n";bytes+=std::string(256,static_cast<char>(40+i));bytes+=std::string(128,static_cast<char>(128));}
  if(!WriteFileAtomic(sample,bytes))return 5;
  auto path=ToUtf8(sample);Command({"loadfile",path.c_str()});bool loaded=false,decoded=false;const auto deadline=GetTickCount64()+10000;
  while(GetTickCount64()<deadline){auto* e=api.wait_event(player,0.1);if(e->event_id==MPV_EVENT_FILE_LOADED)loaded=true;if(e->event_id==MPV_EVENT_END_FILE){auto* end=static_cast<mpv_event_end_file*>(e->data);decoded=end&&end->reason==MPV_END_FILE_REASON_EOF;break;}}
  api.terminate_destroy(player);player=nullptr;DeleteFileW(sample.c_str());
  auto result=json::Value::MakeObject();result.Set("type",json::Value::String("player.selftest"));result.Set("codecs",json::Value::Bool(true));result.Set("loaded",json::Value::Bool(loaded));result.Set("decoded",json::Value::Bool(decoded));Emit(result);return loaded&&decoded?0:6;
}
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int){
  wchar_t exe[MAX_PATH];GetModuleFileNameW(nullptr,exe,MAX_PATH);directory=ParentDirectory(exe);
  if(!api.Load(directory+L"\\media\\libmpv-2.dll")){Error("播放器组件不可用，请重新安装完整运行包");return 1;}
  int count=0;auto args=CommandLineToArgvW(GetCommandLineW(),&count);
  if(count==2&&std::wstring(args[1])==L"--selftest"){LocalFree(args);return Selftest();}
  if(count!=2){LocalFree(args);return 2;}owner=reinterpret_cast<HWND>(_wcstoui64(args[1],nullptr,10));LocalFree(args);
  DWORD parentPid=0;GetWindowThreadProcessId(owner,&parentPid);if(!IsWindow(owner)||!parentPid)return 2;
  CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
  WNDCLASSW wc{};wc.hInstance=instance;wc.lpfnWndProc=Proc;wc.lpszClassName=L"JisuMediaSurface";wc.style=CS_DBLCLKS;wc.hbrBackground=static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);RegisterClassW(&wc);
  RECT bounds{};GetClientRect(owner,&bounds);surface=CreateWindowExW(WS_EX_NOPARENTNOTIFY,wc.lpszClassName,L"",WS_CHILD|WS_VISIBLE|WS_CLIPCHILDREN,0,0,bounds.right,bounds.bottom,owner,nullptr,instance,nullptr);
  player=api.create();if(!player)return 3;
  const auto wid=std::to_string(reinterpret_cast<uintptr_t>(surface));
  for(const auto& pair:std::initializer_list<std::pair<const char*,const char*>>{{"config","no"},{"load-scripts","no"},{"ytdl","no"},{"terminal","no"},{"idle","yes"},{"keep-open","yes"},{"vo","gpu"},{"gpu-api","d3d11"},{"gpu-context","d3d11"},{"hwdec","auto-safe"},{"input-default-bindings","no"},{"input-builtin-bindings","no"},{"input-vo-keyboard","no"},{"osc","no"},{"osd-level","0"},{"cache","yes"},{"demuxer-max-bytes","64MiB"},{"demuxer-max-back-bytes","16MiB"},{"network-timeout","20"},{"screenshot-format","png"},{"tone-mapping","bt.2390"}})api.set_option_string(player,pair.first,pair.second);
  api.set_option_string(player,"wid",wid.c_str());
  if(api.initialize(player)<0){Error("播放器初始化失败");api.terminate_destroy(player);return 4;}
  reader=std::thread([]{std::string pending;char buffer[4096];DWORD n;while(!quitting&&ReadFile(GetStdHandle(STD_INPUT_HANDLE),buffer,sizeof(buffer),&n,nullptr)&&n){pending.append(buffer,n);if(pending.size()>65536)break;size_t p;while((p=pending.find('\n'))!=std::string::npos){auto* text=new std::string(pending.substr(0,p));pending.erase(0,p+1);if(!PostMessageW(surface,kCommand,0,reinterpret_cast<LPARAM>(text)))delete text;}}PostMessageW(surface,WM_CLOSE,0,0);});
  SetTimer(surface,1,500,nullptr);Event("player.ready");
  MSG message{};while(GetMessageW(&message,nullptr,0,0)>0){if(message.message==WM_KEYDOWN){Proc(surface,message.message,message.wParam,message.lParam);continue;}if(message.message==WM_LBUTTONDBLCLK){Proc(surface,message.message,message.wParam,message.lParam);continue;}TranslateMessage(&message);DispatchMessageW(&message);}
  if(mini)DestroyWindow(mini);
  SaveResume(true);quitting=true;CancelSynchronousIo(reader.native_handle());reader.join();api.terminate_destroy(player);CoUninitialize();return 0;
}
