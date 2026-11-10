#include "url_drop.h"
#include <wrl/implements.h>
#include <iostream>
#include <cstring>
#include <cstdlib>
using namespace jisu;
class TextData final:public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,IDataObject>{
 public:
  explicit TextData(std::wstring value):text(std::move(value)){}int reads=0;std::wstring text;
  HRESULT STDMETHODCALLTYPE GetData(FORMATETC* format,STGMEDIUM* out)override{
    ++reads;if(!format||!out||format->cfFormat!=CF_UNICODETEXT)return DV_E_FORMATETC;
    const auto bytes=(text.size()+1)*sizeof(wchar_t);HGLOBAL block=GlobalAlloc(GMEM_MOVEABLE,bytes);if(!block)return E_OUTOFMEMORY;
    auto* data=GlobalLock(block);if(!data){GlobalFree(block);return E_OUTOFMEMORY;}std::memcpy(data,text.c_str(),bytes);GlobalUnlock(block);
    out->tymed=TYMED_HGLOBAL;out->hGlobal=block;out->pUnkForRelease=nullptr;return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*,STGMEDIUM*)override{return E_NOTIMPL;}
  HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* f)override{return f&&f->cfFormat==CF_UNICODETEXT?S_OK:DV_E_FORMATETC;}
  HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*,FORMATETC* out)override{if(out)out->ptd=nullptr;return DATA_S_SAMEFORMATETC;}
  HRESULT STDMETHODCALLTYPE SetData(FORMATETC*,STGMEDIUM*,BOOL)override{return E_NOTIMPL;}
  HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD,IEnumFORMATETC**)override{return E_NOTIMPL;}
  HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*,DWORD,IAdviseSink*,DWORD*)override{return OLE_E_ADVISENOTSUPPORTED;}
  HRESULT STDMETHODCALLTYPE DUnadvise(DWORD)override{return OLE_E_ADVISENOTSUPPORTED;}
  HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**)override{return OLE_E_ADVISENOTSUPPORTED;}
};
void Check(bool ok,const char* text){if(!ok){std::cerr<<"FAIL URL drop: "<<text<<'\n';std::exit(1);}}
int main(){
  int commits=0;std::wstring last;auto target=MakeUrlDropTarget([&](POINTL p,const std::wstring* url,bool finish){if(finish&&url){++commits;last=*url;}return p.y>=80&&p.y<=112;});
  auto data=Microsoft::WRL::Make<TextData>(L"https://example.com/path\nPage title");DWORD effect=DROPEFFECT_COPY|DROPEFFECT_LINK;
  target->DragEnter(data.Get(),0,{20,90},&effect);target->DragOver(0,{30,95},&effect);Check(data->reads==0&&effect==DROPEFFECT_COPY,"preview only queries formats");
  target->Drop(data.Get(),0,{20,90},&effect);Check(commits==1&&last==L"https://example.com/path"&&effect==DROPEFFECT_COPY,"drop accepts canonical first-line URL");
  for(const auto* invalid:{L"javascript:alert(1)",L"file:///C:/private",L"https://user:secret@example.com/",L"https://"}){
    auto bad=Microsoft::WRL::Make<TextData>(invalid);effect=DROPEFFECT_COPY;target->DragEnter(bad.Get(),0,{20,90},&effect);target->Drop(bad.Get(),0,{20,90},&effect);Check(effect==DROPEFFECT_NONE&&commits==1,"unsafe drop refused without side effects");
  }
  effect=DROPEFFECT_MOVE;target->DragEnter(data.Get(),0,{20,90},&effect);target->Drop(data.Get(),0,{20,90},&effect);Check(commits==1&&effect==DROPEFFECT_NONE,"move-only source cannot trigger copy");
  auto huge=Microsoft::WRL::Make<TextData>(L"https://example.com/"+std::wstring(40000,L'x'));effect=DROPEFFECT_COPY;target->DragEnter(huge.Get(),0,{20,90},&effect);target->Drop(huge.Get(),0,{20,90},&effect);Check(effect==DROPEFFECT_NONE&&commits==1,"oversized HGLOBAL rejected");
  std::cout<<"PASS production OLE target: lazy bounded Unicode URL, protocol/credential guards, no move side effect\n";
}
