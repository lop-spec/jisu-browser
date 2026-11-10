#include "browser_window.h"
#include "browser_url.h"
#include "json.h"
#include <cmath>
using Microsoft::WRL::Callback;
namespace jisu {
bool BrowserWindow::HandleLinkInput(uint32_t id,const std::wstring& text) {
  if(text.size()>256||!text.starts_with(L"{\"type\":\"jisu.input\""))return false;
  auto* tab=FindTab(id);if(!tab||tab->agent)return true;
  json::Value value;if(!json::Parse(ToUtf8(text),value)||value["type"].AsString()!="jisu.input")return true;
  const auto button=value["button"].AsNumber(-1);if(!std::isfinite(button)||button<0||button>2||std::floor(button)!=button)return true;
  // Presentation only. This never grants popup, file, permission or native UI authority.
  tab->gesture={GetTickCount64(),static_cast<int>(button),value["ctrl"].AsBool(),value["shift"].AsBool()};return true;
}
void BrowserWindow::ObserveInputFrame(uint32_t id,ICoreWebView2Frame* frame) {
  auto* tab=FindTab(id);if(!tab||!frame)return;const auto owner=tab->owner;EventRegistrationToken token{};
  ComPtr<ICoreWebView2Frame2> messages;
  if(SUCCEEDED(frame->QueryInterface(IID_PPV_ARGS(messages.GetAddressOf()))))messages->add_WebMessageReceived(
    Callback<ICoreWebView2FrameWebMessageReceivedEventHandler>([owner,id](ICoreWebView2Frame*,ICoreWebView2WebMessageReceivedEventArgs* args)->HRESULT{
      auto* self=*owner;CoTaskString text;if(self&&!self->destroyed_&&SUCCEEDED(args->TryGetWebMessageAsString(text.Put())))self->HandleLinkInput(id,text.Str());return S_OK;
    }).Get(),&token);
  ComPtr<ICoreWebView2Frame7> children;
  if(SUCCEEDED(frame->QueryInterface(IID_PPV_ARGS(children.GetAddressOf()))))children->add_FrameCreated(
    Callback<ICoreWebView2FrameChildFrameCreatedEventHandler>([owner,id](ICoreWebView2Frame*,ICoreWebView2FrameCreatedEventArgs* args)->HRESULT{
      auto* self=*owner;ComPtr<ICoreWebView2Frame> child;if(self&&!self->destroyed_&&SUCCEEDED(args->get_Frame(child.GetAddressOf())))self->ObserveInputFrame(id,child.Get());return S_OK;
    }).Get(),&token);
}
void BrowserWindow::ConfigureLinkInput(Tab& tab,bool firstNavigation) {
  if(options_.appMode){if(firstNavigation)FinishControllerNavigation(tab.id);return;}
  const auto owner=tab.owner;const auto id=tab.id;const ComPtr<ICoreWebView2> expected=tab.webview;
  if(!tab.inputConfigured&&!tab.agent&&!options_.appMode){
    tab.inputConfigured=true;EventRegistrationToken token{};
    ComPtr<ICoreWebView2_4> frames;
    if(SUCCEEDED(tab.webview.As(&frames)))frames->add_FrameCreated(Callback<ICoreWebView2FrameCreatedEventHandler>(
      [owner,id](ICoreWebView2*,ICoreWebView2FrameCreatedEventArgs* args)->HRESULT{
        auto* self=*owner;ComPtr<ICoreWebView2Frame> frame;if(self&&!self->destroyed_&&SUCCEEDED(args->get_Frame(frame.GetAddressOf())))self->ObserveInputFrame(id,frame.Get());return S_OK;
      }).Get(),&token);
    ComPtr<ICoreWebView2_11> menu;
    if(SUCCEEDED(tab.webview.As(&menu)))menu->add_ContextMenuRequested(Callback<ICoreWebView2ContextMenuRequestedEventHandler>(
      [owner,id](ICoreWebView2*,ICoreWebView2ContextMenuRequestedEventArgs* args)->HRESULT{
        auto* self=*owner;if(!self||self->destroyed_)return S_OK;
        ComPtr<ICoreWebView2ContextMenuTarget> target;ComPtr<ICoreWebView2ContextMenuItemCollection> items;CoTaskString link;
        if(FAILED(args->get_ContextMenuTarget(target.GetAddressOf()))||FAILED(target->get_LinkUri(link.Put()))||!IsWebUrl(link.Str())||FAILED(args->get_MenuItems(items.GetAddressOf())))return S_OK;
        ComPtr<ICoreWebView2Environment9> environment;if(FAILED(self->environment_.As(&environment)))return S_OK;
        UINT32 count=0;items->get_Count(&count);
        for(UINT32 i=0;i<count;++i){ComPtr<ICoreWebView2ContextMenuItem> item;CoTaskString name;
          if(FAILED(items->GetValueAtIndex(i,item.GetAddressOf()))||FAILED(item->get_Name(name.Put()))||name.Str()!=L"openLinkNewTab")continue;
          ComPtr<ICoreWebView2ContextMenuItem> replacement;
          if(FAILED(environment->CreateContextMenuItem(L"在新标签页中打开",nullptr,COREWEBVIEW2_CONTEXT_MENU_ITEM_KIND_COMMAND,replacement.GetAddressOf())))break;
          const auto url=link.Str();EventRegistrationToken selected{};
          replacement->add_CustomItemSelected(Callback<ICoreWebView2CustomItemSelectedEventHandler>([owner,id,url](ICoreWebView2ContextMenuItem*,IUnknown*)->HRESULT{
            auto* self=*owner;if(self&&!self->destroyed_)self->PostTask([owner,id,url]{auto* current=*owner;if(!current||current->destroyed_||!current->FindTab(id))return;current->NewTab(url,false,current->InsertIndexFor(id));});return S_OK;
          }).Get(),&selected);
          if(SUCCEEDED(items->RemoveValueAtIndex(i)))items->InsertValueAtIndex(i,replacement.Get());break;
        }return S_OK;
      }).Get(),&token);
  }
  // Await the final registration before navigating, including earlier agent guard scripts.
  const wchar_t* script=tab.agent||options_.appMode?L"void 0":kLinkGestureScript;
  const HRESULT hr=tab.webview->AddScriptToExecuteOnDocumentCreated(script,
    Callback<ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler>([owner,id,expected,firstNavigation](HRESULT result,LPCWSTR)->HRESULT{
      auto* self=*owner;auto* current=self&&!self->destroyed_?self->FindTab(id):nullptr;
      if(!current||current->webview.Get()!=expected.Get())return S_OK;
      if(firstNavigation)self->FinishControllerNavigation(id);
      else if(SUCCEEDED(result))current->webview->ExecuteScript(kLinkGestureScript,nullptr);
      return S_OK;
    }).Get());
  if(FAILED(hr)&&firstNavigation)FinishControllerNavigation(id);
}
}  // namespace jisu
