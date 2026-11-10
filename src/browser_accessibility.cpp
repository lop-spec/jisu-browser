#include "browser_window.h"
#include "browser_url.h"
#include <algorithm>
namespace jisu {
void BrowserWindow::QueueAccessibility(){
  if(!accessibility_||accessibilityQueued_||destroyed_)return;accessibilityQueued_=true;
  PostTask([this]{accessibilityQueued_=false;UpdateAccessibility();});
}
void BrowserWindow::UpdateAccessibility(){
  if(!accessibility_||destroyed_)return;
  std::vector<AccessibleNode> nodes;std::map<std::pair<AccessibleKind,int64_t>,LONG> ids;
  const bool visible=IsWindowVisible(hwnd_)&&!IsIconic(hwnd_)&&!fullscreen_.Active();const auto* active=ActiveTab();
  std::unique_lock lock(accessibility_->mutex);
  auto add=[&](AccessibleKind kind,int64_t target,const std::wstring& name,D2D1_RECT_F rect,LONG role=ROLE_SYSTEM_PUSHBUTTON,LONG state=STATE_SYSTEM_FOCUSABLE){
    if(rect.right<=rect.left||rect.bottom<=rect.top)return;
    const auto key=std::make_pair(kind,target);auto found=accessibility_->ids.find(key);LONG id=found==accessibility_->ids.end()?accessibility_->nextId++:found->second;ids[key]=id;
    if(!visible)state|=STATE_SYSTEM_INVISIBLE|STATE_SYSTEM_OFFSCREEN;
    if(chromeKeyboardFocus_&&id==chromeFocusId_)state|=STATE_SYSTEM_FOCUSED;
    RECT bounds{static_cast<LONG>(rect.left*layout_.scale),static_cast<LONG>(rect.top*layout_.scale),static_cast<LONG>(rect.right*layout_.scale),static_cast<LONG>(rect.bottom*layout_.scale)};
    nodes.push_back({id,kind,target,TruncateUtf16(name,512),bounds,role,state});
  };
  for(size_t i=0;i<tabs_.size()&&i<layout_.tabs.size();++i){const auto& t=*tabs_[i];auto title=t.title.empty()?L"新标签页":t.title;if(options_.inPrivate)title=L"无痕 · "+std::wstring(title);
    add(AccessibleKind::tab,t.id,title,layout_.tabs[i],ROLE_SYSTEM_PAGETAB,STATE_SYSTEM_FOCUSABLE|STATE_SYSTEM_SELECTABLE|(i==active_?STATE_SYSTEM_SELECTED:0));
    if(i<layout_.tabCloses.size())add(AccessibleKind::closeTab,t.id,L"关闭 "+std::wstring(title),layout_.tabCloses[i]);
    if(i<layout_.tabAudio.size())add(AccessibleKind::mute,t.id,t.muted?L"取消静音":L"静音",layout_.tabAudio[i]);
  }
  add(AccessibleKind::newTab,0,L"新建标签页",layout_.newTab);
  add(AccessibleKind::back,0,L"后退",layout_.back,ROLE_SYSTEM_PUSHBUTTON,STATE_SYSTEM_FOCUSABLE|(active&&active->canBack?0:STATE_SYSTEM_UNAVAILABLE));
  add(AccessibleKind::forward,0,L"前进",layout_.forward,ROLE_SYSTEM_PUSHBUTTON,STATE_SYSTEM_FOCUSABLE|(active&&active->canForward?0:STATE_SYSTEM_UNAVAILABLE));
  add(AccessibleKind::reload,0,active&&active->loading?L"停止加载":L"重新加载",layout_.reload);
  add(AccessibleKind::siteInfo,0,L"站点信息",layout_.siteInfo);
  add(AccessibleKind::star,0,L"书签",layout_.star,ROLE_SYSTEM_PUSHBUTTON,STATE_SYSTEM_FOCUSABLE|(active&&IsStarrable(active->url)?0:STATE_SYSTEM_UNAVAILABLE));
  add(AccessibleKind::menu,0,L"菜单",layout_.menu);
  for(size_t i=0;i<barItems_.size()&&i<layout_.bookmarks.size();++i)add(AccessibleKind::bookmark,barItems_[i]->id,barItems_[i]->title,layout_.bookmarks[i],barItems_[i]->folder?ROLE_SYSTEM_BUTTONMENU:ROLE_SYSTEM_LINK);
  if(layout_.hasOverflow)add(AccessibleKind::overflow,0,L"更多书签",layout_.bookmarkOverflow);
  add(AccessibleKind::minimize,0,L"最小化",layout_.minimize);add(AccessibleKind::maximize,0,IsZoomed(hwnd_)?L"还原窗口":L"最大化",layout_.maximize);add(AccessibleKind::closeWindow,0,L"关闭窗口",layout_.windowClose);
  const LONG focus=chromeKeyboardFocus_?chromeFocusId_:0;const bool changed=accessibility_->nodes!=nodes||accessibility_->focus!=focus;
  accessibility_->nodes=std::move(nodes);accessibility_->ids=std::move(ids);accessibility_->focus=focus;
  lock.unlock();if(changed)NotifyWinEvent(EVENT_OBJECT_REORDER,hwnd_,OBJID_CLIENT,CHILDID_SELF);
}
void BrowserWindow::AdvanceChromeFocus(int direction){
  if(!accessibility_){accessibility_=std::make_shared<AccessibilityState>();accessibility_->window=hwnd_;accessibility_->cookie=NextAccessibilityCookie();accessibility_->name=options_.inPrivate?L"极速浏览器（无痕）":L"极速浏览器";}
  UpdateAccessibility();std::vector<AccessibleNode> nodes;
  {std::lock_guard lock(accessibility_->mutex);for(const auto& n:accessibility_->nodes)if(!(n.state&(STATE_SYSTEM_INVISIBLE|STATE_SYSTEM_UNAVAILABLE)))nodes.push_back(n);}
  if(nodes.empty())return;
  auto at=std::find_if(nodes.begin(),nodes.end(),[&](const auto& n){return n.id==chromeFocusId_&&chromeKeyboardFocus_;});
  if(at==nodes.end()){
    at=std::find_if(nodes.begin(),nodes.end(),[](const auto& n){return n.kind==AccessibleKind::back||n.kind==AccessibleKind::forward||n.kind==AccessibleKind::reload;});
    if(at==nodes.end())at=nodes.begin();if(direction<0)at=nodes.end()-1;
  }else{
    const auto index=static_cast<int64_t>(at-nodes.begin())+(direction<0?-1:1);
    if(index<0||index>=static_cast<int64_t>(nodes.size())){chromeKeyboardFocus_=false;chromeFocusId_=0;FocusAddressBar();QueueAccessibility();return;}
    at=nodes.begin()+index;
  }
  AccessibleAction(at->id,true);
}
ChromeHit BrowserWindow::KeyboardChromeHit()const{
  if(!chromeKeyboardFocus_||!accessibility_)return {};
  AccessibleNode node;bool found=false;{std::lock_guard lock(accessibility_->mutex);for(const auto& n:accessibility_->nodes)if(n.id==chromeFocusId_){node=n;found=true;break;}}
  if(!found)return {};
  int index=-1;for(size_t i=0;i<tabs_.size();++i)if(tabs_[i]->id==node.target)index=static_cast<int>(i);
  switch(node.kind){
    case AccessibleKind::tab:return {ChromeHit::kTab,index};case AccessibleKind::closeTab:return {ChromeHit::kTabClose,index};case AccessibleKind::mute:return {ChromeHit::kTabAudio,index};
    case AccessibleKind::newTab:return {ChromeHit::kNewTab};case AccessibleKind::back:return {ChromeHit::kBack};case AccessibleKind::forward:return {ChromeHit::kForward};case AccessibleKind::reload:return {ChromeHit::kReload};case AccessibleKind::siteInfo:return {ChromeHit::kSiteInfo};case AccessibleKind::star:return {ChromeHit::kStar};case AccessibleKind::menu:return {ChromeHit::kMenu};case AccessibleKind::overflow:return {ChromeHit::kBookmarkOverflow};case AccessibleKind::minimize:return {ChromeHit::kMinimize};case AccessibleKind::maximize:return {ChromeHit::kMaximize};case AccessibleKind::closeWindow:return {ChromeHit::kWindowClose};
    case AccessibleKind::bookmark:for(size_t i=0;i<barItems_.size();++i)if(barItems_[i]->id==node.target)return {ChromeHit::kBookmark,static_cast<int>(i)};return {};
  }return {};
}
void BrowserWindow::AccessibleAction(LONG id,bool focusOnly){
  if(!accessibility_||destroyed_)return;AccessibleNode node;bool found=false;
  {std::lock_guard lock(accessibility_->mutex);for(const auto& n:accessibility_->nodes)if(n.id==id){node=n;found=true;break;}}
  if(!found||node.state&(STATE_SYSTEM_INVISIBLE|STATE_SYSTEM_UNAVAILABLE))return;
  if(focusOnly){chromeKeyboardFocus_=true;chromeFocusId_=id;focusInWebView_=false;SetFocus(hwnd_);UpdateAccessibility();InvalidateChrome(false);NotifyWinEvent(EVENT_OBJECT_FOCUS,hwnd_,OBJID_CLIENT,id);return;}
  auto* tab=FindTab(static_cast<uint32_t>(node.target));
  switch(node.kind){
    case AccessibleKind::tab:if(tab&&IndexOf(*tab)<tabs_.size())Activate(IndexOf(*tab));break;
    case AccessibleKind::closeTab:if(tab&&IndexOf(*tab)<tabs_.size())CloseTab(IndexOf(*tab));break;
    case AccessibleKind::mute:if(tab&&IndexOf(*tab)<tabs_.size())ToggleMuted(IndexOf(*tab));break;
    case AccessibleKind::newTab:NewTab(L"");FocusAddressBar();break;
    case AccessibleKind::back:if(auto* t=ActiveTab();t&&t->webview&&t->canBack)t->webview->GoBack();break;
    case AccessibleKind::forward:if(auto* t=ActiveTab();t&&t->webview&&t->canForward)t->webview->GoForward();break;
    case AccessibleKind::reload:if(auto* t=ActiveTab();t&&t->webview){if(t->loading)t->webview->Stop();else t->webview->Reload();}break;
    case AccessibleKind::siteInfo:{POINT point{node.bounds.left,node.bounds.bottom};ClientToScreen(hwnd_,&point);ShowSiteInfo(point);break;}
    case AccessibleKind::star:ToggleBookmark();break;
    case AccessibleKind::menu:{POINT p{static_cast<LONG>(layout_.menu.left*layout_.scale),static_cast<LONG>(layout_.menu.bottom*layout_.scale)};ClientToScreen(hwnd_,&p);ShowMainMenu(p);break;}
    case AccessibleKind::bookmark:if(auto* b=bookmarks_.FindById(node.target)){if(b->folder){POINT p{node.bounds.left,node.bounds.bottom};ClientToScreen(hwnd_,&p);ShowBookmarkFolderMenu(*b,p);}else OpenUrl(b->url,false);}break;
    case AccessibleKind::overflow:{POINT p{node.bounds.left,node.bounds.bottom};ClientToScreen(hwnd_,&p);ShowOverflowMenu(p);break;}
    case AccessibleKind::minimize:ShowWindow(hwnd_,SW_MINIMIZE);break;
    case AccessibleKind::maximize:ShowWindow(hwnd_,IsZoomed(hwnd_)?SW_RESTORE:SW_MAXIMIZE);break;
    case AccessibleKind::closeWindow:PostMessageW(hwnd_,WM_CLOSE,0,0);break;
  }
  QueueAccessibility();
}
}  // namespace jisu
