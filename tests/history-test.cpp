#include "history.h"
#include "json.h"
#include <iostream>
#include <cstdlib>
#include <chrono>
#include <algorithm>
using namespace jisu;
void require(bool ok,const char* text){if(!ok){std::cerr<<"FAIL history: "<<text<<std::endl;std::exit(1);}}
json::Value Query(HistoryStore& s,const std::wstring& q=L"",size_t offset=0){json::Value j;require(json::Parse(s.Query(q,offset),j),"valid json");return j;}
int main(){
  wchar_t temp[MAX_PATH];GetTempPathW(MAX_PATH,temp);const auto file=std::wstring(temp)+L"jisu-history-test-"+std::to_wstring(GetCurrentProcessId())+L".json";DeleteFileW(file.c_str());
  require(HistoryStore::SafeUrl(L"https://a.example/#route")==L"https://a.example/#route","keep SPA route");
  require(HistoryStore::SafeUrl(L"https://a.example/?code=utf8&page=2")==L"https://a.example/?code=utf8&page=2","ordinary code retained");
  require(HistoryStore::SafeUrl(L"https://a.example/?q=normal&access_token=secret&page=2")==L"https://a.example/?q=normal&page=2","only secret parameter removed");
  require(HistoryStore::SafeUrl(L"https://a.example/cb?code=secret&state=x")==L"https://a.example/cb","strip SSO token");
  require(HistoryStore::SafeUrl(L"file:///private").empty()&&HistoryStore::SafeUrl(L"https://user:pass@a.example").empty()&&HistoryStore::SafeUrl(L"https://newtab.jisu.example/history.html").empty(),"reject files credentials internal");
  {
    HistoryStore s;s.Start(file);require(s.WaitIdle(),"load empty");
    s.Visit(L"https://example.com/a",L"one",0,true);s.Visit(L"https://example.com/b",L"two");s.Visit(L"https://example.com/a",L"updated");s.Title(L"https://example.com/a",L"中文动漫");require(s.WaitIdle(),"batched save");
    auto all=Query(s);require(all["total"].AsNumber()==2,"coalesce visits within 30 seconds");require(Query(s,L"中文")["total"].AsNumber()==1,"unicode search");
    const auto suggested=s.Suggest(L"example");require(!suggested.empty()&&suggested[0].url==L"https://example.com/a"&&suggested[0].typed==1,"frequency and typed ranking");
    require(all["rows"].Items()[0]["title"].AsString()==ToUtf8(L"中文动漫"),"update title");
    s.Remove(static_cast<int64_t>(all["rows"].Items()[0]["id"].AsNumber()));require(s.WaitIdle(),"delete save");require(Query(s)["total"].AsNumber()==1,"delete one");
  }
  {
    HistoryStore s;s.Start(file);require(s.WaitIdle(),"reload");require(Query(s)["total"].AsNumber()==1,"persist across restart");
    for(int i=0;i<250;++i)s.Visit(L"https://example.com/"+std::to_wstring(i),L"entry");require(s.WaitIdle(),"bulk save");require(Query(s,L"",100)["rows"].Items().size()==100,"pagination");
    s.Clear();require(s.WaitIdle(),"clear save");require(Query(s)["total"].AsNumber()==0,"clear all");
  }
  {HistoryStore s;s.Start(file);require(s.WaitIdle()&&Query(s)["total"].AsNumber()==0,"clear persisted");}
  {
    HistoryStore s;s.Start(file);require(s.WaitIdle(),"load queue fixture");
    for(int i=0;i<6000;++i)s.Visit(L"https://burst.example/"+std::to_wstring(i),L"burst");
    require(s.WaitIdle(30000)&&Query(s)["total"].AsNumber()==6000,"queue beyond 2048 retains every unique visit");
    s.Clear();s.Visit(L"https://counter.example/",L"one",0,true);
    for(int i=0;i<3000;++i)s.Visit(L"https://counter.example/",L"counter",0,true);
    require(s.WaitIdle(15000),"counter queue idle");const auto count=s.Suggest(L"counter");
    require(count.size()==1&&count[0].visits==3001&&count[0].typed==3001,"adjacent coalescing keeps all counts");
    s.Clear();const auto now=static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    s.Visit(L"https://range.example/a",L"a",now-1000);s.Visit(L"https://range.example/b",L"b",now);
    s.Visit(L"https://other.example/a",L"other",now-1000);
    s.RemoveRange(L"https://range.example",now-1500,now);require(s.WaitIdle(),"range idle");
    require(Query(s)["total"].AsNumber()==2&&Query(s,L"range.example/b")["total"].AsNumber()==1,"origin and half-open range preserve boundaries and other sites");
    s.Clear();
    for(int i=0;i<20000;++i)s.Visit(L"https://bench.example/"+std::to_wstring(i),L"benchmark entry");
    require(s.WaitIdle(60000)&&Query(s)["total"].AsNumber()==20000,"full history benchmark fixture");
    std::vector<double> times;
    for(int i=0;i<40;++i){const auto begin=std::chrono::steady_clock::now();const auto result=s.Query(L"bench");require(!result.empty(),"benchmark query reply");times.push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count());}
    std::sort(times.begin(),times.end());std::cout<<"history query 20000 entries p95_ms="<<times[37]<<'\n';require(times[37]<=100,"history query p95 within 100ms");
    s.Clear();
  }
  {
    const std::string sentinel="sentinel history";WriteFileAtomic(file,sentinel);
    HistoryStore privateHistory(true);privateHistory.Start(file);privateHistory.Visit(L"https://private.example/",L"private");
    require(privateHistory.WaitIdle()&&Query(privateHistory)["total"].AsNumber()==0,"private history never loads normal data or records visits");
    privateHistory.Clear();privateHistory.Flush();std::string bytes;require(ReadFileBytes(file,bytes)&&bytes==sentinel,"private history never writes");
  }
  DeleteFileW(file.c_str());std::cout<<"PASS history: lazy worker; persistence; title/SPA; unicode search; pagination; dedup; single delete; clear/reload; internal/file/credential/token filtering\n";
}
