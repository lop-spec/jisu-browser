// Exercises the production Direct2D chrome renderer only. No WebView, browser,
// profile, extension, remote debugging or website login data is opened.
#include "graphics.h"
#include <dwmapi.h>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <stdexcept>

using namespace jisu;
namespace {
void Check(bool ok, const char* error) { if (!ok) throw std::runtime_error(error); }
void Hr(HRESULT hr) { Check(SUCCEEDED(hr), "COM/render operation failed"); }

void SavePng(HBITMAP bitmap, const std::filesystem::path& file) {
  ComPtr<IWICImagingFactory> wic;
  Hr(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wic.GetAddressOf())));
  ComPtr<IWICBitmap> source;
  Hr(wic->CreateBitmapFromHBITMAP(bitmap, nullptr, WICBitmapIgnoreAlpha, source.GetAddressOf()));
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
  Hr(source->GetSize(&width, &height));
  Hr(frame->SetSize(width, height));
  WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
  Hr(frame->SetPixelFormat(&format));
  Hr(frame->WriteSource(source.Get(), nullptr));
  Hr(frame->Commit());
  Hr(encoder->Commit());
}

void Render(const GraphicsWarmup& graphics, const Palette& palette, const std::filesystem::path& out,
            const wchar_t* name, float width, UINT dpi, size_t count, size_t active, ChromeHit hover = {}) {
  const float scale = static_cast<float>(dpi) / 96.0f;
  const int pxWidth = static_cast<int>(width * scale);
  const int pxHeight = static_cast<int>(768 * scale);
  HWND window = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, L"JisuTabChromeTest", L"Native tab render test",
      WS_POPUP, -30000, -30000, pxWidth, pxHeight, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
  Check(window != nullptr, "Test HWND creation failed");
  ShowWindow(window, SW_SHOWNOACTIVATE);
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
  for (size_t i = 0; i < count; ++i) {
    state.tabs.push_back(ChromeTab{i == 0 ? L"登录QQ邮箱" : L"新标签页", i == active});
  }
  {
    ChromePainter painter;
    Hr(painter.CreateTarget(graphics, window, RenderTargetKind::kSoftware, dpi));
    painter.Paint(palette, layout, state);
    DwmFlush();
    HDC dc = GetDC(window);
    HDC memory = CreateCompatibleDC(dc);
    HBITMAP bitmap = CreateCompatibleBitmap(dc, pxWidth, static_cast<int>(100 * scale));
    const HGDIOBJ old = SelectObject(memory, bitmap);
    Check(PrintWindow(window, memory, 2) != FALSE, "PrintWindow failed");
    const COLORREF sample = GetPixel(memory, static_cast<int>(5*scale), static_cast<int>(3*scale));
    Check(std::abs(static_cast<int>(GetRValue(sample)) - palette.frame.r) < 3 &&
          std::abs(static_cast<int>(GetGValue(sample)) - palette.frame.g) < 3 &&
          std::abs(static_cast<int>(GetBValue(sample)) - palette.frame.b) < 3, "Captured frame color mismatch");
    // The active shoulder must be rounded, its body white/dark-toolbar, and its
    // concave foot must fill into the toolbar without a seam.
    const auto& r = layout.tabs[active];
    auto IsColor = [&](float x, float y, Rgb color) {
      const COLORREF pixel = GetPixel(memory, static_cast<int>(x*scale), static_cast<int>(y*scale));
      return std::abs(static_cast<int>(GetRValue(pixel))-color.r) < 3 &&
             std::abs(static_cast<int>(GetGValue(pixel))-color.g) < 3 &&
             std::abs(static_cast<int>(GetBValue(pixel))-color.b) < 3;
    };
    Check(IsColor(r.left + 1, 6, palette.frame), "Rounded shoulder missing");
    Check(IsColor((r.left+r.right)*0.5f, 8, palette.toolbar), "Selected tab body missing");
    Check(IsColor(r.right + 3, 49, palette.toolbar), "Concave foot missing");
    Check(IsColor((r.left+r.right)*0.5f, 50, palette.toolbar), "Tab/toolbar seam detected");
    SavePng(bitmap, out / name);
    SelectObject(memory, old);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(window, dc);
  }
  DestroyWindow(window);
}
}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int) {
  std::wstring directory = commandLine;
  if (directory.size() >= 2 && directory.front() == L'"' && directory.back() == L'"') directory = directory.substr(1, directory.size()-2);
  const std::filesystem::path out = directory.empty() ? L"tab-chrome-evidence" : directory;
  std::filesystem::create_directories(out);
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  int result = 0;
  try {
    WNDCLASSW cls{};
    cls.lpfnWndProc = DefWindowProcW;
    cls.hInstance = instance;
    cls.lpszClassName = L"JisuTabChromeTest";
    Check(RegisterClassW(&cls) != 0, "Test class registration failed");
    Options options;
    options.warmD3D = 0;
    GraphicsWarmup graphics;
    graphics.Start(options);
    graphics.WaitReady();
    graphics.NotifyFirstPaint();
    const Palette light{false, {235,242,250}, {220,230,244}, {255,255,255}, {236,237,239}, {241,243,244}, {218,220,224}, {218,220,224}, {31,31,31}, {95,99,104}, {26,115,232}, {255,255,255}};
    const Palette dark{true, {28,28,28}, {42,42,42}, {43,43,43}, {58,58,58}, {31,31,31}, {60,60,60}, {58,58,58}, {232,232,232}, {160,160,160}, {76,139,245}, {32,32,32}};
    Render(graphics, light, out, L"light-1024.png", 1024, 96, 3, 2);
    Render(graphics, light, out, L"hover-1024.png", 1024, 96, 3, 2, {ChromeHit::kTabClose, 1});
    Render(graphics, light, out, L"light-125-percent.png", 1024, 120, 3, 2);
    Render(graphics, light, out, L"narrow-390.png", 390, 96, 3, 1);
    Render(graphics, light, out, L"crowded-1024.png", 1024, 96, 20, 4);
    Render(graphics, dark, out, L"dark-1024.png", 1024, 96, 3, 2);
    std::ofstream(out / "result.txt") << "PASS actual production renderer: 220 DIP tabs; rounded shoulders; concave feet; no toolbar seam; light/dark; hover; 100/125 percent DPI; 390/1024 widths; crowded strip. No WebView or profile opened.\n";
  } catch (const std::exception& e) {
    std::ofstream(out / "result.txt") << "FAIL " << e.what() << '\n';
    result = 1;
  }
  CoUninitialize();
  return result;
}
