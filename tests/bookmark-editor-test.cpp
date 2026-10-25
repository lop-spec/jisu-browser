#include "bookmark_editor.h"
#include <stdexcept>
#include <iostream>
void require(bool ok) { if (!ok) throw std::runtime_error("Bookmark save/move gate failed"); }
int main() {
  CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
  jisu::BookmarkStore store;
  const auto revision=store.Revision();
  auto cancelled=jisu::ShowBookmarkEditor(nullptr,GetModuleHandleW(nullptr),store,L"fixture",L"https://example.com",1,false,[](HWND h){
    SetDlgItemTextW(h,5104,L"discarded draft");SendMessageW(h,WM_COMMAND,5105,0);PostMessageW(h,WM_COMMAND,IDCANCEL,0);
  });
  require(!cancelled&&store.Revision()==revision&&store.Bar().children.empty());
  auto edit=jisu::ShowBookmarkEditor(nullptr,GetModuleHandleW(nullptr),store,L"fixture",L"https://example.com",1,false,[](HWND h){
    require(GetDlgItem(h,5103)!=nullptr);
    SetDlgItemTextW(h,5104,L"folder A");SendMessageW(h,WM_COMMAND,5105,0);
    SetDlgItemTextW(h,5104,L"folder B");SendMessageW(h,WM_COMMAND,5105,0);
    SetDlgItemTextW(h,5101,L"saved title");PostMessageW(h,WM_COMMAND,IDOK,0);
  });
  require(edit.has_value()&&store.Bar().children.empty());
  const int64_t id=jisu::ApplyBookmarkEdit(store,*edit);
  auto* node=store.FindById(id);require(node&&node->title==L"saved title"&&node->parent->title==L"folder B"&&node->parent->parent->title==L"folder A");
  const auto oldId=node->id;
  auto moved=*edit;moved.folders.clear();moved.folders.push_back({store.Other().id,0,L"other",L"other",false});moved.parent=store.Other().id;moved.title=L"renamed";moved.url=L"https://example.org";
  require(jisu::ApplyBookmarkEdit(store,moved,oldId)==oldId);
  require(store.FindById(id)->parent==&store.Other());
  auto* a=store.Bar().children.front().get();auto* b=a->children.front().get();
  require(!store.Move(a->id,b->id,0));require(!store.Move(a->id,a->id,0));require(!store.Move(1,b->id,0));
  require(store.Move(id,b->id,0));require(store.FindById(id)->parent==b);
  const auto unchanged=store.Revision();auto invalid=moved;invalid.parent=99999;
  require(!jisu::ApplyBookmarkEdit(store,invalid,id)&&store.Revision()==unchanged);
  wchar_t temp[MAX_PATH];GetTempPathW(MAX_PATH,temp);const std::wstring file=std::wstring(temp)+L"jisu-bookmarks-test-"+std::to_wstring(GetCurrentProcessId())+L".json";
  // Copy through the public serializer into a fresh store to verify actual atomic on-disk save/reload.
  jisu::BookmarkStore disk;disk.Load(file);auto* folder=disk.AddFolder(disk.Bar(),0,L"persistent");auto* url=disk.AddUrl(disk.Bar(),1,L"one",L"https://example.com");const auto fid=folder->id,uid=url->id;
  require(disk.Move(uid,fid,0)&&disk.Save());jisu::BookmarkStore reload;require(reload.Load(file));require(reload.FindById(uid)->parent->id==fid);DeleteFileW(file.c_str());
  CoUninitialize();std::cout<<"PASS native editor controls, choose/new nested folder, cancel rollback, edit/move, cycle rejection, persistence\n";
}
