#include "browser_window.h"
#include "browser_url.h"
#include "json.h"
#include <wincrypt.h>
#include <cryptuiapi.h>
using Microsoft::WRL::Callback;
namespace jisu {
void BrowserWindow::ConfigureSecurity(Tab& tab){
  if(options_.appMode||tab.agent||tab.securityConfigured||!tab.webview)return;tab.securityConfigured=true;const auto owner=tab.owner;const auto id=tab.id;EventRegistrationToken token{};
  if(SUCCEEDED(tab.webview->GetDevToolsProtocolEventReceiver(L"Security.visibleSecurityStateChanged",tab.securityEvents.GetAddressOf())))
    tab.securityEvents->add_DevToolsProtocolEventReceived(Callback<ICoreWebView2DevToolsProtocolEventReceivedEventHandler>(
      [owner,id](ICoreWebView2*,ICoreWebView2DevToolsProtocolEventReceivedEventArgs* args)->HRESULT{
        auto* self=*owner;auto* t=self&&!self->destroyed_?self->FindTab(id):nullptr;if(!t)return S_OK;
        CoTaskString text;json::Value data;if(FAILED(args->get_ParameterObjectAsJson(text.Put()))||!json::Parse(ToUtf8(text.Str()),data))return S_OK;
        const auto state=data["visibleSecurityState"]["securityState"].AsString();
        t->securityState=t->certificateError?3:state=="secure"?1:state=="insecure"||state=="insecure-broken"?2:0;
        self->InvalidateChrome();return S_OK;
      }).Get(),&token);
  tab.webview->CallDevToolsProtocolMethod(L"Security.enable",L"{}",Callback<ICoreWebView2CallDevToolsProtocolMethodCompletedHandler>([](HRESULT,LPCWSTR)->HRESULT{return S_OK;}).Get());
  ComPtr<ICoreWebView2_14> errors;
  if(SUCCEEDED(tab.webview.As(&errors)))errors->add_ServerCertificateErrorDetected(
    Callback<ICoreWebView2ServerCertificateErrorDetectedEventHandler>([owner,id](ICoreWebView2*,ICoreWebView2ServerCertificateErrorDetectedEventArgs* args)->HRESULT{
      auto* self=*owner;auto* t=self&&!self->destroyed_?self->FindTab(id):nullptr;if(!t)return S_OK;CoTaskString uri;args->get_RequestUri(uri.Put());
      if(FaviconOrigin(uri.Str())==t->securityOrigin){t->certificateError=true;t->securityState=3;self->InvalidateChrome();}
      return S_OK; // Keep the runtime's certificate interstitial; never silently bypass it.
    }).Get(),&token);
}
void BrowserWindow::ShowSiteInfo(POINT screen){
  const auto* tab=ActiveTab();if(!tab)return;const auto id=tab->id;const auto origin=FaviconOrigin(tab->url);
  const bool internal=IsInternalPage(tab->url)||IsNewTabPageUrl(tab->url);
  const wchar_t* state=internal?L"浏览器内部页面":tab->certificateError?L"证书异常，请勿输入敏感信息":tab->securityState==1&&tab->url.starts_with(L"https://")?L"连接安全（内核已验证）":tab->url.starts_with(L"http://")?L"连接未加密（HTTP）":L"连接安全状态尚未确认";
  HMENU menu=CreatePopupMenu();AppendMenuW(menu,MF_STRING|MF_GRAYED,0,state);
  if(!origin.empty())AppendMenuW(menu,MF_STRING|MF_GRAYED,0,MenuText(origin).c_str());
  if(options_.inPrivate)AppendMenuW(menu,MF_STRING|MF_GRAYED,0,L"无痕：不保留浏览资料");
  AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
  AppendMenuW(menu,MF_STRING|(tab->url.starts_with(L"https://")&&!internal?0:MF_GRAYED),1,L"查看证书");
  AppendMenuW(menu,MF_STRING,2,L"站点权限");
  const auto action=TrackPopupMenuEx(menu,TPM_RETURNCMD|TPM_NONOTIFY,screen.x,screen.y,hwnd_,nullptr);DestroyMenu(menu);
  if(destroyed_||!FindTab(id))return;if(action==1)ShowCertificate(id);else if(action==2)OpenSettings();
}
void BrowserWindow::ShowCertificate(uint32_t id){
  const auto* tab=FindTab(id);if(!tab||!tab->webview||!IsActive(*tab))return;
  const auto origin=FaviconOrigin(tab->url);if(!origin.starts_with(L"https://"))return;
  auto request=json::Value::MakeObject();request.Set("origin",json::Value::String(ToUtf8(origin)));
  const auto owner=tab->owner;
  const HRESULT hr=tab->webview->CallDevToolsProtocolMethod(L"Network.getCertificate",FromUtf8(json::Serialize(request)).c_str(),
    Callback<ICoreWebView2CallDevToolsProtocolMethodCompletedHandler>([owner,id,origin](HRESULT result,LPCWSTR text)->HRESULT{
      auto* self=*owner;const auto* t=self&&!self->destroyed_?self->FindTab(id):nullptr;
      if(!t||!self->IsActive(*t)||FaviconOrigin(t->url)!=origin||GetForegroundWindow()!=self->hwnd_)return S_OK;
      json::Value data;std::string encoded;
      if(SUCCEEDED(result)&&text&&json::Parse(ToUtf8(text),data)&&!data["tableNames"].Items().empty())encoded=data["tableNames"].Items()[0].AsString();
      DWORD length=0;std::vector<BYTE> bytes;
      if(!encoded.empty()&&encoded.size()<=1024*1024&&CryptStringToBinaryA(encoded.c_str(),static_cast<DWORD>(encoded.size()),CRYPT_STRING_BASE64,nullptr,&length,nullptr,nullptr)){
        bytes.resize(length);if(!CryptStringToBinaryA(encoded.c_str(),static_cast<DWORD>(encoded.size()),CRYPT_STRING_BASE64,bytes.data(),&length,nullptr,nullptr))bytes.clear();
      }
      self->PostTask([owner,id,origin,bytes=std::move(bytes)]{
        auto* current=*owner;const auto* page=current&&!current->destroyed_?current->FindTab(id):nullptr;
        if(!page||!current->IsActive(*page)||FaviconOrigin(page->url)!=origin||GetForegroundWindow()!=current->hwnd_)return;
        const auto certificate=bytes.empty()?nullptr:CertCreateCertificateContext(X509_ASN_ENCODING|PKCS_7_ASN_ENCODING,bytes.data(),static_cast<DWORD>(bytes.size()));
        if(!certificate){current->ShowNotice(L"此页面未返回可查看的连接证书，请确认页面已加载。");return;}
        CRYPTUI_VIEWCERTIFICATE_STRUCTW view{};view.dwSize=sizeof(view);view.hwndParent=current->hwnd_;view.dwFlags=CRYPTUI_DISABLE_ADDTOSTORE|CRYPTUI_DISABLE_EDITPROPERTIES;view.pCertContext=certificate;
        BOOL changed=FALSE;CryptUIDlgViewCertificateW(&view,&changed);CertFreeCertificateContext(certificate);
      });return S_OK;
    }).Get());
  if(FAILED(hr)&&GetForegroundWindow()==hwnd_)ShowNotice(L"当前内核无法读取连接证书。");
}
}  // namespace jisu
