#include "browser_window.h"
#include "json.h"
#include <algorithm>
using Microsoft::WRL::Callback;
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
  else if(type=="history.clear"){
    history_.Clear();
    // 内核自己的访问记录（链接的已访问颜色、站内补全）一并清掉，和页面上说的「清空」一致
    ComPtr<ICoreWebView2_13> view;ComPtr<ICoreWebView2Profile> profile;ComPtr<ICoreWebView2Profile2> profile2;
    if(tab.webview&&SUCCEEDED(tab.webview.As(&view))&&SUCCEEDED(view->get_Profile(profile.GetAddressOf()))&&SUCCEEDED(profile.As(&profile2))){
      profile2->ClearBrowsingData(COREWEBVIEW2_BROWSING_DATA_KINDS_BROWSING_HISTORY,
        Callback<ICoreWebView2ClearBrowsingDataCompletedHandler>([](HRESULT)->HRESULT{return S_OK;}).Get());
    }
  }
  auto query=FromUtf8(message["query"].AsString());if(query.size()>200)query.resize(200);
  const auto offset=static_cast<size_t>(std::clamp(message["offset"].AsNumber(),0.0,20000.0));
  PostToPage(tab,history_.Query(query,offset));return true;
}
}  // namespace jisu
