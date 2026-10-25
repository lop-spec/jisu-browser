#include "bookmark_editor.h"
#include <algorithm>
#include <map>
#include <cwctype>

namespace jisu {
namespace {
constexpr int kName = 5101, kUrl = 5102, kParent = 5103, kFolderName = 5104, kCreate = 5105, kError = 5106, kRemove = 5107;
struct Editor {
  BookmarkEdit edit;
  bool existing = false;
  HFONT font = nullptr;
  std::function<void(HWND)> ready;
};
std::wstring Text(HWND h, int id) {
  const int n = GetWindowTextLengthW(GetDlgItem(h,id));
  std::wstring s(static_cast<size_t>(n)+1,L'\0');
  GetDlgItemTextW(h,id,s.data(),n+1);s.resize(n);return s;
}
std::wstring Trimmed(std::wstring s) {
  const auto a=s.find_first_not_of(L" \t\r\n"); if(a==std::wstring::npos)return {};
  return s.substr(a,s.find_last_not_of(L" \t\r\n")-a+1);
}
int64_t Selected(HWND h, Editor& e) {
  const LRESULT index=SendDlgItemMessageW(h,kParent,CB_GETCURSEL,0,0);
  return index>=0&&static_cast<size_t>(index)<e.edit.folders.size()?e.edit.folders[index].id:1;
}
void Populate(HWND h, Editor& e, int64_t selected) {
  SendDlgItemMessageW(h,kParent,CB_RESETCONTENT,0,0);
  int index=0,found=0;
  for(const auto& f:e.edit.folders){SendDlgItemMessageW(h,kParent,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(f.path.c_str()));if(f.id==selected)found=index;++index;}
  SendDlgItemMessageW(h,kParent,CB_SETCURSEL,found,0);
}
HWND Control(HWND h, Editor& e, const wchar_t* cls, const wchar_t* text, DWORD style, int id, int x,int y,int w,int height) {
  RECT rect{x,y,x+w,y+height};MapDialogRect(h,&rect);
  HWND child=CreateWindowExW(0,cls,text,WS_CHILD|WS_VISIBLE|style,rect.left,rect.top,rect.right-rect.left,rect.bottom-rect.top,h,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
  if(child)SendMessageW(child,WM_SETFONT,reinterpret_cast<WPARAM>(e.font),TRUE);return child;
}
INT_PTR CALLBACK DialogProc(HWND h,UINT msg,WPARAM w,LPARAM l) {
  auto* e=reinterpret_cast<Editor*>(GetWindowLongPtrW(h,DWLP_USER));
  if(msg==WM_INITDIALOG){
    e=reinterpret_cast<Editor*>(l);SetWindowLongPtrW(h,DWLP_USER,l);
    NONCLIENTMETRICSW metrics{sizeof(metrics)};SystemParametersInfoW(SPI_GETNONCLIENTMETRICS,sizeof(metrics),&metrics,0);
    e->font=CreateFontIndirectW(&metrics.lfMessageFont);
    SetWindowTextW(h,e->existing?L"编辑书签":L"添加书签");
    Control(h,*e,L"STATIC",L"名称",0,0,12,12,272,10);
    Control(h,*e,L"EDIT",e->edit.title.c_str(),WS_TABSTOP|WS_BORDER|ES_AUTOHSCROLL,kName,12,25,272,16);
    Control(h,*e,L"STATIC",L"网址",0,0,12,47,272,10);
    Control(h,*e,L"EDIT",e->edit.url.c_str(),WS_TABSTOP|WS_BORDER|ES_AUTOHSCROLL,kUrl,12,60,272,16);
    Control(h,*e,L"STATIC",L"保存到",0,0,12,83,272,10);
    Control(h,*e,L"COMBOBOX",L"",WS_TABSTOP|WS_VSCROLL|CBS_DROPDOWNLIST,kParent,12,96,272,130);
    Control(h,*e,L"STATIC",L"在所选位置新建文件夹",0,0,12,120,272,10);
    Control(h,*e,L"EDIT",L"",WS_TABSTOP|WS_BORDER|ES_AUTOHSCROLL,kFolderName,12,134,194,16);
    Control(h,*e,L"BUTTON",L"新建文件夹",WS_TABSTOP|BS_PUSHBUTTON,kCreate,212,133,72,18);
    Control(h,*e,L"STATIC",L"",0,kError,12,156,272,22);
    if(e->existing)Control(h,*e,L"BUTTON",L"移除",WS_TABSTOP|BS_PUSHBUTTON,kRemove,12,183,50,20);
    Control(h,*e,L"BUTTON",L"取消",WS_TABSTOP|BS_PUSHBUTTON,IDCANCEL,164,183,56,20);
    Control(h,*e,L"BUTTON",L"保存",WS_TABSTOP|BS_DEFPUSHBUTTON,IDOK,228,183,56,20);
    Populate(h,*e,e->edit.parent);
    SendDlgItemMessageW(h,kName,EM_SETLIMITTEXT,4096,0);SendDlgItemMessageW(h,kUrl,EM_SETLIMITTEXT,16384,0);SendDlgItemMessageW(h,kFolderName,EM_SETLIMITTEXT,200,0);
    RECT owner{},rect{};GetWindowRect(GetParent(h),&owner);GetWindowRect(h,&rect);
    MONITORINFO monitor{sizeof(monitor)};GetMonitorInfoW(MonitorFromWindow(GetParent(h),MONITOR_DEFAULTTONEAREST),&monitor);
    const int width=rect.right-rect.left,height=rect.bottom-rect.top;
    const int x=std::clamp(owner.left+(owner.right-owner.left-width)/2,monitor.rcWork.left,std::max(monitor.rcWork.left,monitor.rcWork.right-width));
    const int y=std::clamp(owner.top+80,monitor.rcWork.top,std::max(monitor.rcWork.top,monitor.rcWork.bottom-height));
    SetWindowPos(h,nullptr,x,y,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
    if(e->ready)e->ready(h);
    SetFocus(GetDlgItem(h,kName));SendDlgItemMessageW(h,kName,EM_SETSEL,0,-1);return FALSE;
  }
  if(!e)return FALSE;
  if(msg==WM_COMMAND){
    const int id=LOWORD(w);
    if(id==kCreate){
      const std::wstring name=Trimmed(Text(h,kFolderName));
      if(name.empty()){SetDlgItemTextW(h,kError,L"请输入新文件夹名称");SetFocus(GetDlgItem(h,kFolderName));return TRUE;}
      const int64_t parent=Selected(h,*e);
      auto p=std::find_if(e->edit.folders.begin(),e->edit.folders.end(),[parent](const auto& f){return f.id==parent;});
      if(p==e->edit.folders.end())return TRUE;
      const int64_t draft=-1-static_cast<int64_t>(e->edit.folders.size());
      const std::wstring path=p->path+L" / "+name;
      e->edit.folders.push_back({draft,parent,path,name,true});Populate(h,*e,draft);
      SetDlgItemTextW(h,kFolderName,L"");SetDlgItemTextW(h,kError,L"新文件夹将在点击“保存”后创建");return TRUE;
    }
    if(id==IDOK){
      e->edit.title=Trimmed(Text(h,kName));e->edit.url=Trimmed(Text(h,kUrl));e->edit.parent=Selected(h,*e);
      std::wstring lower=e->edit.url;std::transform(lower.begin(),lower.end(),lower.begin(),[](wchar_t c){return static_cast<wchar_t>(std::towlower(c));});
      if(!(lower.starts_with(L"https://")||lower.starts_with(L"http://"))||lower.find_first_of(L"\r\n\t")!=std::wstring::npos){SetDlgItemTextW(h,kError,L"请输入 http:// 或 https:// 开头的网址");return TRUE;}
      if(e->edit.title.empty())e->edit.title=e->edit.url;
      EndDialog(h,IDOK);return TRUE;
    }
    if(id==kRemove&&e->existing){e->edit.remove=true;EndDialog(h,IDOK);return TRUE;}
    if(id==IDCANCEL){EndDialog(h,IDCANCEL);return TRUE;}
  }
  if(msg==WM_CLOSE){EndDialog(h,IDCANCEL);return TRUE;}
  if(msg==WM_DESTROY&&e->font){DeleteObject(e->font);e->font=nullptr;}
  return FALSE;
}
}
std::optional<BookmarkEdit> ShowBookmarkEditor(HWND owner,HINSTANCE instance,BookmarkStore& store,
 const std::wstring& title,const std::wstring& url,int64_t parent,bool existing,const std::function<void(HWND)>& ready) {
  Editor editor;editor.edit.title=title;editor.edit.url=url;editor.edit.parent=parent;editor.existing=existing;editor.ready=ready;
  auto add=[&](auto&& self,const BookmarkNode& node,const std::wstring& prefix)->void{
    const std::wstring path=prefix.empty()?node.title:prefix+L" / "+node.title;
    editor.edit.folders.push_back({node.id,node.parent?node.parent->id:0,path,node.title,false});
    for(const auto& child:node.children)if(child->folder)self(self,*child,path);
  };
  add(add,store.Bar(),L"");add(add,store.Other(),L"");add(add,store.Mobile(),L"");
  // Aligned in-memory DLGTEMPLATE with no resource dependency, standard shell font and controls.
  struct Template { DLGTEMPLATE header; WORD menu=0,cls=0,title=0; } dialog{};
  dialog.header.style=WS_POPUP|WS_CAPTION|WS_SYSMENU|DS_MODALFRAME|DS_CENTER;
  dialog.header.cx=296;dialog.header.cy=215;
  const INT_PTR result=DialogBoxIndirectParamW(instance,&dialog.header,owner,DialogProc,reinterpret_cast<LPARAM>(&editor));
  if(editor.font)DeleteObject(editor.font);
  if(result!=IDOK)return std::nullopt;
  return editor.edit;
}
int64_t ApplyBookmarkEdit(BookmarkStore& store,const BookmarkEdit& edit,int64_t existingId) {
  if(edit.remove)return existingId&&store.Remove(existingId)?existingId:0;
  auto* original=existingId?store.FindById(existingId):nullptr;
  if(existingId&&(!original||original->folder||store.IsRoot(*original)))return 0;
  std::map<int64_t,int64_t> ids;
  for(const auto& f:edit.folders)if(!f.draft)ids[f.id]=f.id;
  // Validate the entire draft graph before creating anything.
  for(const auto& f:edit.folders)if(f.draft){if(!ids.contains(f.parent)||ids.contains(f.id)||f.title.empty())return 0;ids[f.id]=f.id;}
  if(!ids.contains(edit.parent))return 0;
  for(const auto& f:edit.folders)if(!f.draft){const auto* p=store.FindById(f.id);if(!p||!p->folder)return 0;}
  for(const auto& f:edit.folders)if(f.draft){auto* p=store.FindById(ids[f.parent]);auto* n=store.AddFolder(*p,p->children.size(),f.title);if(!n)return 0;ids[f.id]=n->id;}
  auto* parent=store.FindById(ids[edit.parent]);if(!parent||!parent->folder)return 0;
  if(original){store.Rename(existingId,edit.title);store.SetUrl(existingId,edit.url);if(original->parent!=parent)store.Move(existingId,parent->id,parent->children.size());return existingId;}
  auto* added=store.AddUrl(*parent,parent->children.size(),edit.title,edit.url);return added?added->id:0;
}
}  // namespace jisu
