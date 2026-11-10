#include "favicons.h"
#include <wincodec.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cwctype>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>

namespace jisu {
namespace {
constexpr size_t kMaxQueue = 512;
struct PixelPool { std::mutex mutex; std::map<std::wstring,std::shared_ptr<const FaviconPixels>> decoded; };
PixelPool& Pixels() { static auto* pool = new PixelPool; return *pool; }
uint64_t HashBytes(const std::string& bytes) {
  uint64_t hash=14695981039346656037ull;
  for(unsigned char c:bytes){hash^=c;hash*=1099511628211ull;}return hash;
}
uint64_t HashOrigin(const std::wstring& origin) { return HashBytes(ToUtf8(origin)); }
std::shared_ptr<const FaviconPixels> Decode(IWICImagingFactory* factory,const std::string& bytes) {
  if(!factory||bytes.empty()||bytes.size()>512*1024)return {};
  ComPtr<IStream> stream;ComPtr<IWICBitmapDecoder> decoder;ComPtr<IWICBitmapFrameDecode> frame;
  ComPtr<IWICBitmapScaler> scaler;ComPtr<IWICFormatConverter> converter;
  if(FAILED(CreateStreamOnHGlobal(nullptr,TRUE,stream.GetAddressOf())))return {};
  ULONG written=0;LARGE_INTEGER zero{};
  if(FAILED(stream->Write(bytes.data(),static_cast<ULONG>(bytes.size()),&written))||written!=bytes.size()||FAILED(stream->Seek(zero,STREAM_SEEK_SET,nullptr)))return {};
  if(FAILED(factory->CreateDecoderFromStream(stream.Get(),nullptr,WICDecodeMetadataCacheOnLoad,decoder.GetAddressOf()))||FAILED(decoder->GetFrame(0,frame.GetAddressOf())))return {};
  UINT w=0,h=0;if(FAILED(frame->GetSize(&w,&h))||!w||!h||w>1024||h>1024)return {};
  auto pixels=std::make_shared<FaviconPixels>();pixels->width=std::min(w,64u);pixels->height=std::min(h,64u);
  if(FAILED(factory->CreateBitmapScaler(scaler.GetAddressOf()))||FAILED(scaler->Initialize(frame.Get(),pixels->width,pixels->height,WICBitmapInterpolationModeFant))||
     FAILED(factory->CreateFormatConverter(converter.GetAddressOf()))||FAILED(converter->Initialize(scaler.Get(),GUID_WICPixelFormat32bppPBGRA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom)))return {};
  pixels->stride=pixels->width*4;pixels->bytes.resize(static_cast<size_t>(pixels->stride)*pixels->height);pixels->decoderThread=GetCurrentThreadId();
  if(FAILED(converter->CopyPixels(nullptr,pixels->stride,static_cast<UINT>(pixels->bytes.size()),pixels->bytes.data())))return {};
  return pixels;
}
void Publish(const std::wstring& file,std::shared_ptr<const FaviconPixels> pixels) {
  auto& pool=Pixels();std::lock_guard lock(pool.mutex);
  if(!pool.decoded.contains(file)&&pool.decoded.size()>=512)pool.decoded.erase(pool.decoded.begin());
  pool.decoded[file]=std::move(pixels);
}
}
std::shared_ptr<const FaviconPixels> DecodedFavicon(const std::wstring& file) {
  auto& pool=Pixels();std::lock_guard lock(pool.mutex);const auto found=pool.decoded.find(file);return found==pool.decoded.end()?nullptr:found->second;
}
struct FaviconCache::State {
  struct Job { std::wstring origin;std::string bytes; };
  std::wstring directory;HWND window=nullptr;UINT message=0;
  std::mutex mutex;std::condition_variable wake;std::deque<Job> queue;std::set<std::wstring> requested;
  std::unordered_map<std::wstring,int> known;
  std::unordered_map<std::wstring,uint64_t> stored;
  bool stop=false,exited=false;
  std::wstring File(const std::wstring& key)const{return directory+L"\\"+key+L".png";}
  void Notify(uint64_t hash) {
    std::lock_guard lock(mutex);if(window&&!stop)PostMessageW(window,message,static_cast<WPARAM>(hash),0);
  }
  static void Run(std::shared_ptr<State> self) {
    const HRESULT com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    struct Scope{bool ok;~Scope(){if(ok)CoUninitialize();}} scope{SUCCEEDED(com)};
    ComPtr<IWICImagingFactory> factory;
    CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(factory.GetAddressOf()));
    EnsureDirectory(self->directory);
    for(;;){
      Job job;
      {std::unique_lock lock(self->mutex);self->wake.wait(lock,[&]{return self->stop||!self->queue.empty();});
       if(self->stop){self->exited=true;self->wake.notify_all();return;}
       auto it=std::find_if(self->queue.begin(),self->queue.end(),[](const auto& j){return !j.bytes.empty();});
       if(it==self->queue.end())it=self->queue.begin();job=std::move(*it);self->queue.erase(it);}
      const auto hash=HashOrigin(job.origin);const auto key=FaviconKeyFromHash(hash),file=self->File(key);
      std::string bytes=std::move(job.bytes);const bool supplied=!bytes.empty();
      if(!supplied)ReadFileBytes(file,bytes);
      auto pixels=Decode(factory.Get(),bytes);
      bool good=pixels!=nullptr;
      if(good&&supplied){std::string existing;good=(ReadFileBytes(file,existing)&&existing==bytes)||WriteFileAtomic(file,bytes,false);}
      if(good)Publish(file,std::move(pixels));
      else if(!supplied&&!bytes.empty())DeleteFileW(file.c_str());
      {std::lock_guard lock(self->mutex);self->requested.erase(job.origin);if(good)self->known[key]=1;else if(!supplied)self->known[key]=-1;}
      if(good)self->Notify(hash);
    }
  }
};
std::wstring FaviconOrigin(const std::wstring& url) {
  const size_t split=url.find(L"://");if(split==std::wstring::npos)return {};
  std::wstring scheme=url.substr(0,split);std::transform(scheme.begin(),scheme.end(),scheme.begin(),towlower);
  if(scheme!=L"https"&&scheme!=L"http")return {};
  const size_t end=url.find_first_of(L"/?#",split+3);std::wstring host=url.substr(split+3,end==std::wstring::npos?end:end-split-3);
  if(host.empty()||host.find(L'@')!=std::wstring::npos)return {};
  std::transform(host.begin(),host.end(),host.begin(),towlower);return scheme+L"://"+host;
}
uint64_t FaviconHash(const std::wstring& url){const auto origin=FaviconOrigin(url);return origin.empty()?0:HashOrigin(origin);}
std::wstring FaviconKeyFromHash(uint64_t hash){wchar_t text[20];swprintf_s(text,L"%016llx",static_cast<unsigned long long>(hash));return text;}
std::wstring FaviconKey(const std::wstring& url){const auto hash=FaviconHash(url);return hash?FaviconKeyFromHash(hash):std::wstring();}
FaviconCache::~FaviconCache(){Stop();}
void FaviconCache::Start(const std::wstring& profile,HWND window,UINT message){
  if(state_||profile.empty())return;state_=std::make_shared<State>();state_->directory=profile+L"\\Favicons";state_->window=window;state_->message=message;
  std::thread(&State::Run,state_).detach();
}
void FaviconCache::SetWindow(HWND window){if(state_){std::lock_guard lock(state_->mutex);state_->window=window;}}
void FaviconCache::Stop(){
  if(!state_)return;auto state=std::move(state_);std::unique_lock lock(state->mutex);state->stop=true;state->window=nullptr;state->queue.clear();state->wake.notify_all();
  state->wake.wait_for(lock,std::chrono::milliseconds(300),[&]{return state->exited;});
}
std::wstring FaviconCache::Directory()const{return state_?state_->directory:std::wstring();}
std::wstring FaviconCache::Request(const std::wstring& url){
  if(!state_)return {};const auto origin=FaviconOrigin(url);if(origin.empty())return {};
  const auto key=FaviconKeyFromHash(HashOrigin(origin));auto& state=*state_;std::lock_guard lock(state.mutex);
  auto& status=state.known[key];const auto file=state.File(key);
  if(status==1){if(DecodedFavicon(file))return file;status=0;}
  if(status==0&&!state.stop&&state.queue.size()<kMaxQueue&&state.requested.insert(origin).second){state.queue.push_back({origin,{}});state.wake.notify_one();}
  return {};
}
void FaviconCache::Store(const std::wstring& url,IStream* stream){
  if(!state_||!stream)return;const auto origin=FaviconOrigin(url);if(origin.empty())return;
  STATSTG stat{};if(FAILED(stream->Stat(&stat,STATFLAG_NONAME))||!stat.cbSize.QuadPart||stat.cbSize.QuadPart>512*1024)return;
  LARGE_INTEGER zero{};if(FAILED(stream->Seek(zero,STREAM_SEEK_SET,nullptr)))return;
  std::string bytes(static_cast<size_t>(stat.cbSize.QuadPart),'\0');ULONG read=0;
  if(FAILED(stream->Read(bytes.data(),static_cast<ULONG>(bytes.size()),&read))||read!=bytes.size())return;
  const auto content=HashBytes(bytes);const auto key=FaviconKeyFromHash(HashOrigin(origin));auto& state=*state_;std::lock_guard lock(state.mutex);
  if(state.stop)return;auto& last=state.stored[key];if(last==content)return;last=content;
  state.queue.push_back({origin,std::move(bytes)});state.wake.notify_one();
}
}  // namespace jisu
