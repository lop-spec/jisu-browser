#include "url_drop.h"
#include "browser_url.h"
#include <wrl/implements.h>
#include <algorithm>
namespace jisu {
namespace {
UINT UrlFormat(){static const UINT value=RegisterClipboardFormatW(L"UniformResourceLocatorW");return value;}
bool CanRead(IDataObject* data){
  if(!data)return false;
  for(const auto format:{UrlFormat(),static_cast<UINT>(CF_UNICODETEXT)}){
    FORMATETC value{static_cast<CLIPFORMAT>(format),nullptr,DVASPECT_CONTENT,-1,TYMED_HGLOBAL};
    if(format&&SUCCEEDED(data->QueryGetData(&value)))return true;
  }return false;
}
std::wstring ReadUrl(IDataObject* data){
  if(!data)return {};
  for(const auto format:{UrlFormat(),static_cast<UINT>(CF_UNICODETEXT)}){
    FORMATETC value{static_cast<CLIPFORMAT>(format),nullptr,DVASPECT_CONTENT,-1,TYMED_HGLOBAL};STGMEDIUM medium{};
    if(!format||FAILED(data->GetData(&value,&medium)))continue;
    std::wstring text;
    if(medium.tymed==TYMED_HGLOBAL&&medium.hGlobal){
      const auto size=GlobalSize(medium.hGlobal);
      if(size>=sizeof(wchar_t)&&size<=65536){if(const auto* chars=static_cast<const wchar_t*>(GlobalLock(medium.hGlobal))){
        text.assign(chars,wcsnlen(chars,size/sizeof(wchar_t)));GlobalUnlock(medium.hGlobal);
      }}
    }
    ReleaseStgMedium(&medium);
    text=Trim(text.substr(0,text.find_first_of(L"\r\n")));
    const auto start=text.find(L"://");const auto host=start==std::wstring::npos?std::wstring():text.substr(start+3,text.find_first_of(L"/?#",start+3)-(start+3));
    if(text.size()<=16384&&IsStarrable(text)&&!IsBlockedNavigationUrl(text)&&!host.empty()&&host.find(L'@')==std::wstring::npos&&text.find_first_of(L" \t\r\n") == std::wstring::npos)return text;
  }return {};
}
class UrlDropTarget final:public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,IDropTarget>{
 public:
  explicit UrlDropTarget(UrlDropHandler handler):handler_(std::move(handler)){}
  HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data,DWORD,POINTL point,DWORD* effect)override{
    if(!effect)return E_POINTER;readable_=CanRead(data);available_=*effect;return DragOver(0,point,effect);
  }
  HRESULT STDMETHODCALLTYPE DragOver(DWORD,POINTL point,DWORD* effect)override{
    if(!effect)return E_POINTER;*effect=readable_&&handler_(point,nullptr,false)?Effect():DROPEFFECT_NONE;return S_OK;
  }
  HRESULT STDMETHODCALLTYPE DragLeave()override{readable_=false;handler_({},nullptr,true);return S_OK;}
  HRESULT STDMETHODCALLTYPE Drop(IDataObject* data,DWORD,POINTL point,DWORD* effect)override{
    if(!effect)return E_POINTER;const auto url=readable_?ReadUrl(data):std::wstring();
    *effect=Effect()!=DROPEFFECT_NONE&&!url.empty()&&handler_(point,&url,true)?Effect():DROPEFFECT_NONE;
    readable_=false;handler_({},nullptr,true);return S_OK;
  }
 private:
  DWORD Effect()const{return available_&DROPEFFECT_COPY?DROPEFFECT_COPY:available_&DROPEFFECT_LINK?DROPEFFECT_LINK:DROPEFFECT_NONE;}
  UrlDropHandler handler_;bool readable_=false;DWORD available_=0;
};
}
ComPtr<IDropTarget> MakeUrlDropTarget(UrlDropHandler handler){return Microsoft::WRL::Make<UrlDropTarget>(std::move(handler));}
}
