#include "browser_window.h"
#include "extension_toolbar.h"
#include "extension_popup_focus.h"
#include <dwmapi.h>
#include <algorithm>
#include <cmath>
using Microsoft::WRL::Callback;
namespace jisu {
LRESULT CALLBACK BrowserWindow::ExtensionPopupProc(HWND h,UINT message,WPARAM w,LPARAM l) {
  auto* self=reinterpret_cast<BrowserWindow*>(GetWindowLongPtrW(h,GWLP_USERDATA));
  if(message==WM_NCCREATE){const auto* c=reinterpret_cast<CREATESTRUCTW*>(l);self=static_cast<BrowserWindow*>(c->lpCreateParams);SetWindowLongPtrW(h,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));SetPropW(h,L"Jisu.PopupGeneration",reinterpret_cast<HANDLE>(static_cast<uintptr_t>(self->extensionPopupGeneration_)));}
  if(self){
    const auto generation=reinterpret_cast<uintptr_t>(GetPropW(h,L"Jisu.PopupGeneration"));
    if(message==WM_CLOSE){PostMessageW(self->hwnd_,kMsgCloseExtensionPopup,static_cast<WPARAM>(generation),0);return 0;}
    // Returning to the owner is handled by the actual toolbar/page click. Closing here
    // before its BN_CLICKED would turn a second click into close-then-reopen.
    if(message==WM_ACTIVATE&&LOWORD(w)==WA_INACTIVE&&ShouldCloseExtensionPopupOnDeactivate(self->hwnd_,h,reinterpret_cast<HWND>(l))){PostMessageW(self->hwnd_,kMsgCloseExtensionPopup,static_cast<WPARAM>(generation),0);}
    if(message==WM_NCDESTROY)RemovePropW(h,L"Jisu.PopupGeneration");
    if(message==WM_SIZE&&self->extensionPopup_&&self->extensionPopup_->controller){RECT r{};GetClientRect(h,&r);self->extensionPopup_->controller->put_Bounds(r);}
    if(message==WM_ERASEBKGND){RECT r{};GetClientRect(h,&r);HBRUSH brush=CreateSolidBrush(ToColorRef(self->palette_.content));FillRect(reinterpret_cast<HDC>(w),&r,brush);DeleteObject(brush);return 1;}
  }
  return DefWindowProcW(h,message,w,l);
}
void BrowserWindow::CloseExtensionPopup() {
  // 没有弹层就什么也不做：切标签、改窗口大小、点工具栏都会调到这里，不再每次写状态文件
  if (!extensionPopupWindow_ && !extensionPopup_) return;
  ++extensionPopupGeneration_;
  extensionPopupLoaded_=false;
  if(extensionPopup_&&extensionPopup_->controller)extensionPopup_->controller->Close();
  extensionPopup_.reset();popupExtensionIndex_=static_cast<size_t>(-1);
  HWND window=extensionPopupWindow_;extensionPopupWindow_=nullptr;
  if(window)DestroyWindow(window);
  WriteExtensionToolbarState();
}
void BrowserWindow::ShowExtensionPopup(size_t index) {
  if(index>=extensions_.size()||!environment_)return;
  auto& e=extensions_[index];if(!e.enabled||FAILED(e.result)||e.id.empty())return;
  if(extensionPopupWindow_&&popupExtensionIndex_==index){CloseExtensionPopup();return;}
  CloseExtensionPopup();
  auto page=e.popup;while(!page.empty()&&page.front()==L'/')page.erase(page.begin());
  if(page.empty()){HandleExtensionCommand(kExtensionSettingsBase+static_cast<UINT>(index));return;}
  if(page.find(L':')!=std::wstring::npos||page.find(L"..")!=std::wstring::npos)return;
  WNDCLASSEXW wc{sizeof(wc)};wc.hInstance=instance_;wc.lpfnWndProc=ExtensionPopupProc;wc.lpszClassName=L"JisuExtensionPopup";wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);
  if(!RegisterClassExW(&wc)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)return;
  HWND anchor=e.button&&IsWindowVisible(e.button)?e.button:extensionsButton_;
  RECT button{},main{};GetWindowRect(anchor,&button);GetWindowRect(hwnd_,&main);
  MONITORINFO monitor{sizeof(monitor)};GetMonitorInfoW(MonitorFromWindow(hwnd_,MONITOR_DEFAULTTONEAREST),&monitor);
  const int width=std::min(MulDiv(400,static_cast<int>(dpi_),96),static_cast<int>(monitor.rcWork.right-monitor.rcWork.left-16));
  const int available=monitor.rcWork.bottom-button.bottom-8;
  const int height=std::max(180,std::min(MulDiv(e.nativeTabLimit?230:570,static_cast<int>(dpi_),96),available));
  const int x=std::clamp(button.right-width,monitor.rcWork.left+4,std::max(monitor.rcWork.left+4,monitor.rcWork.right-width-4));
  const int y=std::min(button.bottom+4,monitor.rcWork.bottom-height-4);
  popupExtensionIndex_=index;
  extensionPopupWindow_=CreateWindowExW(WS_EX_TOOLWINDOW,L"JisuExtensionPopup",e.name.c_str(),WS_POPUP|WS_BORDER|WS_CLIPCHILDREN,x,y,width,height,hwnd_,nullptr,instance_,this);
  if(!extensionPopupWindow_){popupExtensionIndex_=static_cast<size_t>(-1);return;}
  const DWORD corner=2;DwmSetWindowAttribute(extensionPopupWindow_,33,&corner,sizeof(corner));
  const BOOL dark=palette_.dark;DwmSetWindowAttribute(extensionPopupWindow_,20,&dark,sizeof(dark));
  extensionPopup_=std::make_unique<Tab>();extensionPopup_->id=nextTabId_++;extensionPopup_->url=L"chrome-extension://"+e.id+L"/"+page;
  extensionPopup_->parent=extensionPopupWindow_;
  const auto generation=extensionPopupGeneration_;const auto window=extensionPopupWindow_;const auto url=extensionPopup_->url;
  std::weak_ptr<int> alive=alive_;
  ShowWindow(window,SW_SHOWNOACTIVATE);WriteExtensionToolbarState();
  const HRESULT started=environment_->CreateCoreWebView2Controller(window,Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
    [this,alive,generation,window,url](HRESULT result,ICoreWebView2Controller* controller)->HRESULT {
      if(alive.expired()||generation!=extensionPopupGeneration_||!extensionPopup_){if(controller)controller->Close();return S_OK;}
      if(FAILED(result)||!controller){CloseExtensionPopup();return S_OK;}
      extensionPopup_->controller=controller;controller->get_CoreWebView2(extensionPopup_->webview.GetAddressOf());
      RECT r{};GetClientRect(window,&r);controller->put_Bounds(r);controller->put_IsVisible(TRUE);
      ComPtr<ICoreWebView2Controller2> c2;if(SUCCEEDED(controller->QueryInterface(IID_PPV_ARGS(c2.GetAddressOf())))){const auto c=palette_.content;c2->put_DefaultBackgroundColor({255,c.r,c.g,c.b});}
      auto view=extensionPopup_->webview;if(!view){CloseExtensionPopup();return S_OK;}
      EventRegistrationToken token{};
      view->add_WebMessageReceived(Callback<ICoreWebView2WebMessageReceivedEventHandler>([this,alive,generation](ICoreWebView2*,ICoreWebView2WebMessageReceivedEventArgs* args)->HRESULT {
        if(alive.expired()||generation!=extensionPopupGeneration_||!extensionPopup_)return S_OK;
        CoTaskString source,text;args->get_Source(source.Put());if(SUCCEEDED(args->TryGetWebMessageAsString(text.Put())))HandleTabLimitMessage(*extensionPopup_,source.Str(),ToUtf8(text.Str()));return S_OK;
      }).Get(),&token);
      view->add_NavigationCompleted(Callback<ICoreWebView2NavigationCompletedEventHandler>([this,alive,generation,window](ICoreWebView2*,ICoreWebView2NavigationCompletedEventArgs* args)->HRESULT {
        if(alive.expired()||generation!=extensionPopupGeneration_)return S_OK;
        BOOL success=FALSE;args->get_IsSuccess(&success);extensionPopupLoaded_=success!=FALSE;WriteExtensionToolbarState();
        // 弹层按页面内容定高（宽度跟页面自己写的宽度），不再一律 400×570：内容少留一大块空白、内容多出滚动条
        if(success&&extensionPopup_&&extensionPopup_->webview)extensionPopup_->webview->ExecuteScript(
          L"(()=>{const b=document.body,d=document.documentElement;if(!b)return null;const s=getComputedStyle(b),r=b.getBoundingClientRect();"
          L"return [Math.ceil(r.width+parseFloat(s.marginLeft)+parseFloat(s.marginRight)),Math.ceil(Math.max(d.scrollHeight,r.height+parseFloat(s.marginTop)+parseFloat(s.marginBottom)))];})()",
          Callback<ICoreWebView2ExecuteScriptCompletedHandler>([this,alive,generation,window](HRESULT hr,LPCWSTR result)->HRESULT{
            if(alive.expired()||generation!=extensionPopupGeneration_||FAILED(hr)||!result||!IsWindow(window))return S_OK;
            int cssWidth=0,cssHeight=0;if(swscanf_s(result,L"[%d,%d]",&cssWidth,&cssHeight)!=2||cssWidth<=0||cssHeight<=0)return S_OK;
            RECT current{};GetWindowRect(window,&current);
            MONITORINFO monitor{sizeof(monitor)};GetMonitorInfoW(MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST),&monitor);
            const int dpi=static_cast<int>(GetDpiForWindow(window));
            const int border=(current.right-current.left)-[&]{RECT c{};GetClientRect(window,&c);return static_cast<int>(c.right);}();
            const int width=std::clamp(MulDiv(cssWidth,dpi,96)+border,MulDiv(200,dpi,96),std::min<int>(MulDiv(800,dpi,96),monitor.rcWork.right-monitor.rcWork.left-16));
            const int height=std::clamp(MulDiv(cssHeight,dpi,96)+border,MulDiv(80,dpi,96),std::min<int>(MulDiv(600,dpi,96),static_cast<int>(monitor.rcWork.bottom-current.top-4)));
            const int x=std::clamp(static_cast<int>(current.right)-width,static_cast<int>(monitor.rcWork.left)+4,std::max<int>(monitor.rcWork.left+4,monitor.rcWork.right-width-4));
            SetWindowPos(window,nullptr,x,current.top,width,height,SWP_NOZORDER|SWP_NOACTIVATE);
            return S_OK;
          }).Get());
        // Only a user-clicked foreground browser moves keyboard focus into its own popup.
        if(GetForegroundWindow()==hwnd_&&extensionPopup_)extensionPopup_->controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
        return S_OK;
      }).Get(),&token);
      view->add_NavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>([this,alive,generation,url](ICoreWebView2*,ICoreWebView2NavigationStartingEventArgs* args)->HRESULT {
        if(alive.expired()||generation!=extensionPopupGeneration_)return S_OK;
        CoTaskString uri;args->get_Uri(uri.Put());const auto value=uri.Str();
        const auto prefix=url.substr(0,url.find(L'/',19)+1);
        if(!value.starts_with(prefix)){args->put_Cancel(TRUE);}
        return S_OK;
      }).Get(),&token);
      view->add_NewWindowRequested(Callback<ICoreWebView2NewWindowRequestedEventHandler>([this,alive,generation](ICoreWebView2*,ICoreWebView2NewWindowRequestedEventArgs* args)->HRESULT {
        args->put_Handled(TRUE);if(alive.expired()||generation!=extensionPopupGeneration_)return S_OK;
        CoTaskString uri;args->get_Uri(uri.Put());const auto u=uri.Str();
        if(u.starts_with(L"http://")||u.starts_with(L"https://")||u.starts_with(L"chrome-extension://"))PostMessageW(hwnd_,kMsgOpenExtensionSettings,0,reinterpret_cast<LPARAM>(new std::wstring(u)));
        return S_OK;
      }).Get(),&token);
      controller->add_AcceleratorKeyPressed(Callback<ICoreWebView2AcceleratorKeyPressedEventHandler>([this,alive,generation](ICoreWebView2Controller*,ICoreWebView2AcceleratorKeyPressedEventArgs* args)->HRESULT {
        if(alive.expired())return S_OK;UINT key=0;args->get_VirtualKey(&key);if(key==VK_ESCAPE){args->put_Handled(TRUE);PostMessageW(hwnd_,kMsgCloseExtensionPopup,static_cast<WPARAM>(generation),0);}return S_OK;
      }).Get(),&token);
      view->Navigate(url.c_str());return S_OK;
    }).Get());
  if(FAILED(started))CloseExtensionPopup();
}
}  // namespace jisu
