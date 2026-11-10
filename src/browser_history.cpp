#include "browser_window.h"
#include "json.h"
#include <algorithm>
#include <cmath>
using Microsoft::WRL::Callback;
namespace jisu {
namespace { constexpr wchar_t kHistoryUrl[]=L"https://newtab.jisu.example/history.html"; }
void BrowserWindow::RecordVisit(Tab& tab) {
  if(options_.inPrivate || tab.agent || !options_.metricsPath.empty() || options_.preheatSeconds>0 || HistoryStore::SafeUrl(tab.url).empty())return;
  history_.Start(profileDir_+L"\\History.json");history_.Visit(tab.url,tab.title,0,tab.typedNavigation);tab.typedNavigation=false;
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
  else if(type=="history.removeRange") {
    const auto origin=FromUtf8(message["origin"].AsString());
    const double start=message["start"].AsNumber(), end=message["end"].AsNumber(8640000000000000.0);
    if ((origin.empty() || (origin.size()<=2048 && FaviconOrigin(origin)==origin)) &&
        std::isfinite(start) && std::isfinite(end) && start>=0 && end>start && end<=8640000000000000.0)
      history_.RemoveRange(origin,static_cast<int64_t>(start),static_cast<int64_t>(end));
  }
  else if(type=="history.clear"){
    history_.Clear();
    // 内核自己的访问记录（链接的已访问颜色、站内补全）一并清掉，和页面上说的「清空」一致
    ComPtr<ICoreWebView2_13> view;ComPtr<ICoreWebView2Profile> profile;ComPtr<ICoreWebView2Profile2> profile2;
    if(tab.webview&&SUCCEEDED(tab.webview.As(&view))&&SUCCEEDED(view->get_Profile(profile.GetAddressOf()))&&SUCCEEDED(profile.As(&profile2))){
      const auto alive = std::weak_ptr<int>(alive_);
      const HRESULT started = profile2->ClearBrowsingData(COREWEBVIEW2_BROWSING_DATA_KINDS_BROWSING_HISTORY,
        Callback<ICoreWebView2ClearBrowsingDataCompletedHandler>([this,alive](HRESULT result)->HRESULT{
          if(!alive.expired()&&FAILED(result))ShowNotice(L"外壳历史已清除，但内核历史清除失败，请重试。");return S_OK;
        }).Get());
      if(FAILED(started))ShowNotice(L"内核历史清除未能启动，请稍后重试。");
    }else ShowNotice(L"外壳历史已清除，但当前内核未提供历史清除接口。");
  }
  auto query=FromUtf8(message["query"].AsString());if(query.size()>200)query.resize(200);
  const auto offset=static_cast<size_t>(std::clamp(message["offset"].AsNumber(),0.0,20000.0));
  const auto shared = profile_; const auto alive = std::weak_ptr<int>(alive_); const auto target = hwnd_; const auto id = tab.id;
  std::thread([this, shared, alive, target, id, query, offset] {
    const auto result = shared->history.Query(query, offset);
    auto* task = new std::function<void()>([this, alive, id, result] {
      if (alive.expired()) return;
      if (Tab* current = FindTab(id); current && current->url == kHistoryUrl) PostToPage(*current, result);
    });
    if (alive.expired() || !PostMessageW(target, WM_APP + 6, 0, reinterpret_cast<LPARAM>(task))) delete task;
  }).detach();
  return true;
}
}  // namespace jisu
