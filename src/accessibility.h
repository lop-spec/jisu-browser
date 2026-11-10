#pragma once
#include "common.h"
#include <oleacc.h>
#include <uiautomationcore.h>
#include <uiautomationcoreapi.h>
#include <map>
#include <memory>
#include <mutex>
#include <vector>
namespace jisu {
inline constexpr UINT kAccessibilityAction=WM_APP+7;
enum class AccessibleKind {tab,closeTab,mute,newTab,back,forward,reload,siteInfo,star,menu,bookmark,overflow,minimize,maximize,closeWindow};
struct AccessibleNode { LONG id=0;AccessibleKind kind{};int64_t target=0;std::wstring name;RECT bounds{};LONG role=ROLE_SYSTEM_PUSHBUTTON,state=STATE_SYSTEM_FOCUSABLE;
  bool operator==(const AccessibleNode& b)const{return id==b.id&&kind==b.kind&&target==b.target&&name==b.name&&role==b.role&&state==b.state&&EqualRect(&bounds,&b.bounds);}
};
struct AccessibilityState {
  std::mutex mutex;HWND window=nullptr;uintptr_t cookie=0;bool alive=true;std::wstring name;LONG focus=0,nextId=65536;
  std::map<std::pair<AccessibleKind,int64_t>,LONG> ids;std::vector<AccessibleNode> nodes;
};
uintptr_t NextAccessibilityCookie();
ComPtr<IAccessible> MakeAccessibilityObject(const std::shared_ptr<AccessibilityState>& state);
LRESULT AccessibilityObject(IAccessible* object,WPARAM wParam);
LRESULT AccessibilityUiaObject(HWND window,IAccessible* object,WPARAM wParam,LPARAM lParam);
void ReleaseAccessibilityUia(HWND window);
}
