#include "browser_window.h"
#include "browser_url.h"
#include "json.h"
#include <unordered_set>

namespace jisu {
void BrowserWindow::QueueBookmarkImport(uint32_t tabId,std::string request,unsigned retry) {
  auto fail=[&](const char* error){if(auto* tab=FindTab(tabId);tab&&IsBookmarksPageUrl(tab->url)){
    auto out=json::Value::MakeObject();out.Set("type",json::Value::String("bm.imported"));out.Set("count",json::Value::Number(-1));out.Set("error",json::Value::String(error));PostToPage(*tab,json::Serialize(out));}};
  if(!profileLoaded_||request.size()>32*1024*1024||profile_->importJobs.load()>=2){fail("导入尚未就绪、文件过大或已有导入，请稍后重试");return;}
  const auto revision=bookmarks_.Revision();const auto snapshot=profile_->bookmarkSnapshot;
  const auto profile=profile_;const auto alive=std::weak_ptr<int>(alive_);const HWND target=hwnd_;
  ++profile->importJobs;
  try{std::thread([this,profile,alive,target,tabId,revision,snapshot,request=std::move(request),retry]()mutable{
    auto parsed=std::make_shared<BookmarkStore>();int count=-1;std::string error,serialized;
    try{
      json::Value message;
      if(!json::Parse(request,message)||!parsed->RestoreSerialized(snapshot))error="导入数据或当前快照无效";
      else if(message["type"].AsString()=="bm.import"){
        const auto file=FromUtf8(message["file"].AsString());auto name=TruncateUtf16(FromUtf8(message["name"].AsString()),40);
        if(BookmarkStore::IsImportSourceFile(file))count=parsed->Import(file,name.empty()?L"其他浏览器":name,&error);
        else error="不允许导入此路径";
      }else if(message["type"].AsString()=="bm.importHtml"){
        size_t nodes=0;
        auto validate=[&](auto&& self,const json::Value& rows,size_t depth)->bool{
          if(!rows.IsArray()||depth>32)return false;
          for(const auto& row:rows.Items()){
            if(++nodes>20000||row["title"].AsString().size()>16000)return false;
            if(row["children"].IsArray()){if(!self(self,row["children"],depth+1))return false;}
            else if(!IsWebUrl(FromUtf8(row["url"].AsString()))||row["url"].AsString().size()>16384)return false;
          }return true;
        };
        if(!validate(validate,message["nodes"],0))error="HTML书签过大或格式无效";
        else{
          count=0;std::unordered_set<std::wstring> urls;
          auto collect=[&](auto&& self,const BookmarkNode& node)->void{if(!node.folder)urls.insert(node.url);for(const auto& c:node.children)self(self,*c);};
          for(auto* root:{&parsed->Bar(),&parsed->Other(),&parsed->Mobile()})collect(collect,*root);
          auto merge=[&](auto&& self,const json::Value& rows,BookmarkNode& parent)->void{
            for(const auto& row:rows.Items()){
              const auto title=FromUtf8(row["title"].AsString());
              if(row["children"].IsArray()){
                BookmarkNode* folder=nullptr;for(auto& c:parent.children)if(c->folder&&c->title==title){folder=c.get();break;}
                if(!folder)folder=parsed->AddFolder(parent,parent.children.size(),title);
                if(folder)self(self,row["children"],*folder);
              }else{const auto url=FromUtf8(row["url"].AsString());if(urls.insert(url).second&&parsed->AddUrl(parent,parent.children.size(),title,url))++count;}
            }
          };merge(merge,message["nodes"],parsed->Bar());
        }
      }else error="未知导入类型";
      if(count>=0)serialized=parsed->Serialized();
    }catch(...){count=-1;error="导入解析失败，原有书签未修改";}
    --profile->importJobs;
    auto* task=new std::function<void()>([this,alive,tabId,revision,snapshot,request=std::move(request),retry,parsed=std::move(parsed),count,error=std::move(error),serialized=std::move(serialized)]()mutable{
      if(alive.expired())return;
      if(count>=0&&bookmarks_.Revision()!=revision){
        if(retry<3){QueueBookmarkImport(tabId,std::move(request),retry+1);return;}
        count=-1;error="书签持续变化，未覆盖当前修改；请重新导入";
      }
      if(count>=0&&serialized!=snapshot){
        bookmarks_.AdoptParsed(*parsed);OnBookmarksChanged(std::move(serialized));
        try{std::thread([old=std::move(parsed)]()mutable{old.reset();}).detach();}catch(...){ /* The temporary frees the old tree if no worker can be created. */ }
      }
      if(auto* tab=FindTab(tabId);tab&&IsBookmarksPageUrl(tab->url)){
        auto out=json::Value::MakeObject();out.Set("type",json::Value::String("bm.imported"));out.Set("count",json::Value::Number(count));out.Set("error",json::Value::String(error));PostToPage(*tab,json::Serialize(out));
      }
    });
    if(alive.expired()||!PostMessageW(target,WM_APP+6,0,reinterpret_cast<LPARAM>(task)))delete task;
  }).detach();}catch(...){--profile->importJobs;fail("无法开始后台导入，请稍后重试");}
}
}  // namespace jisu
