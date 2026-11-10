#include "favicons.h"
#include <iostream>
#include <chrono>
#include <cstdlib>
using namespace jisu;
void Check(bool ok,const char* text){if(!ok){std::cerr<<"FAIL favicon: "<<text<<'\n';std::exit(1);}}
int wmain(int argc,wchar_t** argv){
  Check(argc==2,"ICO fixture argument");wchar_t temp[MAX_PATH]{};GetTempPathW(MAX_PATH,temp);
  const auto root=std::wstring(temp)+L"jisu-favicons-test-"+std::to_wstring(GetCurrentProcessId());
  const auto directory=root+L"\\Favicons";EnsureDirectory(directory);
  const std::wstring url=L"https://cache.example/icon",file=directory+L"\\"+FaviconKey(url)+L".png";
  Check(CopyFileW(argv[1],file.c_str(),FALSE)!=FALSE,"copy real image");
  FaviconCache cache;cache.Start(root,nullptr,0);const auto began=GetTickCount64();
  Check(cache.Request(url).empty(),"first UI request does not read/decode");
  std::wstring ready;while(ready.empty()&&GetTickCount64()-began<5000){Sleep(10);ready=cache.Request(url);}
  const auto pixels=DecodedFavicon(file);
  Check(ready==file&&pixels&&pixels->decoderThread!=GetCurrentThreadId(),"decode on worker, not caller UI");
  Check(pixels->width<=64&&pixels->height<=64&&pixels->bytes.size()==pixels->stride*pixels->height,"bounded premultiplied pixels");
  const auto invalid=directory+L"\\"+FaviconKey(L"https://html.example/")+L".png";
  WriteFileAtomic(invalid,"<!doctype html><title>not an image</title>");cache.Request(L"https://html.example/");
  cache.Request(L"https://private.intranet.example/");cache.Request(L"http://127.0.0.1/");
  const auto until=GetTickCount64()+3000;while(GetFileAttributesW(invalid.c_str())!=INVALID_FILE_ATTRIBUTES&&GetTickCount64()<until)Sleep(10);
  Check(cache.Request(L"https://private.intranet.example/").empty()&&cache.Request(L"http://127.0.0.1/").empty(),"missing bookmark origins never acquire files");
  Check(GetFileAttributesW(invalid.c_str())==INVALID_FILE_ATTRIBUTES,"HTML is not cached as an icon");
  cache.Stop();DeleteFileW(file.c_str());RemoveDirectoryW(directory.c_str());RemoveDirectoryW(root.c_str());
  std::cout<<"PASS cached ICO decoded off UI; <=64px pixels; missing/private DNS origins never probed; corrupt image rejected; worker ms="<<(GetTickCount64()-began)<<'\n';
}
