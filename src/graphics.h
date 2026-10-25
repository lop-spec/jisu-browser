// 标签条与工具栏的 Direct2D 绘制，以及首帧所需图形对象的后台预热。
#pragma once

#include "common.h"
#include "options.h"
#include "palette.h"

#include <d2d1.h>
#include <d3d11.h>
#include <dwrite.h>
#include <dxgiformat.h>
#include <wincodec.h>
#include <map>

#include <string>
#include <vector>

namespace jisu {

// 后台线程预先准备首帧要用的对象（D2D/DWrite 工厂、字体，以及 WARP 或显卡驱动），与建窗、发起内核环境并行。
// 这些 DLL 在链接时设为延迟加载，第一次调用发生在这个线程上，不占 UI 线程。
class GraphicsWarmup {
 public:
  GraphicsWarmup() = default;
  ~GraphicsWarmup();
  GraphicsWarmup(const GraphicsWarmup&) = delete;
  GraphicsWarmup& operator=(const GraphicsWarmup&) = delete;

  void Start(const Options& options);
  void WaitReady();
  // UI 线程画完首帧后调用：后台线程 DwmFlush 等这一帧被 DWM 合成，记 firstFramePresented。
  void NotifyFirstPaint();

  // WaitReady() 返回后只由 UI 线程使用
  ComPtr<ID2D1Factory> d2d;
  ComPtr<IDWriteFactory> dwrite;
  ComPtr<IDWriteTextFormat> tabTitle;
  ComPtr<IDWriteTextFormat> message;

 private:
  static DWORD WINAPI ThreadMain(void* param);
  static DWORD WINAPI WarmDeviceMain(void* param);
  bool Prepare();
  HRESULT CreateTextFormats();

  bool warmD3D_ = true;
  bool warp_ = true;  // 软件渲染目标走 WARP，预热也建 WARP 设备
  bool painted_ = false;  // 在 SetEvent(firstPaint_) 之前写、等到事件之后读，由事件保证可见性
  HANDLE ready_ = nullptr;
  HANDLE firstPaint_ = nullptr;
  HANDLE thread_ = nullptr;
  HANDLE warmThread_ = nullptr;
};

struct ChromeHit {
  enum Kind { kNone, kTab, kTabClose, kNewTab, kBack, kForward, kReload, kStar, kMenu, kBookmark, kBookmarkOverflow, kMinimize, kMaximize, kWindowClose };
  Kind kind = kNone;
  int index = -1;
  bool operator==(const ChromeHit&) const = default;
};

// 标签条与工具栏的布局，单位 DIP；separatorPx/contentTopPx 为物理像素，网页区从 contentTopPx 开始。
struct ChromeLayout {
  float scale = 1.0f;
  float width = 0;
  float tabStripHeight = 38;
  float toolbarHeight = 40;
  float bookmarkBarHeight = 0;  // 书签栏隐藏时为 0
  std::vector<D2D1_RECT_F> tabs;
  std::vector<D2D1_RECT_F> tabCloses;
  D2D1_RECT_F newTab{};
  D2D1_RECT_F minimize{}, maximize{}, windowClose{};
  D2D1_RECT_F back{};
  D2D1_RECT_F forward{};
  D2D1_RECT_F reload{};
  D2D1_RECT_F omnibox{};
  D2D1_RECT_F star{};  // 地址框右端的书签星标
  D2D1_RECT_F menu{};  // 工具栏最右的菜单钮
  std::vector<D2D1_RECT_F> bookmarks;  // 书签栏里放得下的项
  D2D1_RECT_F bookmarkOverflow{};       // 「»」：放不下的项收进这里
  bool hasOverflow = false;
  int separatorPx = 0;
  int contentTopPx = 0;
};

struct ChromeTab {
  std::wstring title;
  bool active = false;
  bool loading = false;
  bool agent = false;       // 智能体标签：标题前画一个圈
  bool agentAwake = false;  // 正在执行命令：圈填实
  std::wstring favicon;
};

struct ChromeBookmark {
  std::wstring title;
  bool folder = false;
  std::wstring favicon;
};

struct ChromeState {
  std::vector<ChromeTab> tabs;
  ChromeHit hover;
  bool canBack = false;
  bool canForward = false;
  bool loading = false;
  bool omniboxFocused = false;
  bool canStar = false;  // 当前页能加书签（http/https）
  bool starred = false;  // 当前页已在书签里
  std::vector<ChromeBookmark> bookmarks;  // 书签栏各项，与 ChromeLayout::bookmarks 一一对应
  bool bookmarkDrag = false;
  int bookmarkDrop = -1;
  bool bookmarkDropIntoFolder = false;
  bool maximized = false;
  std::wstring message;  // 内容区提示，例如没有 WebView2 运行时
};

class ChromePainter {
 public:
  HRESULT CreateTarget(const GraphicsWarmup& graphics, HWND hwnd, RenderTargetKind kind, UINT dpi);
  void Resize(UINT width, UINT height);
  void SetDpi(UINT dpi);
  void Paint(const Palette& palette, const ChromeLayout& layout, const ChromeState& state);
  bool IsHardware() const;
  // 书签栏一项的宽度（DIP，含内边距与图标）；渲染目标建好之前返回估算值
  float MeasureBookmark(const std::wstring& title, bool folder) const;
  void ClearFaviconBitmaps() { faviconBitmaps_.clear(); }

 private:
  HRESULT EnsureTarget();
  HRESULT EnsureGeometry();
  void SetColor(Rgb color, float alpha = 1.0f);
  void Fill(const D2D1_RECT_F& rect, Rgb color);
  void FillRounded(const D2D1_RECT_F& rect, float radius, Rgb color);
  void Stroke(D2D1_POINT_2F from, D2D1_POINT_2F to, float width = 1.5f);
  void DrawTab(const Palette& palette, const ChromeLayout& layout, const ChromeState& state, size_t index);
  void DrawToolbarButton(const Palette& palette, const D2D1_RECT_F& rect, bool hovered, bool enabled);
  void DrawArrow(D2D1_POINT_2F center, bool left);
  void DrawCross(D2D1_POINT_2F center, float half);
  void DrawReload(D2D1_POINT_2F center);
  void DrawStar(D2D1_POINT_2F center, bool filled);
  void DrawMenuDots(D2D1_POINT_2F center);
  void DrawFolderGlyph(D2D1_POINT_2F center);
  void DrawPageGlyph(D2D1_POINT_2F center);
  void DrawBookmarkBar(const Palette& palette, const ChromeLayout& layout, const ChromeState& state);
  bool DrawFavicon(const std::wstring& file, D2D1_POINT_2F center, float size = 16.0f);
  std::map<std::wstring, ComPtr<ID2D1Bitmap>> faviconBitmaps_;
  ComPtr<IWICImagingFactory> wic_;

  ComPtr<ID2D1Factory> factory_;
  ComPtr<IDWriteFactory> dwrite_;
  ComPtr<IDWriteTextFormat> tabTitle_;
  ComPtr<IDWriteTextFormat> message_;
  ComPtr<ID2D1HwndRenderTarget> target_;
  ComPtr<ID2D1SolidColorBrush> brush_;
  ComPtr<ID2D1StrokeStyle> roundStroke_;
  ComPtr<ID2D1PathGeometry> reloadArc_;
  ComPtr<ID2D1PathGeometry> reloadHead_;
  ComPtr<ID2D1PathGeometry> star_;
  HWND hwnd_ = nullptr;
  RenderTargetKind kind_ = RenderTargetKind::kHardware;
  UINT dpi_ = 96;
};

}  // namespace jisu
