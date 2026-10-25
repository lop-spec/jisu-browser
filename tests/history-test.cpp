#include "history.h"
#include "json.h"
#include <iostream>
#include <cstdlib>
using namespace jisu;
void require(bool ok,const char* text){if(!ok){std::cerr<<"FAIL history: "<<text<<std::endl;std::exit(1);}}
json::Value Query(HistoryStore& s,const std::wstring& q=L"",size_t offset=0){json::Value j;require(json::Parse(s.Query(q,offset),j),"valid json");return j;}
int main(){
  wchar_t temp[MAX_PATH];GetTempPathW(MAX_PATH,temp);const auto file=std::wstring(temp)+L"jisu-history-test-"+std::to_wstring(GetCurrentProcessId())+L".json";DeleteFileW(file.c_str());
  require(HistoryStore::SafeUrl(L"https://a.example/#route")==L"https://a.example/#route","keep SPA route");
  require(HistoryStore::SafeUrl(L"https://a.example/cb?code=secret&state=x")==L"https://a.example/cb","strip SSO token");
  require(HistoryStore::SafeUrl(L"file:///private").empty()&&HistoryStore::SafeUrl(L"https://user:pass@a.example").empty()&&HistoryStore::SafeUrl(L"https://newtab.jisu.example/history.html").empty(),"reject files credentials internal");
  {
    HistoryStore s;s.Start(file);require(s.WaitIdle(),"load empty");
    s.Visit(L"https://example.com/a",L"one");s.Visit(L"https://example.com/b",L"two");s.Visit(L"https://example.com/a",L"updated");s.Title(L"https://example.com/a",L"中文动漫");require(s.WaitIdle(),"batched save");
    auto all=Query(s);require(all["total"].AsNumber()==2,"coalesce visits within 30 seconds");require(Query(s,L"中文")["total"].AsNumber()==1,"unicode search");
    require(all["rows"].Items()[0]["title"].AsString()==ToUtf8(L"中文动漫"),"update title");
    s.Remove(static_cast<int64_t>(all["rows"].Items()[0]["id"].AsNumber()));require(s.WaitIdle(),"delete save");require(Query(s)["total"].AsNumber()==1,"delete one");
  }
  {
    HistoryStore s;s.Start(file);require(s.WaitIdle(),"reload");require(Query(s)["total"].AsNumber()==1,"persist across restart");
    for(int i=0;i<250;++i)s.Visit(L"https://example.com/"+std::to_wstring(i),L"entry");require(s.WaitIdle(),"bulk save");require(Query(s,L"",100)["rows"].Items().size()==100,"pagination");
    s.Clear();require(s.WaitIdle(),"clear save");require(Query(s)["total"].AsNumber()==0,"clear all");
  }
  {HistoryStore s;s.Start(file);require(s.WaitIdle()&&Query(s)["total"].AsNumber()==0,"clear persisted");}
  DeleteFileW(file.c_str());std::cout<<"PASS history: lazy worker; persistence; title/SPA; unicode search; pagination; dedup; single delete; clear/reload; internal/file/credential/token filtering\n";
}
