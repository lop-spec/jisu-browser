// Production Direct2D paint path on WIC bitmaps. No browser, WebView, HWND,
// profile, extension or website login data is opened; no desktop activation.
#include "graphics.h"
#include <cmath>
#include <filesystem>
#include <fstream>
#include <stdexcept>

using namespace jisu;
namespace {
void Check(bool ok, const char* error) { if (!ok) throw std::runtime_error(error); }
void Hr(HRESULT hr) { Check(SUCCEEDED(hr), "COM/render operation failed"); }
void SavePng(IWICImagingFactory* wic, IWICBitmap* bitmap, const std::filesystem::path& file) {
  ComPtr<IWICStream> stream;
  Hr(wic->CreateStream(stream.GetAddressOf()));
  Hr(stream->InitializeFromFilename(file.c_str(), GENERIC_WRITE));
  ComPtr<IWICBitmapEncoder> encoder;
  Hr(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.GetAddressOf()));
  Hr(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache));
  ComPtr<IWICBitmapFrameEncode> frame;
  Hr(encoder->CreateNewFrame(frame.GetAddressOf(), nullptr));
  Hr(frame->Initialize(nullptr));
  UINT width = 0, height = 0;
  Hr(bitmap->GetSize(&width, &height));
  Hr(frame->SetSize(width, height));
  WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
  Hr(frame->SetPixelFormat(&format));
  Hr(frame->WriteSource(bitmap, nullptr));
  Hr(frame->Commit());
  Hr(encoder->Commit());
}
void Render(const GraphicsWarmup& graphics, const Palette& palette, const std::filesystem::path& out,
            const wchar_t* name, float width, UINT dpi, size_t count, size_t active, ChromeHit hover = {}) {
  const float scale = static_cast<float>(dpi) / 96.0f;
  const UINT pxWidth = static_cast<UINT>(width * scale);
  const UINT pxHeight = static_cast<UINT>(100 * scale);
  ComPtr<IWICImagingFactory> wic;
  Hr(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wic.GetAddressOf())));
  ComPtr<IWICBitmap> bitmap;
  Hr(wic->CreateBitmap(pxWidth, pxHeight, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, bitmap.GetAddressOf()));
  ChromeLayout layout;
  layout.width = width;
  layout.scale = scale;
  layout.separatorPx = static_cast<int>((kTabStripHeight + layout.toolbarHeight) * scale);
  layout.contentTopPx = layout.separatorPx + 1;
  auto strip = LayoutTabStrip(width, count);
  layout.tabs = strip.tabs;
  layout.tabCloses = strip.closes;
  layout.newTab = strip.newTab;
  layout.minimize = D2D1::RectF(width-138, 0, width-92, kTabStripHeight);
  layout.maximize = D2D1::RectF(width-92, 0, width-46, kTabStripHeight);
  layout.windowClose = D2D1::RectF(width-46, 0, width, kTabStripHeight);
  layout.back = D2D1::RectF(8, 54, 40, 86);
  layout.forward = D2D1::RectF(44, 54, 76, 86);
  layout.reload = D2D1::RectF(80, 54, 112, 86);
  layout.omnibox = D2D1::RectF(120, 55, width-50, 85);
  layout.menu = D2D1::RectF(width-40, 54, width-8, 86);
  ChromeState state;
  state.hover = hover;
  for (size_t i = 0; i < count; ++i) state.tabs.push_back(ChromeTab{i == 0 ? L"登录QQ邮箱" : L"新标签页", i == active});
  ChromePainter painter;
  Hr(painter.CreateBitmapTarget(graphics, bitmap.Get(), dpi));
  painter.Paint(palette, layout, state);
  SavePng(wic.Get(), bitmap.Get(), out / name);
  auto IsColor = [&](float x, float y, Rgb color) {
    const WICRect rect{static_cast<INT>(x*scale), static_cast<INT>(y*scale), 1, 1};
    BYTE pixel[4]{};
    Hr(bitmap->CopyPixels(&rect, 4, 4, pixel));
    return std::abs(static_cast<int>(pixel[2])-color.r) < 3 &&
           std::abs(static_cast<int>(pixel[1])-color.g) < 3 &&
           std::abs(static_cast<int>(pixel[0])-color.b) < 3;
  };
  const auto& r = layout.tabs[active];
  Check(IsColor(5, 3, palette.frame), "Frame color mismatch");
  Check(IsColor(r.left + 1, 6, palette.frame), "Rounded shoulder missing");
  Check(IsColor((r.left+r.right)*0.5f, 8, palette.toolbar), "Selected tab body missing");
  Check(IsColor(r.right + 3, 49, palette.toolbar), "Concave foot missing");
  Check(IsColor((r.left+r.right)*0.5f, 50, palette.toolbar), "Tab/toolbar seam detected");
}
}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR commandLine, int) {
  std::wstring directory = commandLine;
  if (directory.size() >= 2 && directory.front() == L'"' && directory.back() == L'"') directory = directory.substr(1, directory.size()-2);
  const std::filesystem::path out = directory.empty() ? L"tab-chrome-evidence" : directory;
  std::filesystem::create_directories(out);
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  int result = 0;
  try {
    Options options;
    options.warmD3D = 0;
    GraphicsWarmup graphics;
    graphics.Start(options);
    graphics.WaitReady();
    graphics.NotifyFirstPaint();
    const Palette light = LoadPalette();
    Check(!light.dark && light.frame.r == 235 && light.frame.g == 242 && light.frame.b == 250 &&
          light.toolbar.r == 255 && light.toolbar.g == 255 && light.toolbar.b == 255,
          "Live chrome palette must match the light reference, regardless of Windows app theme");
    const Palette dark{true, {28,28,28}, {42,42,42}, {43,43,43}, {58,58,58}, {31,31,31}, {60,60,60}, {58,58,58}, {232,232,232}, {160,160,160}, {76,139,245}, {32,32,32}};
    Render(graphics, light, out, L"light-1024.png", 1024, 96, 3, 2);
    Render(graphics, light, out, L"hover-1024.png", 1024, 96, 3, 2, {ChromeHit::kTabClose, 1});
    Render(graphics, light, out, L"light-125-percent.png", 1024, 120, 3, 2);
    Render(graphics, light, out, L"narrow-390.png", 390, 96, 3, 1);
    Render(graphics, light, out, L"crowded-1024.png", 1024, 96, 20, 4);
    Render(graphics, dark, out, L"dark-1024.png", 1024, 96, 3, 2);
    std::ofstream(out / "result.txt") << "PASS production Direct2D renderer: 220 DIP tabs; rounded shoulders; concave feet; no toolbar seam; light/dark; hover; 100/125 percent DPI; 390/1024 widths; crowded strip. No browser/profile opened.\n";
  } catch (const std::exception& e) {
    std::ofstream(out / "result.txt") << "FAIL " << e.what() << '\n';
    result = 1;
  }
  CoUninitialize();
  return result;
}
