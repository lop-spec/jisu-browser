#include "browser_window.h"
#include "extension_toolbar.h"
#include "json.h"
#include <commctrl.h>
#include <wincodec.h>
#include <algorithm>
#include <cmath>
#include <cstring>
using Microsoft::WRL::Callback;

namespace jisu {
namespace {
std::wstring ExtensionFile(const std::wstring& folder, std::wstring relative) {
  while (!relative.empty() && (relative.front() == L'/' || relative.front() == L'\\')) relative.erase(relative.begin());
  if (relative.empty() || relative.find(L"..") != std::wstring::npos || relative.find(L':') != std::wstring::npos) return {};
  std::replace(relative.begin(), relative.end(), L'/', L'\\');
  return folder + L"\\" + relative;
}
HICON ProgramIcon(const std::wstring& folder, UINT dpi) {
  std::string bytes; json::Value manifest;
  if (!ReadFileBytes(folder + L"\\manifest.json", bytes) || !json::Parse(bytes, manifest)) return nullptr;
  auto icons = manifest["action"]["default_icon"];
  if (icons.IsNull()) icons = manifest["icons"];
  std::wstring relative;
  if (icons.IsString()) relative = FromUtf8(icons.AsString());
  else for (const auto* size : {"32", "38", "48", "24", "19", "16", "128"}) if (icons[size].IsString()) { relative = FromUtf8(icons[size].AsString()); break; }
  if(relative.empty()) for(const auto& [size,value]:icons.Members()) if(value.IsString()){relative=FromUtf8(value.AsString());break;}
  const auto file = ExtensionFile(folder, relative);
  if (file.empty()) return nullptr;
  const int size = std::clamp(MulDiv(20, static_cast<int>(dpi), 96), 16, 64);
  ComPtr<IWICImagingFactory> wic; ComPtr<IWICBitmapDecoder> decoder; ComPtr<IWICBitmapFrameDecode> frame;
  ComPtr<IWICBitmapScaler> scaler; ComPtr<IWICFormatConverter> converter;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wic.GetAddressOf()))) ||
      FAILED(wic->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, decoder.GetAddressOf())) ||
      FAILED(decoder->GetFrame(0, frame.GetAddressOf())) || FAILED(wic->CreateBitmapScaler(scaler.GetAddressOf())) ||
      FAILED(scaler->Initialize(frame.Get(), size, size, WICBitmapInterpolationModeFant)) ||
      FAILED(wic->CreateFormatConverter(converter.GetAddressOf())) ||
      FAILED(converter->Initialize(scaler.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) return nullptr;
  BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth = size; info.bmiHeader.biHeight = -size;
  info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
  void* pixels = nullptr; HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
  if (!bitmap) return nullptr;
  HICON result = nullptr;
  if (SUCCEEDED(converter->CopyPixels(nullptr, size * 4, size * size * 4, static_cast<BYTE*>(pixels)))) {
    std::vector<BYTE> mask(static_cast<size_t>(((size + 15) / 16) * 2) * size);
    HBITMAP mono = CreateBitmap(size, size, 1, 1, mask.data());
    if (mono) { ICONINFO icon{}; icon.fIcon = TRUE; icon.hbmMask = mono; icon.hbmColor = bitmap; result = CreateIconIndirect(&icon); DeleteObject(mono); }
  }
  DeleteObject(bitmap); return result;
}
std::wstring MenuLabel(std::wstring value) {
  std::wstring out; for (auto c : value) { if (c == L'&') out += L'&'; out += c == L'\t' || c == L'\n' ? L' ' : c; } return out;
}
void Tool(HWND tooltip, HWND button, const std::wstring& text) {
  TOOLINFOW info{sizeof(info)}; info.uFlags = TTF_IDISHWND | TTF_SUBCLASS; info.hwnd = GetParent(button);
  info.uId = reinterpret_cast<UINT_PTR>(button); info.lpszText = const_cast<LPWSTR>(text.c_str());
  SendMessageW(tooltip, TTM_DELTOOLW, 0, reinterpret_cast<LPARAM>(&info));
  SendMessageW(tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&info));
}
}
void BrowserWindow::RefreshExtensionToolbar() {
  if (destroyed_ || !hwnd_) return;
  if (!extensionsButton_) {
    extensionsButton_ = CreateWindowExW(0, L"BUTTON", L"扩展程序", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
      0,0,0,0,hwnd_,reinterpret_cast<HMENU>(static_cast<INT_PTR>(kExtensionsButton)),instance_,nullptr);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_WIN95_CLASSES}; InitCommonControlsEx(&controls);
    extensionTooltip_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE, TOOLTIPS_CLASSW, nullptr,
      WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,hwnd_,nullptr,instance_,nullptr);
    if (extensionTooltip_) { SendMessageW(extensionTooltip_,TTM_SETMAXTIPWIDTH,0,320); Tool(extensionTooltip_,extensionsButton_,L"扩展程序：打开、固定到工具栏"); }
  }
  for (size_t i=0;i<extensions_.size();++i) {
    auto& e=extensions_[i];
    if (!e.button) {
      e.button = CreateWindowExW(0,L"BUTTON",e.name.c_str(),WS_CHILD | WS_TABSTOP | BS_OWNERDRAW,
        0,0,0,0,hwnd_,reinterpret_cast<HMENU>(static_cast<INT_PTR>(kExtensionButtonBase+i)),instance_,nullptr);
      if (extensionTooltip_) Tool(extensionTooltip_,e.button,e.name);
    }
    if (!e.toolbarIcon) e.toolbarIcon=ProgramIcon(e.path,dpi_);
    EnableWindow(e.button,e.enabled && SUCCEEDED(e.result) && !e.id.empty());
  }
  Relayout(); InvalidateChrome(); WriteExtensionToolbarState();
}
float BrowserWindow::LayoutExtensionToolbar(float right,float top,float scale) {
  if (!extensionsButton_) return right;
  auto place=[top,scale](HWND button,float left) {
    SetWindowPos(button,nullptr,static_cast<int>(std::lround(left*scale)),static_cast<int>(std::lround((top+4)*scale)),
      static_cast<int>(std::lround(32*scale)),static_cast<int>(std::lround(32*scale)),SWP_NOZORDER | SWP_NOACTIVATE);
  };
  float left=right-32; place(extensionsButton_,left); ShowWindow(extensionsButton_,SW_SHOWNOACTIVATE);
  for (size_t i=extensions_.size();i-->0;) {
    auto& e=extensions_[i];
    if (e.pinned && e.enabled && SUCCEEDED(e.result) && !e.id.empty() && left>360) {
      left-=36; place(e.button,left); ShowWindow(e.button,SW_SHOWNOACTIVATE);
    } else ShowWindow(e.button,SW_HIDE);
  }
  return left-6;
}
void BrowserWindow::DrawExtensionButton(const DRAWITEMSTRUCT& d) {
  const bool down=(d.itemState&ODS_SELECTED)!=0,disabled=(d.itemState&ODS_DISABLED)!=0;
  HBRUSH background=CreateSolidBrush(ToColorRef(down?palette_.buttonHover:palette_.toolbar));FillRect(d.hDC,&d.rcItem,background);DeleteObject(background);
  HICON icon=nullptr;std::wstring fallback=L"扩";
  if (d.CtlID>=kExtensionButtonBase && d.CtlID-kExtensionButtonBase<extensions_.size()) {
    const auto& e=extensions_[d.CtlID-kExtensionButtonBase];icon=e.toolbarIcon;
    fallback=e.nativeTabLimit?L"×":e.name.find(L"4K")!=std::wstring::npos?L"4K":L"验";
  }
  const int size=MulDiv(20,static_cast<int>(dpi_),96);
  if(d.CtlID==kExtensionsButton) {
    const int cx=(d.rcItem.left+d.rcItem.right)/2,cy=(d.rcItem.top+d.rcItem.bottom)/2;
    const int a=MulDiv(6,static_cast<int>(dpi_),96),r=MulDiv(3,static_cast<int>(dpi_),96);
    HPEN pen=CreatePen(PS_SOLID,std::max(1,MulDiv(1,static_cast<int>(dpi_),96)),ToColorRef(palette_.text));
    HBRUSH fill=CreateSolidBrush(ToColorRef(palette_.text));auto oldPen=SelectObject(d.hDC,pen);auto oldBrush=SelectObject(d.hDC,fill);
    RoundRect(d.hDC,cx-a,cy-a,cx+a,cy+a,2,2);Ellipse(d.hDC,cx-r,cy-a-r,cx+r,cy-a+r);Ellipse(d.hDC,cx+a-r,cy-r,cx+a+r,cy+r);
    HBRUSH cut=CreateSolidBrush(ToColorRef(down?palette_.buttonHover:palette_.toolbar));SelectObject(d.hDC,cut);SelectObject(d.hDC,GetStockObject(NULL_PEN));Ellipse(d.hDC,cx-a-r,cy-r,cx-a+r,cy+r);Ellipse(d.hDC,cx-r,cy+a-r,cx+r,cy+a+r);
    SelectObject(d.hDC,oldPen);SelectObject(d.hDC,oldBrush);DeleteObject(pen);DeleteObject(fill);DeleteObject(cut);
  }
  else if(icon)DrawIconEx(d.hDC,(d.rcItem.left+d.rcItem.right-size)/2,(d.rcItem.top+d.rcItem.bottom-size)/2,icon,size,size,0,nullptr,DI_NORMAL);
  else {
    SetBkMode(d.hDC,TRANSPARENT);SetTextColor(d.hDC,ToColorRef(disabled?palette_.textMuted:palette_.text));
    HFONT font=CreateFontW(-MulDiv(13,static_cast<int>(dpi_),96),0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Microsoft YaHei UI");
    auto old=SelectObject(d.hDC,font);RECT r=d.rcItem;DrawTextW(d.hDC,fallback.c_str(),-1,&r,DT_CENTER|DT_VCENTER|DT_SINGLELINE);SelectObject(d.hDC,old);DeleteObject(font);
  }
  if(d.itemState&ODS_FOCUS){RECT r=d.rcItem;InflateRect(&r,-2,-2);DrawFocusRect(d.hDC,&r);}
}
void BrowserWindow::SetExtensionPinned(size_t index,bool pinned) {
  if(index>=extensions_.size())return;
  auto& e=extensions_[index];e.pinned=pinned;
  std::string text;json::Value config;
  if(ReadFileBytes(profileDir_+L"\\extensions.json",text)&&json::Parse(text,config)) {
    auto entries=config["extensions"];for(auto& item:entries.MutableItems())if(FromUtf8(item["path"].AsString())==e.path)item.Set("pinned",json::Value::Bool(pinned));
    config.Set("extensions",entries);WriteFileAtomic(profileDir_+L"\\extensions.json",json::Serialize(config));
  }
  SaveExtensionStatus();
}
void BrowserWindow::ShowExtensionsMenu() {
  CloseExtensionPopup();
  HMENU menu=CreatePopupMenu();
  for(size_t i=0;i<extensions_.size();++i){const auto& e=extensions_[i];HMENU sub=CreatePopupMenu();
    const UINT unavailable=(!e.enabled||FAILED(e.result)||e.id.empty())?MF_GRAYED:0;
    AppendMenuW(sub,MF_STRING|unavailable,kExtensionOpenBase+i,L"打开");
    AppendMenuW(sub,MF_STRING|(e.pinned?MF_CHECKED:0),kExtensionPinBase+i,e.pinned?L"取消固定":L"固定到工具栏");
    AppendMenuW(sub,MF_STRING|unavailable|(e.options.empty()?MF_GRAYED:0),kExtensionSettingsBase+i,L"设置");
    AppendMenuW(menu,MF_POPUP,reinterpret_cast<UINT_PTR>(sub),MenuLabel(e.name).c_str());
  }
  if(extensions_.empty())AppendMenuW(menu,MF_STRING|MF_GRAYED,0,L"暂无扩展");
  AppendMenuW(menu,MF_SEPARATOR,0,nullptr);AppendMenuW(menu,MF_STRING,kExtensionManage,L"管理扩展");
  RECT r{};GetWindowRect(extensionsButton_,&r);
  const UINT result=TrackPopupMenuEx(menu,TPM_RETURNCMD|TPM_NONOTIFY|TPM_RIGHTALIGN|TPM_TOPALIGN,r.right,r.bottom,hwnd_,nullptr);
  DestroyMenu(menu);HandleExtensionCommand(result);
}
bool BrowserWindow::HandleExtensionCommand(UINT command) {
  if(command==kExtensionsButton){ShowExtensionsMenu();return true;}
  if(command>=kExtensionButtonBase&&command-kExtensionButtonBase<extensions_.size()){ShowExtensionPopup(command-kExtensionButtonBase);return true;}
  if(command>=kExtensionPinBase&&command-kExtensionPinBase<extensions_.size()){const size_t i=command-kExtensionPinBase;SetExtensionPinned(i,!extensions_[i].pinned);return true;}
  if(command>=kExtensionSettingsBase&&command-kExtensionSettingsBase<extensions_.size()){
    const auto& e=extensions_[command-kExtensionSettingsBase];
    auto page=e.options;while(!page.empty()&&page.front()==L'/')page.erase(page.begin());
    if(e.enabled&&SUCCEEDED(e.result)&&!page.empty()&&page.find(L':')==std::wstring::npos&&page.find(L"..") == std::wstring::npos){CloseExtensionPopup();NewTab(L"chrome-extension://"+e.id+L"/"+page);}
    return true;
  }
  if(command>=kExtensionOpenBase&&command-kExtensionOpenBase<extensions_.size()){ShowExtensionPopup(command-kExtensionOpenBase);return true;}
  if(command==kExtensionManage){CloseExtensionPopup();OpenExtensions();return true;}return false;
}
void BrowserWindow::WriteExtensionToolbarState() {
  if(destroyed_||profileDir_.empty())return;
  std::string out="{\"buttons\":[";bool first=true;
  for(const auto& e:extensions_){if(!first)out+=',';first=false;
    out+="{\"id\":";json::AppendQuoted(out,ToUtf8(e.id));out+=",\"pinned\":"+std::string(e.pinned?"true":"false");
    out+=",\"visible\":"+std::string(e.button&&(GetWindowLongPtrW(e.button,GWL_STYLE)&WS_VISIBLE)?"true":"false");
    out+=",\"button\":"+std::to_string(reinterpret_cast<uintptr_t>(e.button))+"}";
  }
  out+="],\"popupOpen\":"+std::string(extensionPopupWindow_?"true":"false")+",\"popupLoaded\":"+std::string(extensionPopupLoaded_?"true":"false");
  out+=",\"popupId\":";json::AppendQuoted(out,popupExtensionIndex_<extensions_.size()?ToUtf8(extensions_[popupExtensionIndex_].id):"");
  out+=",\"popupWindow\":"+std::to_string(reinterpret_cast<uintptr_t>(extensionPopupWindow_))+"}";
  WriteFileAtomic(profileDir_+L"\\extension-toolbar-state.json",out);
}
}  // namespace jisu
