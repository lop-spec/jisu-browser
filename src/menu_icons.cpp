#include "menu_icons.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace jisu {
BookmarkMenuIcons::BookmarkMenuIcons(UINT dpi) : size_(std::clamp(MulDiv(16, static_cast<int>(dpi), 96), 16, 64)) {}
BookmarkMenuIcons::~BookmarkMenuIcons() { for (HBITMAP bitmap : owned_) DeleteObject(bitmap); }
HBITMAP BookmarkMenuIcons::MakeBitmap(const std::vector<BYTE>& bgra) {
  BITMAPINFO info{};
  info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth = size_;
  info.bmiHeader.biHeight = -size_;  // WIC and native popup menus use top-down premultiplied BGRA.
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!bitmap || !bits) { if (bitmap) DeleteObject(bitmap); return nullptr; }
  std::memcpy(bits, bgra.data(), bgra.size());
  owned_.push_back(bitmap);
  return bitmap;
}
HBITMAP BookmarkMenuIcons::Decode(const std::wstring& file) {
  if (file.empty()) return nullptr;
  auto found = images_.find(file);
  if (found != images_.end()) return found->second;
  if (!wic_) CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wic_.GetAddressOf()));
  ComPtr<IWICBitmapDecoder> decoder;
  ComPtr<IWICBitmapFrameDecode> frame;
  ComPtr<IWICBitmapScaler> scaler;
  ComPtr<IWICFormatConverter> converter;
  UINT width = 0, height = 0;
  HBITMAP bitmap = nullptr;
  if (wic_ && SUCCEEDED(wic_->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, decoder.GetAddressOf())) &&
      SUCCEEDED(decoder->GetFrame(0, frame.GetAddressOf())) && SUCCEEDED(frame->GetSize(&width, &height)) &&
      width > 0 && height > 0 && width <= 4096 && height <= 4096 &&
      SUCCEEDED(wic_->CreateBitmapScaler(scaler.GetAddressOf())) &&
      SUCCEEDED(scaler->Initialize(frame.Get(), size_, size_, WICBitmapInterpolationModeFant)) &&
      SUCCEEDED(wic_->CreateFormatConverter(converter.GetAddressOf())) &&
      SUCCEEDED(converter->Initialize(scaler.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) {
    std::vector<BYTE> pixels(static_cast<size_t>(size_) * size_ * 4);
    if (SUCCEEDED(converter->CopyPixels(nullptr, static_cast<UINT>(size_ * 4), static_cast<UINT>(pixels.size()), pixels.data()))) bitmap = MakeBitmap(pixels);
  }
  images_.emplace(file, bitmap);
  return bitmap;
}
HBITMAP BookmarkMenuIcons::Fallback(bool folder) {
  const std::wstring key = folder ? L":folder" : L":site";
  auto found = images_.find(key);
  if (found != images_.end()) return found->second;
  std::vector<BYTE> pixels(static_cast<size_t>(size_) * size_ * 4);
  // Supersampled transparent glyphs, not blank spaces for unknown website icons.
  for (int y = 0; y < size_; ++y) for (int x = 0; x < size_; ++x) {
    int covered = 0;
    for (int sy = 0; sy < 4; ++sy) for (int sx = 0; sx < 4; ++sx) {
      const double px = (x + (sx + 0.5) / 4) * 16 / size_, py = (y + (sy + 0.5) / 4) * 16 / size_;
      const double dx = px - 8, dy = py - 8, radius = std::sqrt(dx * dx + dy * dy);
      const double meridian = dx * dx / (2.6 * 2.6) + dy * dy / 36;
      bool inside;
      if (folder) {
        const bool body = px >= 1.0 && px <= 15.0 && py >= 5.0 && py <= 13.8;
        const bool middle = px > 2.2 && px < 13.8 && py > 6.2 && py < 12.6;
        const bool tab = px >= 1.0 && px <= 7.0 && py >= 2.8 && py <= 5.5;
        const bool tabInside = px > 2.2 && px < 5.8 && py > 4;
        inside = (body && !middle) || (tab && !tabInside);
      } else inside = (radius >= 5.35 && radius <= 6.65) ||
                       (radius < 6 && std::abs(dy) < .6) || (radius < 6 && std::abs(meridian - 1) < .28);
      if (inside) ++covered;
    }
    const BYTE alpha = static_cast<BYTE>((covered * 255 + 8) / 16);
    const size_t o = (static_cast<size_t>(y) * size_ + x) * 4;
    for (int c = 0; c < 3; ++c) pixels[o + c] = static_cast<BYTE>((112 * alpha + 127) / 255);
    pixels[o + 3] = alpha;
  }
  HBITMAP bitmap = MakeBitmap(pixels);
  images_.emplace(key, bitmap);
  return bitmap;
}
bool BookmarkMenuIcons::Append(HMENU menu, UINT flags, UINT_PTR command, const std::wstring& text,
                               const std::wstring& image, bool folder) {
  if (!AppendMenuW(menu, flags, command, text.c_str())) return false;
  HBITMAP bitmap = folder ? nullptr : Decode(image);
  const bool site = bitmap != nullptr;
  if (!bitmap) bitmap = Fallback(folder);
  if (!bitmap) return false;
  MENUINFO style{sizeof(MENUINFO)};
  style.fMask = MIM_STYLE;
  if (GetMenuInfo(menu, &style)) { style.dwStyle |= MNS_CHECKORBMP; SetMenuInfo(menu, &style); }
  MENUITEMINFOW item{sizeof(MENUITEMINFOW)};
  item.fMask = MIIM_BITMAP;
  item.hbmpItem = bitmap;
  const BOOL ok = SetMenuItemInfoW(menu, GetMenuItemCount(menu) - 1, TRUE, &item);
  if (ok) { if (site) ++siteImages_; else ++fallbackImages_; }
  return ok != FALSE;
}
}  // namespace jisu
