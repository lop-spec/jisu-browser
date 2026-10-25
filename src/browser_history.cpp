#include "browser_window.h"
#include "json.h"
#include <algorithm>
namespace jisu {
namespace { constexpr wchar_t kHistoryUrl[]=L"https://newtab.jisu.example/history.html"; }
void BrowserWindow::RecordVisit(Tab& tab) {
  if(tab.agent || !options_.metricsPath.empty() || options_.preheatSeconds>0 || HistoryStore::SafeUrl(tab.url).empty())return;
  history_.Start(profileDir_+L"\\History.json");history_.Visit(tab.url,tab.title);
}
void BrowserWindow::OpenHistory() {
  for(size_t i=0;i<tabs_.size();++i)if(tabs_[i]->url==kHistoryUrl&&!tabs_[i]->agent){Activate(i);return;}
  NewTab(kHistoryUrl);
}
bool BrowserWindow::HandleHistoryMessage(Tab& tab,const std::string& text) {
  if(tab.url!=kHistoryUrl)return false;
  json::Value message;if(!json::Parse(text,message))return false;
  const auto type=message["type"].AsString();if(!type.starts_with("history."))return false;
  history_.Start(profileDir_+L"\\History.json");
  if(type=="history.remove")history_.Remove(static_cast<int64_t>(message["id"].AsNumber()));
  else if(type=="history.clear")history_.Clear();
  auto query=FromUtf8(message["query"].AsString());if(query.size()>200)query.resize(200);
  const auto offset=static_cast<size_t>(std::clamp(message["offset"].AsNumber(),0.0,20000.0));
  PostToPage(tab,history_.Query(query,offset));return true;
}
}  // namespace jisu
