#include "browser_window.h"
#include "browser_url.h"
#include "json.h"
#include "translation_script.h"
#include <winhttp.h>
#include <thread>
using Microsoft::WRL::Callback;
namespace jisu {
namespace {
std::wstring Encode(const std::string& text) {
  constexpr wchar_t hex[]=L"0123456789ABCDEF";std::wstring out;
  for(unsigned char c:text){if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'||c=='~')out+=c;
    else{out+=L'%';out+=hex[c>>4];out+=hex[c&15];}}
  return out;
}
// Fixed provider only; no website cookies, Authorization, redirects, generic fetch or URL logging.
std::string Google(const std::string& query,const std::wstring& proxy) {
  HINTERNET session=WinHttpOpen(L"JisuBrowser/Translate",proxy.empty()?WINHTTP_ACCESS_TYPE_DEFAULT_PROXY:WINHTTP_ACCESS_TYPE_NAMED_PROXY,
      proxy.empty()?WINHTTP_NO_PROXY_NAME:proxy.c_str(),WINHTTP_NO_PROXY_BYPASS,0);
  if(!session)return {};
  WinHttpSetTimeouts(session,10000,10000,15000,15000);
  HINTERNET connection=WinHttpConnect(session,L"translate.googleapis.com",443,0);
  const auto object=L"/translate_a/single?client=gtx&sl=auto&tl=zh-CN&dt=t&q="+Encode(query);
  HINTERNET request=connection?WinHttpOpenRequest(connection,L"GET",object.c_str(),nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE):nullptr;
  std::string body;
  if(request){
    DWORD disabled=WINHTTP_DISABLE_COOKIES|WINHTTP_DISABLE_REDIRECTS|WINHTTP_DISABLE_AUTHENTICATION;
    WinHttpSetOption(request,WINHTTP_OPTION_DISABLE_FEATURE,&disabled,sizeof(disabled));
    if(WinHttpSendRequest(request,WINHTTP_NO_ADDITIONAL_HEADERS,0,WINHTTP_NO_REQUEST_DATA,0,0,0)&&WinHttpReceiveResponse(request,nullptr)){
      DWORD status=0,size=sizeof(status);WinHttpQueryHeaders(request,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,WINHTTP_HEADER_NAME_BY_INDEX,&status,&size,WINHTTP_NO_HEADER_INDEX);
      if(status==200){char buffer[8192];DWORD read=0;bool ok=true;
        while((ok=WinHttpReadData(request,buffer,sizeof(buffer),&read))&&read){body.append(buffer,read);if(body.size()>262144){ok=false;break;}}
        if(!ok)body.clear();
      }
    }
    WinHttpCloseHandle(request);
  }
  if(connection)WinHttpCloseHandle(connection);WinHttpCloseHandle(session);return body;
}
std::string Translate(const json::Value& texts,const std::wstring& proxy) {
  const std::string separator="\n\xee\x80\x80\n";std::string query;
  for(const auto& text:texts.Items()){if(!query.empty())query+=separator;query+=text.AsString();}
  json::Value response;if(!json::Parse(Google(query,proxy),response)||response.Items().empty())return {};
  std::string output;
  for(const auto& part:response.Items()[0].Items())if(!part.Items().empty())output+=part.Items()[0].AsString();
  auto values=json::Value::MakeArray();size_t begin=0;
  for(;;){const auto end=output.find("\xee\x80\x80",begin);values.Push(json::Value::String(output.substr(begin,end==std::string::npos?end:end-begin)));
    if(end==std::string::npos)break;begin=end+3;}
  if(values.Items().size()!=texts.Items().size())return {};
  return json::Serialize(values);
}
}
void BrowserWindow::AddTranslationContextMenu(Tab& tab, ICoreWebView2ContextMenuItemCollection* items) {
  if(!items||options_.appMode||tab.agent||!IsActive(tab)||IsInternalPage(tab.url)||
      !(tab.url.starts_with(L"http://")||tab.url.starts_with(L"https://")))return;
  ComPtr<ICoreWebView2Environment9> environment;
  if(FAILED(environment_.As(&environment)))return;
  ComPtr<ICoreWebView2ContextMenuItem> item;
  if(FAILED(environment->CreateContextMenuItem(tab.translationEnabled?L"恢复原文":L"翻译成中文",nullptr,
      COREWEBVIEW2_CONTEXT_MENU_ITEM_KIND_COMMAND,item.GetAddressOf())))return;
  const auto owner=tab.owner;const auto id=tab.id;const auto url=tab.url;const auto generation=tab.translationGeneration;
  EventRegistrationToken selected{};
  if(FAILED(item->add_CustomItemSelected(Callback<ICoreWebView2CustomItemSelectedEventHandler>(
      [owner,id,url,generation](ICoreWebView2ContextMenuItem*,IUnknown*)->HRESULT{
    auto* self=*owner;if(!self||self->destroyed_)return S_OK;
    self->PostTask([owner,id,url,generation]{
      auto* self=*owner;if(!self||self->destroyed_)return;auto* tab=self->FindTab(id);
      // The menu may outlive a navigation, tab switch or translation toggle.
      if(!tab||tab->agent||!self->IsActive(*tab)||tab->url!=url||tab->translationGeneration!=generation)return;
      self->TogglePageTranslation();
    });return S_OK;
  }).Get(),&selected)))return;
  UINT32 count=0;if(FAILED(items->get_Count(&count)))return;
  ComPtr<ICoreWebView2ContextMenuItem> separator;
  if(count&&SUCCEEDED(environment->CreateContextMenuItem(L"",nullptr,COREWEBVIEW2_CONTEXT_MENU_ITEM_KIND_SEPARATOR,separator.GetAddressOf()))&&
      SUCCEEDED(items->InsertValueAtIndex(count,separator.Get())))++count;
  items->InsertValueAtIndex(count,item.Get());
}
void BrowserWindow::TogglePageTranslation() {
  Tab* tab=ActiveTab();if(!tab||!tab->webview)return;
  if(tab->translationEnabled){
    tab->translationEnabled=false;tab->translationBusy=false;++tab->translationGeneration;
    tab->webview->ExecuteScript(L"window.__jisuTranslation?.restore()",nullptr);return;
  }
  if(options_.appMode||tab->agent||IsInternalPage(tab->url)||!(tab->url.starts_with(L"http://")||tab->url.starts_with(L"https://"))){
    ShowNotice(L"请在普通 HTTP(S) 网页中使用翻译。");return;
  }
  tab->translationEnabled=true;tab->translationBusy=true;tab->translationFailed=false;tab->translationBatches=0;
  const auto generation=++tab->translationGeneration;const auto owner=tab->owner;const auto id=tab->id;
  tab->webview->ExecuteScript(FromUtf8(kTranslationScript).c_str(),Callback<ICoreWebView2ExecuteScriptCompletedHandler>([owner,id,generation](HRESULT hr,LPCWSTR)->HRESULT{
    auto* self=*owner;if(!self||self->destroyed_)return S_OK;auto* current=self->FindTab(id);
    if(!current||current->translationGeneration!=generation)return S_OK;
    current->translationBusy=false;if(FAILED(hr)){current->translationFailed=true;self->ShowNotice(L"翻译未能启动，请刷新后重试。");}
    else self->ContinuePageTranslation(*current);return S_OK;
  }).Get());
}
void BrowserWindow::ContinuePageTranslation(Tab& tab) {
  if(!tab.translationEnabled||tab.translationBusy||tab.translationFailed||!tab.webview||!IsActive(tab)||tab.loading)return;
  if(tab.translationBatches>=500){tab.translationFailed=true;tab.webview->ExecuteScript(L"window.__jisuTranslation?.error('页面过长，请恢复原文后分段翻译')",nullptr);return;}
  tab.translationBusy=true;const auto owner=tab.owner;const auto id=tab.id;const auto generation=tab.translationGeneration;
  tab.webview->ExecuteScript(L"window.__jisuTranslation?.next()",Callback<ICoreWebView2ExecuteScriptCompletedHandler>([owner,id,generation](HRESULT hr,LPCWSTR text)->HRESULT{
    auto* self=*owner;if(!self||self->destroyed_)return S_OK;auto* tab=self->FindTab(id);
    if(!tab||tab->translationGeneration!=generation)return S_OK;
    json::Value batch;
    if(FAILED(hr)||!text||!json::Parse(ToUtf8(text),batch)||batch["stopped"].AsBool()){tab->translationEnabled=false;tab->translationBusy=false;return S_OK;}
    const auto texts=batch["texts"];
    if(texts.Items().empty()){tab->translationBusy=false;return S_OK;}
    size_t bytes=0;for(const auto& t:texts.Items())bytes+=t.AsString().size();
    if(texts.Items().size()>20||bytes>16000){tab->translationBusy=false;tab->translationFailed=true;return S_OK;}
    ++tab->translationBatches;
    // Reuse this machine's existing browser relay, without modifying the proxy chain.
    std::wstring proxy;std::string status;json::Value route;
    if(ReadFileBytes(self->profileDir_+L"\\proxy-binding-status.json",status)&&json::Parse(status,route)){
      const auto listen=route["listen"].AsString();if(listen.starts_with("127.0.0.1:")&&listen.find_first_not_of("0123456789",10)==std::string::npos)proxy=FromUtf8(listen);
    }
    const auto alive=std::weak_ptr<int>(self->alive_);const auto target=self->hwnd_;
    std::thread([alive,target,owner,id,generation,texts,proxy]{
      Sleep(150);const auto result=Translate(texts,proxy);
      auto* task=new std::function<void()>([alive,owner,id,generation,result]{
        if(alive.expired())return;auto* self=*owner;if(!self||self->destroyed_)return;auto* tab=self->FindTab(id);
        if(!tab||tab->translationGeneration!=generation||!tab->translationEnabled||!tab->webview)return;
        if(result.empty()){tab->translationBusy=false;tab->translationFailed=true;tab->webview->ExecuteScript(L"window.__jisuTranslation?.error('Google 翻译暂不可用，请恢复原文后重试')",nullptr);return;}
        const auto script=L"window.__jisuTranslation?.accept("+FromUtf8(result)+L")";
        tab->webview->ExecuteScript(script.c_str(),Callback<ICoreWebView2ExecuteScriptCompletedHandler>([owner,id,generation](HRESULT hr,LPCWSTR)->HRESULT{
          auto* self=*owner;if(!self||self->destroyed_)return S_OK;auto* tab=self->FindTab(id);if(!tab||tab->translationGeneration!=generation)return S_OK;
          tab->translationBusy=false;if(FAILED(hr))tab->translationFailed=true;else self->ContinuePageTranslation(*tab);return S_OK;
        }).Get());
      });
      if(alive.expired()||!PostMessageW(target,WM_APP+6,0,reinterpret_cast<LPARAM>(task)))delete task;
    }).detach();return S_OK;
  }).Get());
}
} // namespace jisu
