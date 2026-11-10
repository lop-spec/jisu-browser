#include "accessibility.h"
#include <atomic>
#include <algorithm>
#include <oleauto.h>
namespace jisu {
uintptr_t NextAccessibilityCookie(){static std::atomic<uintptr_t> next{1};return next.fetch_add(1);}
namespace {
VARIANT Child(LONG id){VARIANT v{};v.vt=VT_I4;v.lVal=id;return v;}
struct EnumEntry{LONG id;ComPtr<IDispatch> object;};
class AccessibleEnum final:public IEnumVARIANT{
 std::atomic<ULONG> refs_{1};std::vector<EnumEntry> ids_;size_t position_=0;
 public:
 explicit AccessibleEnum(std::vector<EnumEntry> ids,size_t position=0):ids_(std::move(ids)),position_(position){}
 HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** out)override{if(!out)return E_POINTER;*out=nullptr;if(id==IID_IUnknown||id==IID_IEnumVARIANT){*out=static_cast<IEnumVARIANT*>(this);AddRef();return S_OK;}return E_NOINTERFACE;}
 ULONG STDMETHODCALLTYPE AddRef()override{return ++refs_;}ULONG STDMETHODCALLTYPE Release()override{const auto n=--refs_;if(!n)delete this;return n;}
 HRESULT STDMETHODCALLTYPE Next(ULONG count,VARIANT* values,ULONG* fetched)override{if(!values||(!fetched&&count!=1))return E_POINTER;ULONG n=0;while(n<count&&position_<ids_.size()){const auto& entry=ids_[position_++];auto& value=values[n++];value=Child(entry.id);if(entry.object){value.vt=VT_DISPATCH;value.pdispVal=entry.object.Get();value.pdispVal->AddRef();}}if(fetched)*fetched=n;return n==count?S_OK:S_FALSE;}
 HRESULT STDMETHODCALLTYPE Skip(ULONG count)override{const auto n=std::min(static_cast<size_t>(count),ids_.size()-position_);position_+=n;return n==count?S_OK:S_FALSE;}
 HRESULT STDMETHODCALLTYPE Reset()override{position_=0;return S_OK;}
 HRESULT STDMETHODCALLTYPE Clone(IEnumVARIANT** out)override{if(!out)return E_POINTER;*out=new AccessibleEnum(ids_,position_);return S_OK;}
};
class ChromeAccessible final:public IAccessible{
 std::atomic<ULONG> refs_{1};std::shared_ptr<AccessibilityState> state_;ComPtr<IAccessible> standard_;
 bool Alive(){std::lock_guard lock(state_->mutex);return state_->alive;}
 bool Node(VARIANT child,AccessibleNode& out){if(child.vt!=VT_I4)return false;std::lock_guard lock(state_->mutex);if(!state_->alive)return false;for(const auto& n:state_->nodes)if(n.id==child.lVal){out=n;return true;}return false;}
 std::vector<LONG> Ids(){std::vector<LONG> ids;if(!Alive())return ids;LONG native=0;standard_->get_accChildCount(&native);for(LONG i=1;i<=std::min(native,65535L);++i)ids.push_back(i);std::lock_guard lock(state_->mutex);for(const auto& n:state_->nodes)ids.push_back(n.id);return ids;}
 std::vector<EnumEntry> EnumEntries(){std::vector<EnumEntry> entries;for(const auto id:Ids()){EnumEntry item{id,{}};if(id<65536)standard_->get_accChild(Child(id),item.object.GetAddressOf());entries.push_back(std::move(item));}return entries;}
 HRESULT Action(VARIANT child,bool focus){AccessibleNode node;if(!Node(child,node))return E_INVALIDARG;if(node.state&(STATE_SYSTEM_UNAVAILABLE|STATE_SYSTEM_INVISIBLE))return S_FALSE;HWND window;uintptr_t cookie;{std::lock_guard lock(state_->mutex);window=state_->window;cookie=state_->cookie;}return PostMessageW(window,kAccessibilityAction,cookie,focus?-node.id:node.id)?S_OK:CO_E_OBJNOTCONNECTED;}
 public:
 explicit ChromeAccessible(std::shared_ptr<AccessibilityState> state):state_(std::move(state)){CreateStdAccessibleObject(state_->window,OBJID_CLIENT,IID_PPV_ARGS(standard_.GetAddressOf()));}
 bool Ready()const{return standard_!=nullptr;}
 HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** out)override{
  if(!out)return E_POINTER;*out=nullptr;
  if(id==IID_IEnumVARIANT){*out=static_cast<IEnumVARIANT*>(new AccessibleEnum(EnumEntries()));return S_OK;}
  if(id==IID_IUnknown||id==IID_IDispatch||id==IID_IAccessible){*out=static_cast<IAccessible*>(this);AddRef();return S_OK;}return E_NOINTERFACE;
 }
 ULONG STDMETHODCALLTYPE AddRef()override{return ++refs_;}ULONG STDMETHODCALLTYPE Release()override{const auto n=--refs_;if(!n)delete this;return n;}
 HRESULT STDMETHODCALLTYPE GetTypeInfoCount(UINT* count)override{return standard_->GetTypeInfoCount(count);}
 HRESULT STDMETHODCALLTYPE GetTypeInfo(UINT index,LCID locale,ITypeInfo** info)override{return standard_->GetTypeInfo(index,locale,info);}
 HRESULT STDMETHODCALLTYPE GetIDsOfNames(REFIID,LPOLESTR* names,UINT count,LCID locale,DISPID* ids)override{ComPtr<ITypeInfo> info;const auto hr=standard_->GetTypeInfo(0,locale,info.GetAddressOf());return SUCCEEDED(hr)?DispGetIDsOfNames(info.Get(),names,count,ids):hr;}
 HRESULT STDMETHODCALLTYPE Invoke(DISPID id,REFIID,LCID locale,WORD flags,DISPPARAMS* args,VARIANT* result,EXCEPINFO* exception,UINT* error)override{
  if(id==DISPID_NEWENUM&&result){VariantInit(result);result->vt=VT_UNKNOWN;result->punkVal=new AccessibleEnum(EnumEntries());return S_OK;}
  ComPtr<ITypeInfo> info;const auto hr=standard_->GetTypeInfo(0,locale,info.GetAddressOf());return SUCCEEDED(hr)?DispInvoke(static_cast<IAccessible*>(this),info.Get(),id,flags,args,result,exception,error):hr;
 }
 HRESULT STDMETHODCALLTYPE get_accParent(IDispatch** out)override{if(!Alive())return CO_E_OBJNOTCONNECTED;return standard_->get_accParent(out);}
 HRESULT STDMETHODCALLTYPE get_accChildCount(LONG* out)override{if(!out)return E_POINTER;if(!Alive())return CO_E_OBJNOTCONNECTED;*out=static_cast<LONG>(Ids().size());return S_OK;}
 HRESULT STDMETHODCALLTYPE get_accChild(VARIANT child,IDispatch** out)override{if(!out)return E_POINTER;*out=nullptr;if(!Alive())return CO_E_OBJNOTCONNECTED;AccessibleNode node;if(Node(child,node))return S_FALSE;return standard_->get_accChild(child,out);}
 HRESULT STDMETHODCALLTYPE get_accName(VARIANT child,BSTR* out)override{
  if(!out)return E_POINTER;*out=nullptr;if(!Alive())return CO_E_OBJNOTCONNECTED;
  if(child.vt==VT_I4&&child.lVal==CHILDID_SELF){std::lock_guard lock(state_->mutex);*out=SysAllocString(state_->name.c_str());return S_OK;}
  AccessibleNode node;if(Node(child,node)){*out=SysAllocString(node.name.c_str());return S_OK;}return standard_->get_accName(child,out);
 }
 HRESULT STDMETHODCALLTYPE get_accValue(VARIANT child,BSTR* out)override{if(!out)return E_POINTER;*out=nullptr;if(!Alive())return CO_E_OBJNOTCONNECTED;AccessibleNode n;return Node(child,n)?S_FALSE:standard_->get_accValue(child,out);}
 HRESULT STDMETHODCALLTYPE get_accDescription(VARIANT child,BSTR* out)override{if(!out)return E_POINTER;*out=nullptr;if(!Alive())return CO_E_OBJNOTCONNECTED;AccessibleNode n;return Node(child,n)?S_FALSE:standard_->get_accDescription(child,out);}
 HRESULT STDMETHODCALLTYPE get_accRole(VARIANT child,VARIANT* out)override{if(!out)return E_POINTER;if(!Alive())return CO_E_OBJNOTCONNECTED;AccessibleNode n;if(Node(child,n)){*out=Child(n.role);return S_OK;}return standard_->get_accRole(child,out);}
 HRESULT STDMETHODCALLTYPE get_accState(VARIANT child,VARIANT* out)override{if(!out)return E_POINTER;if(!Alive())return CO_E_OBJNOTCONNECTED;AccessibleNode n;if(Node(child,n)){*out=Child(n.state);return S_OK;}return standard_->get_accState(child,out);}
 HRESULT STDMETHODCALLTYPE get_accHelp(VARIANT child,BSTR* out)override{if(!out)return E_POINTER;*out=nullptr;if(!Alive())return CO_E_OBJNOTCONNECTED;AccessibleNode n;return Node(child,n)?S_FALSE:standard_->get_accHelp(child,out);}
 HRESULT STDMETHODCALLTYPE get_accHelpTopic(BSTR* file,VARIANT child,LONG* topic)override{if(!Alive())return CO_E_OBJNOTCONNECTED;AccessibleNode n;if(Node(child,n)){if(file)*file=nullptr;if(topic)*topic=0;return S_FALSE;}return standard_->get_accHelpTopic(file,child,topic);}
 HRESULT STDMETHODCALLTYPE get_accKeyboardShortcut(VARIANT child,BSTR* out)override{if(!out)return E_POINTER;*out=nullptr;if(!Alive())return CO_E_OBJNOTCONNECTED;AccessibleNode n;return Node(child,n)?S_FALSE:standard_->get_accKeyboardShortcut(child,out);}
 HRESULT STDMETHODCALLTYPE get_accFocus(VARIANT* out)override{if(!out)return E_POINTER;if(!Alive())return CO_E_OBJNOTCONNECTED;{std::lock_guard lock(state_->mutex);if(state_->focus){*out=Child(state_->focus);return S_OK;}}return standard_->get_accFocus(out);}
 HRESULT STDMETHODCALLTYPE get_accSelection(VARIANT* out)override{if(!out)return E_POINTER;if(!Alive())return CO_E_OBJNOTCONNECTED;std::lock_guard lock(state_->mutex);for(const auto& n:state_->nodes)if(n.kind==AccessibleKind::tab&&(n.state&STATE_SYSTEM_SELECTED)){*out=Child(n.id);return S_OK;}VariantInit(out);return S_FALSE;}
 HRESULT STDMETHODCALLTYPE get_accDefaultAction(VARIANT child,BSTR* out)override{if(!out)return E_POINTER;*out=nullptr;if(!Alive())return CO_E_OBJNOTCONNECTED;AccessibleNode n;if(Node(child,n)){*out=SysAllocString(L"激活");return S_OK;}return standard_->get_accDefaultAction(child,out);}
 HRESULT STDMETHODCALLTYPE accSelect(LONG flags,VARIANT child)override{if(!Alive())return CO_E_OBJNOTCONNECTED;AccessibleNode n;if(!Node(child,n))return standard_->accSelect(flags,child);if(flags&SELFLAG_TAKEFOCUS)return Action(child,true);if(flags&SELFLAG_TAKESELECTION)return Action(child,false);return E_INVALIDARG;}
 HRESULT STDMETHODCALLTYPE accLocation(LONG* x,LONG* y,LONG* width,LONG* height,VARIANT child)override{if(!x||!y||!width||!height)return E_POINTER;if(!Alive())return CO_E_OBJNOTCONNECTED;AccessibleNode n;if(Node(child,n)){POINT point{n.bounds.left,n.bounds.top};ClientToScreen(state_->window,&point);*x=point.x;*y=point.y;*width=n.bounds.right-n.bounds.left;*height=n.bounds.bottom-n.bounds.top;return S_OK;}return standard_->accLocation(x,y,width,height,child);}
 HRESULT STDMETHODCALLTYPE accNavigate(LONG direction,VARIANT start,VARIANT* out)override{
  if(!out)return E_POINTER;VariantInit(out);if(!Alive())return CO_E_OBJNOTCONNECTED;const auto ids=Ids();LONG chosen=0;
  if(start.vt==VT_I4&&start.lVal==CHILDID_SELF){if(direction==NAVDIR_FIRSTCHILD&&!ids.empty())chosen=ids.front();else if(direction==NAVDIR_LASTCHILD&&!ids.empty())chosen=ids.back();}
  else if(start.vt==VT_I4){const auto it=std::find(ids.begin(),ids.end(),start.lVal);if(it!=ids.end()){if((direction==NAVDIR_NEXT||direction==NAVDIR_RIGHT)&&it+1!=ids.end())chosen=*(it+1);else if((direction==NAVDIR_PREVIOUS||direction==NAVDIR_LEFT)&&it!=ids.begin())chosen=*(it-1);else if(direction==NAVDIR_UP){out->vt=VT_DISPATCH;out->pdispVal=this;AddRef();return S_OK;}}}
  if(chosen){*out=Child(chosen);return S_OK;}return S_FALSE;
 }
 HRESULT STDMETHODCALLTYPE accHitTest(LONG x,LONG y,VARIANT* out)override{if(!out)return E_POINTER;if(!Alive())return CO_E_OBJNOTCONNECTED;{std::lock_guard lock(state_->mutex);POINT point{x,y};ScreenToClient(state_->window,&point);for(auto it=state_->nodes.rbegin();it!=state_->nodes.rend();++it)if(PtInRect(&it->bounds,point)){*out=Child(it->id);return S_OK;}}return standard_->accHitTest(x,y,out);}
 HRESULT STDMETHODCALLTYPE accDoDefaultAction(VARIANT child)override{if(!Alive())return CO_E_OBJNOTCONNECTED;AccessibleNode n;return Node(child,n)?Action(child,false):standard_->accDoDefaultAction(child);}
 HRESULT STDMETHODCALLTYPE put_accName(VARIANT,BSTR)override{return E_ACCESSDENIED;}
 HRESULT STDMETHODCALLTYPE put_accValue(VARIANT child,BSTR value)override{if(!Alive())return CO_E_OBJNOTCONNECTED;AccessibleNode n;return Node(child,n)?E_ACCESSDENIED:standard_->put_accValue(child,value);}
};
}
ComPtr<IAccessible> MakeAccessibilityObject(const std::shared_ptr<AccessibilityState>& state){
  auto* provider=new ChromeAccessible(state);if(!provider->Ready()){provider->Release();return {};}
  ComPtr<IAccessible> result;result.Attach(provider);return result;
}
LRESULT AccessibilityObject(IAccessible* object,WPARAM parameter){return object?LresultFromObject(IID_IAccessible,parameter,object):0;}
LRESULT AccessibilityUiaObject(HWND window,IAccessible* object,WPARAM wParam,LPARAM lParam){
  ComPtr<IRawElementProviderSimple> provider;
  return object&&SUCCEEDED(UiaProviderFromIAccessible(object,CHILDID_SELF,UIA_PFIA_DEFAULT,provider.GetAddressOf()))?UiaReturnRawElementProvider(window,wParam,lParam,provider.Get()):0;
}
void ReleaseAccessibilityUia(HWND window){UiaReturnRawElementProvider(window,0,0,nullptr);}
}  // namespace jisu
