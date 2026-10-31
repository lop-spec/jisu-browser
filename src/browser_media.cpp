#include "browser_window.h"
#include "json.h"
#include <algorithm>
#include <cmath>
using Microsoft::WRL::Callback;
namespace jisu {
namespace {
constexpr wchar_t kPlayerUrl[]=L"https://newtab.jisu.example/player.html";
constexpr UINT_PTR kMediaTimer=7;
std::wstring PlayerDirectory(){wchar_t path[MAX_PATH];GetModuleFileNameW(nullptr,path,MAX_PATH);return ParentDirectory(path);}
bool PlayerUrl(const std::wstring& url){return url==kPlayerUrl;}
}
void BrowserWindow::OpenMedia(){
  Tab* source=ActiveTab();if(source&&!PlayerUrl(source->url)&&!source->agent&&source->webview){CaptureMedia(*source);return;}
  NewTab(kPlayerUrl);
}
bool BrowserWindow::EnsureMedia(Tab& tab){
  if(tab.media)return tab.media->Running();
  tab.mediaSurface=CreateWindowExW(0,L"STATIC",L"",WS_CHILD|WS_CLIPCHILDREN|SS_BLACKRECT,0,0,0,0,hwnd_,nullptr,instance_,nullptr);
  tab.media=std::make_unique<MediaSession>();
  if(!tab.media->Start(tab.mediaSurface,PlayerDirectory(),profileDir_)){StopMedia(tab);PostToPage(tab,"{\"type\":\"player.error\",\"error\":\"播放器组件启动失败，请安装完整运行包\"}");return false;}
  SetTimer(hwnd_,kMediaTimer,250,nullptr);PlaceTab(tab);return true;
}
void BrowserWindow::CaptureMedia(Tab& source){
  const auto id=source.id;const auto url=source.url;const auto owner=source.owner;
  // Executed only in response to a user command. No all-page scanner or media hooks.
  source.webview->ExecuteScript(LR"JS((()=>{const videos=[...document.querySelectorAll('video,audio')].sort((a,b)=>b.clientWidth*b.clientHeight-a.clientWidth*a.clientHeight),v=videos[0];if(!v)return JSON.stringify({empty:true});if(v.mediaKeys)return JSON.stringify({error:'受保护的视频请继续使用网页播放器'});let lines=[];try{lines=JSON.parse(v.dataset.jisuLines||'[]');}catch{}return JSON.stringify({url:v.dataset.jisuSource||v.currentSrc||v.src,start:v.currentTime||0,title:v.dataset.jisuTitle||document.title,index:Number(v.dataset.jisuIndex),integration:v.dataset.jisuIntegration||'',intro:Number(v.dataset.jisuIntro)||0,outro:Number(v.dataset.jisuOutro)||0,auto:v.dataset.jisuAuto==='true',lines});})())JS",
    Callback<ICoreWebView2ExecuteScriptCompletedHandler>([owner,id,url](HRESULT hr,LPCWSTR result)->HRESULT{
      auto* self=*owner;if(!self||self->destroyed_)return S_OK;Tab* tab=self->FindTab(id);if(!tab||tab->url!=url)return S_OK;
      json::Value outer,offer;if(FAILED(hr)||!result||!json::Parse(ToUtf8(result),outer)||!json::Parse(outer.AsString(),offer))return S_OK;
      if(offer["empty"].AsBool()){self->NewTab(kPlayerUrl);return S_OK;}
      // 提示用对话框：原先写进内容区的文字被网页盖住看不见；也不在 WebView2 回调里弹模态框
      if(!offer["error"].AsString().empty()){self->ShowNotice(FromUtf8(offer["error"].AsString()));return S_OK;}
      auto media=offer["url"].AsString();if(!(media.starts_with("http://")||media.starts_with("https://"))){
        self->ShowNotice(L"这个网页使用临时 blob/MSE 视频流，未提供可交接的媒体地址。请继续网页播放；清番可直接使用极速播放器。");return S_OK;
      }
      self->StartMedia(id,json::Serialize(offer));return S_OK;
    }).Get());
}
void BrowserWindow::StartMedia(uint32_t sourceId,const std::string& text){
  json::Value offer;if(!json::Parse(text,offer))return;const auto url=offer["url"].AsString();
  if(url.size()>16384||url.find_first_of("\r\n")!=std::string::npos||!(url.starts_with("https://")||url.starts_with("http://")))return;
  Tab* source=FindTab(sourceId);if(!source||source->agent)return;
  Tab* target=FindTab(source->mediaTarget);
  if(!target||!PlayerUrl(target->url)){NewTab(kPlayerUrl);target=ActiveTab();source=FindTab(sourceId);if(!target||!source)return;source->mediaTarget=target->id;target->mediaSource=sourceId;target->mediaPage=ToUtf8(source->url);}
  if(auto* owner=*target->owner;owner&&owner!=this){owner->StartMedia(sourceId,text);return;}
  target->mediaIntegration=offer["integration"].AsString()=="qingfan";
  target->mediaAuto=offer["auto"].AsBool();target->mediaOffer=text;
  if(source->webview)source->webview->ExecuteScript(L"document.querySelectorAll('video,audio').forEach(v=>v.pause());window.dispatchEvent(new Event('jisu-native-start'));",nullptr);
  if(!EnsureMedia(*target))return;
  auto load=offer;load.Set("op",json::Value::String("load"));target->media->Send(json::Serialize(load));
  PostToPage(*target,"{\"type\":\"player.offer\",\"data\":"+text+"}");PlaceTab(*target);
}
void BrowserWindow::SourceMediaAction(Tab& tab,const std::string& action,int line){
  Tab* source=FindTab(tab.mediaSource);if(!source||!source->webview||!tab.mediaIntegration||ToUtf8(source->url)!=tab.mediaPage)return;
  if(action=="line"&&line>=0){const auto script=L"document.querySelector('[data-line=\""+std::to_wstring(line)+L"\"]')?.click();";source->webview->ExecuteScript(script.c_str(),nullptr);}
  else if(action=="next"||action=="prev"){const auto script=L"document.getElementById('"+FromUtf8(action)+L"')?.click();";source->webview->ExecuteScript(script.c_str(),nullptr);}
}
bool BrowserWindow::HandleMediaMessage(Tab& tab,const std::wstring& origin,const std::string& text){
  json::Value message;if(!json::Parse(text,message))return false;const auto type=message["type"].AsString();
  if(type=="media.request"){
    if(tab.agent||!IsActive(tab)||origin!=tab.url||!tab.webview)return true;
    const auto id=tab.id;const auto page=tab.url;const auto owner=tab.owner;
    tab.webview->ExecuteScript(L"navigator.userActivation.isActive",Callback<ICoreWebView2ExecuteScriptCompletedHandler>([owner,id,page](HRESULT hr,LPCWSTR result)->HRESULT{auto* self=*owner;if(!self||self->destroyed_)return S_OK;json::Value active;if(SUCCEEDED(hr)&&result&&json::Parse(ToUtf8(result),active)&&active.AsBool()){auto* source=self->FindTab(id);if(source&&source->url==page&&self->IsActive(*source))self->CaptureMedia(*source);}return S_OK;}).Get());
    return true;
  }
  if(type=="media.offer"){
    Tab* target=FindTab(tab.mediaTarget);
    if(!tab.agent&&target&&target->media&&ToUtf8(tab.url)==target->mediaPage&&origin==tab.url)StartMedia(tab.id,text);
    return true;
  }
  if(!type.starts_with("player."))return false;
  if(!PlayerUrl(tab.url)||origin!=tab.url)return true;
  if(type=="player.get"){
    if(!tab.mediaOffer.empty())PostToPage(tab,"{\"type\":\"player.offer\",\"data\":"+tab.mediaOffer+"}");
    if(!tab.mediaStatus.empty())PostToPage(tab,tab.mediaStatus);return true;
  }
  // Agent-owned background pages are read-only with respect to native media/UI.
  if(tab.agent||!IsActive(tab))return true;
  if(type=="player.open"){
    const auto url=message["url"].AsString();if(!url.starts_with("https://")&&!url.starts_with("http://"))return true;
    if(!EnsureMedia(tab))return true;
    auto load=json::Value::MakeObject();load.Set("op",json::Value::String("load"));load.Set("url",json::Value::String(url));load.Set("title",json::Value::String("极速播放器"));tab.media->Send(json::Serialize(load));return true;
  }
  if(type=="player.file"&&!EnsureMedia(tab))return true;
  if(type=="player.fullscreen"){browserFullscreen_=!fullscreen_.Active();if(!browserFullscreen_)ExitFullscreen();else UpdateFullscreen();PlaceTab(tab);return true;}
  if(type=="player.next"||type=="player.prev")SourceMediaAction(tab,type.substr(7));
  else if(type=="player.line")SourceMediaAction(tab,"line",static_cast<int>(message["index"].AsNumber(-1)));
  else if(type=="player.close"){StopMedia(tab);PostToPage(tab,"{\"type\":\"player.closed\"}");PlaceTab(tab);}
  else if(tab.media){
    const auto op=type.substr(7);
    if(op=="pause"||op=="seek"||op=="set"||op=="enhance"||op=="frame"||op=="frame-back"||op=="screenshot"||op=="subtitle"||op=="file"||op=="pip"){
      message.Set("op",json::Value::String(op));tab.media->Send(json::Serialize(message));
    }
  }
  return true;
}
void BrowserWindow::PollMedia(){
  bool any=false;for(auto& ptr:tabs_){auto& tab=*ptr;if(!tab.media)continue;any=true;
    for(auto& text:tab.media->Poll()){
      json::Value event;if(!json::Parse(text,event))continue;const auto type=event["type"].AsString();
      if(type=="player.status"){
        tab.mediaStatus=text;
        if(GetTickCount64()-tab.mediaSaved>5000){tab.mediaSaved=GetTickCount64();const auto time=event["time-pos"].AsNumber();
          if(auto* source=FindTab(tab.mediaSource);source&&source->webview&&tab.mediaIntegration&&ToUtf8(source->url)==tab.mediaPage){
            const auto script=L"window.dispatchEvent(new CustomEvent('jisu-native-progress',{detail:"+std::to_wstring(time)+L"}));";source->webview->ExecuteScript(script.c_str(),nullptr);
          }
        }
      }else if(type=="player.shortcut"&&IsActive(tab))PostMediaShortcut(event["command"].AsString());
      else if(type=="player.fullscreen"&&IsActive(tab)){if(fullscreen_.Active())ExitFullscreen();else{browserFullscreen_=true;UpdateFullscreen();}PlaceTab(tab);}
      else if(type=="player.escape"&&IsActive(tab)){ExitFullscreen();PlaceTab(tab);}
      else if(type=="player.ended"&&tab.mediaAuto)SourceMediaAction(tab,"next");
      else if(type=="player.error"&&event["fatal"].AsBool()&&tab.mediaIntegration){if(auto* source=FindTab(tab.mediaSource);source&&source->webview&&ToUtf8(source->url)==tab.mediaPage)source->webview->ExecuteScript(L"window.dispatchEvent(new Event('jisu-native-error'));",nullptr);}
      else if(type=="player.stopped"){PostToPage(tab,"{\"type\":\"player.error\",\"error\":\"媒体进程已退出，可重新打开视频\"}");StopMedia(tab);PlaceTab(tab);break;}
      if(IsActive(tab)||type!="player.status")PostToPage(tab,text);
    }
  }
  if(!any)KillTimer(hwnd_,kMediaTimer);
}
void BrowserWindow::StopMedia(Tab& tab){
  if(!destroyed_&&tab.media&&IsActive(tab)&&fullscreen_.Active())ExitFullscreen();
  if(auto* source=FindTab(tab.mediaSource);source&&source->mediaTarget==tab.id){source->mediaTarget=0;if(source->webview)source->webview->ExecuteScript(L"window.dispatchEvent(new Event('jisu-native-stop'));",nullptr);}
  tab.media.reset();if(tab.mediaSurface){DestroyWindow(tab.mediaSurface);tab.mediaSurface=nullptr;}
  tab.mediaSource=0;tab.mediaPage.clear();tab.mediaOffer.clear();tab.mediaStatus.clear();
}
}  // namespace jisu
