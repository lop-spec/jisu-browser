// Native bookmark popup images. All HBITMAPs stay alive until TrackPopupMenuEx ends.
#pragma once
#include "common.h"
#include <wincodec.h>
#include <map>
#include <vector>
namespace jisu {
class BookmarkMenuIcons {
 public:
  explicit BookmarkMenuIcons(UINT dpi);
  ~BookmarkMenuIcons();
  BookmarkMenuIcons(const BookmarkMenuIcons&) = delete;
  BookmarkMenuIcons& operator=(const BookmarkMenuIcons&) = delete;
  bool Append(HMENU menu, UINT flags, UINT_PTR command, const std::wstring& text,
              const std::wstring& image, bool folder);
  size_t SiteImages() const { return siteImages_; }
  size_t FallbackImages() const { return fallbackImages_; }
 private:
  HBITMAP Decode(const std::wstring& file);
  HBITMAP Fallback(bool folder);
  HBITMAP MakeBitmap(const std::vector<BYTE>& bgra);
  int size_ = 16;
  size_t siteImages_ = 0, fallbackImages_ = 0;
  std::map<std::wstring, HBITMAP> images_;
  std::vector<HBITMAP> owned_;
  ComPtr<IWICImagingFactory> wic_;
};
}  // namespace jisu
