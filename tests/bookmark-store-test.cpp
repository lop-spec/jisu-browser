#include "bookmarks.h"
#include "json.h"
#include <iostream>
#include <stdexcept>
using namespace jisu;
void check(bool value) { if (!value) throw std::runtime_error("bookmark store gate failed"); }
int main() {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  wchar_t temp[MAX_PATH]; GetTempPathW(MAX_PATH,temp);
  const auto file = std::wstring(temp)+L"jisu-bookmark-store-"+std::to_wstring(GetCurrentProcessId());
  const std::string bytes=R"JSON({"custom_top":{"keep":true},"roots":{"bookmark_bar":{"id":"1","type":"folder","name":"bar","date_added":"12345","meta_info":{"root":"keep"},"children":[{"id":"4","type":"url","name":"one","url":"https://example.com/","date_last_used":"5678","meta_info":{"color":"blue"}},{"id":"900","type":"future","data":{"keep":true}}]},"other":{"type":"folder","children":[]},"synced":{"type":"folder","children":[]},"future_root":{"id":"999","keep":true}},"version":1})JSON";
  check(WriteFileAtomic(file,bytes));
  BookmarkStore store; check(store.Load(file));
  check(store.FindById(4) && store.FindByUrl(L"https://example.com/")->id==4);
  store.Rename(4,L"changed");
  auto* fresh=store.AddUrl(store.Bar(),0,L"new",L"https://new.example/"); check(fresh && fresh->id>999);
  json::Value result; check(json::Parse(store.Serialized(),result));
  check(result["custom_top"]["keep"].AsBool() && result["roots"]["future_root"]["keep"].AsBool());
  check(result["roots"]["bookmark_bar"]["meta_info"]["root"].AsString()=="keep");
  const auto& children=result["roots"]["bookmark_bar"]["children"].Items();
  check(children.size()==3 && children[1]["date_last_used"].AsString()=="5678" && children[1]["meta_info"]["color"].AsString()=="blue");
  check(children[2]["type"].AsString()=="future");
  const auto revision=store.Revision(); std::string error;
  check(store.Import(file,L"same",&error)==0 && store.Revision()==revision);
  check(store.Remove(4) && !store.FindById(4) && !store.FindByUrl(L"https://example.com/"));
  check(store.Save()); BookmarkStore reloaded;check(reloaded.Load(file) && reloaded.FindByUrl(L"https://new.example/"));
  DeleteFileW(file.c_str()); CoUninitialize();
  std::cout<<"PASS bookmark metadata, unknown children/roots, reserved ids, indexes, duplicate import and save round trip\n";
}
