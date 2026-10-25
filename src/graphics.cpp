#include "graphics.h"

#include "metrics.h"

#include <dwmapi.h>

#include <algorithm>
#include <cmath>

namespace jisu {
namespace {

constexpr float kPi = 3.14159265358979f;

D2D1_COLOR_F ToColor(Rgb c, float alpha = 1.0f) {
  return D2D1::ColorF(c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, alpha);
}

D2D1_POINT_2F Center(const D2D1_RECT_F& r) {
  return D2D1::Point2F((r.left + r.right) * 0.5f, (r.top + r.bottom) * 0.5f);
}

bool IsTabHover(const ChromeHit& hover, int index) {
  return (hover.kind == ChromeHit::kTab || hover.kind == ChromeHit::kTabClose) && hover.index == index;
}

}  // namespace

// ---------------------------------------------------------------------------
// GraphicsWarmup

GraphicsWarmup::~GraphicsWarmup() {
  if (firstPaint_) {
    SetEvent(firstPaint_);  // 没画出首帧就退出时也让线程收尾
  }
  for (HANDLE thread : {thread_, warmThread_}) {
    if (thread) {
      WaitForSingleObject(thread, 2000);
      CloseHandle(thread);
    }
  }
  if (ready_) {
    CloseHandle(ready_);
  }
  if (firstPaint_) {
    CloseHandle(firstPaint_);
  }
}

void GraphicsWarmup::Start(const Options& options) {
  warmD3D_ = options.warmD3D != 0;
  warp_ = options.renderTarget == RenderTargetKind::kSoftware;
  ready_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  firstPaint_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  thread_ = CreateThread(nullptr, 0, &GraphicsWarmup::ThreadMain, this, 0, nullptr);
  if (!thread_) {
    Prepare();  // 建线程失败：退回 UI 线程同步准备，不做设备预热
    SetEvent(ready_);
    return;
  }
  if (warmD3D_) {
    warmThread_ = CreateThread(nullptr, 0, &GraphicsWarmup::WarmDeviceMain, this, 0, nullptr);
  }
}

void GraphicsWarmup::WaitReady() {
  if (ready_) {
    WaitForSingleObject(ready_, INFINITE);
  }
}

void GraphicsWarmup::NotifyFirstPaint() {
  if (thread_) {
    painted_ = true;
    SetEvent(firstPaint_);
    return;
  }
  DwmFlush();
  Metrics::Get().Mark("firstFramePresented");
}

DWORD WINAPI GraphicsWarmup::ThreadMain(void* param) {
  auto* self = static_cast<GraphicsWarmup*>(param);
  self->Prepare();
  SetEvent(self->ready_);
  if (WaitForSingleObject(self->firstPaint_, 60000) == WAIT_OBJECT_0 && self->painted_) {
    DwmFlush();  // 阻塞到下一次 DWM 合成：此时首帧已经上屏
    Metrics::Get().Mark("firstFramePresented");
  }
  return 0;
}

// 设备预热单独一个线程：UI 线程不等它，它只让 WARP / 显卡驱动的 DLL 加载与一次性初始化提前开始，
// 和建窗、建渲染目标重叠；最坏情况是没收益，不会拖慢首帧，也不耽误上面那个线程按时 DwmFlush 打点
DWORD WINAPI GraphicsWarmup::WarmDeviceMain(void* param) {
  auto* self = static_cast<GraphicsWarmup*>(param);
  static constexpr D3D_FEATURE_LEVEL kLevels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                                                  D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
  Metrics::Get().Mark("d3dWarmBegin");
  ComPtr<ID3D11Device> device;
  D3D11CreateDevice(nullptr, self->warp_ ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE, nullptr,
                    D3D11_CREATE_DEVICE_BGRA_SUPPORT, kLevels, static_cast<UINT>(ARRAYSIZE(kLevels)),
                    D3D11_SDK_VERSION, device.GetAddressOf(), nullptr, nullptr);
  Metrics::Get().Mark("d3dWarmReady");
  WaitForSingleObject(self->firstPaint_, 60000);  // 首帧画完再放掉预热设备
  return 0;
}

bool GraphicsWarmup::Prepare() {
  Metrics& metrics = Metrics::Get();
  metrics.Mark("warmupBegin");
  HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.GetAddressOf());
  metrics.Mark("d2dFactoryReady");
  if (SUCCEEDED(hr)) {
    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                             reinterpret_cast<IUnknown**>(dwrite.GetAddressOf()));
  }
  if (SUCCEEDED(hr)) {
    hr = CreateTextFormats();
  }
  if (SUCCEEDED(hr)) {
    // 先排一次版：把标签标题用的中文字体和整形引擎载入，首帧时不再现找
    static constexpr wchar_t kSample[] = L"新标签页 New Tab 极速浏览器";
    ComPtr<IDWriteTextLayout> layout;
    if (SUCCEEDED(dwrite->CreateTextLayout(kSample, static_cast<UINT32>(ARRAYSIZE(kSample) - 1), tabTitle.Get(),
                                           400.0f, 40.0f, layout.GetAddressOf()))) {
      DWRITE_TEXT_METRICS textMetrics{};
      layout->GetMetrics(&textMetrics);
    }
  }
  metrics.Mark("dwriteReady");
  return SUCCEEDED(hr);
}

HRESULT GraphicsWarmup::CreateTextFormats() {
  HRESULT hr = dwrite->CreateTextFormat(L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.5f, L"zh-CN",
                                        tabTitle.GetAddressOf());
  if (FAILED(hr)) {
    return hr;
  }
  tabTitle->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
  tabTitle->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
  const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
  ComPtr<IDWriteInlineObject> ellipsis;
  if (SUCCEEDED(dwrite->CreateEllipsisTrimmingSign(tabTitle.Get(), ellipsis.GetAddressOf()))) {
    tabTitle->SetTrimming(&trimming, ellipsis.Get());
  }

  hr = dwrite->CreateTextFormat(L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                DWRITE_FONT_STRETCH_NORMAL, 14.0f, L"zh-CN", message.GetAddressOf());
  if (SUCCEEDED(hr)) {
    message->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    message->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
  }
  return hr;
}

// ---------------------------------------------------------------------------
// ChromePainter

HRESULT ChromePainter::CreateTarget(const GraphicsWarmup& graphics, HWND hwnd, RenderTargetKind kind, UINT dpi) {
  factory_ = graphics.d2d;
  dwrite_ = graphics.dwrite;
  tabTitle_ = graphics.tabTitle;
  message_ = graphics.message;
  hwnd_ = hwnd;
  kind_ = kind;
  dpi_ = dpi;
  return EnsureTarget();
}

HRESULT ChromePainter::EnsureTarget() {
  if (target_) {
    return S_OK;
  }
  if (!factory_ || !hwnd_) {
    return E_UNEXPECTED;
  }
  RECT client{};
  GetClientRect(hwnd_, &client);
  const D2D1_RENDER_TARGET_PROPERTIES properties = D2D1::RenderTargetProperties(
      kind_ == RenderTargetKind::kSoftware ? D2D1_RENDER_TARGET_TYPE_SOFTWARE : D2D1_RENDER_TARGET_TYPE_DEFAULT,
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), static_cast<float>(dpi_),
      static_cast<float>(dpi_));
  // IMMEDIATELY：EndDraw 不等垂直同步，首帧尽早交给 DWM
  const D2D1_HWND_RENDER_TARGET_PROPERTIES hwndProperties = D2D1::HwndRenderTargetProperties(
      hwnd_,
      D2D1::SizeU(static_cast<UINT32>(client.right - client.left), static_cast<UINT32>(client.bottom - client.top)),
      D2D1_PRESENT_OPTIONS_IMMEDIATELY);
  HRESULT hr = factory_->CreateHwndRenderTarget(properties, hwndProperties, target_.ReleaseAndGetAddressOf());
  if (SUCCEEDED(hr)) {
    hr = target_->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black), brush_.ReleaseAndGetAddressOf());
  }
  if (FAILED(hr)) {
    target_.Reset();
    brush_.Reset();
  }
  return hr;
}

HRESULT ChromePainter::EnsureGeometry() {
  if (reloadArc_) {
    return S_OK;
  }
  if (!factory_) {
    return E_UNEXPECTED;
  }
  HRESULT hr = factory_->CreateStrokeStyle(
      D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                  D2D1_LINE_JOIN_ROUND),
      nullptr, 0, roundStroke_.ReleaseAndGetAddressOf());
  if (FAILED(hr)) {
    return hr;
  }

  // 刷新图标：原点为圆心、半径 6 的 270° 弧，缺口朝上；箭头在弧的末端（左上），指向顺时针方向
  constexpr float radius = 6.0f;
  const float startAngle = -0.25f * kPi;
  const float endAngle = 1.25f * kPi;
  const D2D1_POINT_2F start = D2D1::Point2F(radius * cosf(startAngle), radius * sinf(startAngle));
  const D2D1_POINT_2F end = D2D1::Point2F(radius * cosf(endAngle), radius * sinf(endAngle));

  ComPtr<ID2D1PathGeometry> arc;
  ComPtr<ID2D1GeometrySink> sink;
  hr = factory_->CreatePathGeometry(arc.GetAddressOf());
  if (SUCCEEDED(hr)) {
    hr = arc->Open(sink.GetAddressOf());
  }
  if (SUCCEEDED(hr)) {
    sink->BeginFigure(start, D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddArc(D2D1::ArcSegment(end, D2D1::SizeF(radius, radius), 0.0f, D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                  D2D1_ARC_SIZE_LARGE));
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    hr = sink->Close();
  }
  if (FAILED(hr)) {
    return hr;
  }

  const D2D1_POINT_2F tangent = D2D1::Point2F(-sinf(endAngle), cosf(endAngle));
  const D2D1_POINT_2F normal = D2D1::Point2F(cosf(endAngle), sinf(endAngle));
  const D2D1_POINT_2F tip = D2D1::Point2F(end.x + 3.2f * tangent.x, end.y + 3.2f * tangent.y);
  const D2D1_POINT_2F outer = D2D1::Point2F(end.x + 2.6f * normal.x, end.y + 2.6f * normal.y);
  const D2D1_POINT_2F inner = D2D1::Point2F(end.x - 2.6f * normal.x, end.y - 2.6f * normal.y);
  ComPtr<ID2D1PathGeometry> head;
  sink.Reset();
  hr = factory_->CreatePathGeometry(head.GetAddressOf());
  if (SUCCEEDED(hr)) {
    hr = head->Open(sink.GetAddressOf());
  }
  if (SUCCEEDED(hr)) {
    sink->BeginFigure(tip, D2D1_FIGURE_BEGIN_FILLED);
    sink->AddLine(outer);
    sink->AddLine(inner);
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    hr = sink->Close();
  }
  if (FAILED(hr)) {
    return hr;
  }
  reloadArc_ = arc;
  reloadHead_ = head;

  // 书签星标：外径 7、内径 3.1 的五角星，原点为中心，尖朝上
  ComPtr<ID2D1PathGeometry> star;
  sink.Reset();
  hr = factory_->CreatePathGeometry(star.GetAddressOf());
  if (SUCCEEDED(hr)) {
    hr = star->Open(sink.GetAddressOf());
  }
  if (SUCCEEDED(hr)) {
    for (int i = 0; i < 10; ++i) {
      const float r = (i % 2 == 0) ? 7.0f : 3.1f;
      const float a = -0.5f * kPi + static_cast<float>(i) * kPi / 5.0f;
      const D2D1_POINT_2F p = D2D1::Point2F(r * cosf(a), r * sinf(a));
      if (i == 0) {
        sink->BeginFigure(p, D2D1_FIGURE_BEGIN_FILLED);
      } else {
        sink->AddLine(p);
      }
    }
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    hr = sink->Close();
  }
  if (SUCCEEDED(hr)) {
    star_ = star;
  }
  return S_OK;
}

void ChromePainter::Resize(UINT width, UINT height) {
  if (target_) {
    target_->Resize(D2D1::SizeU(width, height));
  }
}

void ChromePainter::SetDpi(UINT dpi) {
  dpi_ = dpi;
  if (target_) {
    target_->SetDpi(static_cast<float>(dpi), static_cast<float>(dpi));
  }
}

bool ChromePainter::IsHardware() const {
  return target_ && target_->IsSupported(D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_HARDWARE));
}

void ChromePainter::SetColor(Rgb color, float alpha) { brush_->SetColor(ToColor(color, alpha)); }

void ChromePainter::Fill(const D2D1_RECT_F& rect, Rgb color) {
  SetColor(color);
  target_->FillRectangle(rect, brush_.Get());
}

void ChromePainter::FillRounded(const D2D1_RECT_F& rect, float radius, Rgb color) {
  SetColor(color);
  target_->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush_.Get());
}

void ChromePainter::Stroke(D2D1_POINT_2F from, D2D1_POINT_2F to, float width) {
  target_->DrawLine(from, to, brush_.Get(), width, roundStroke_.Get());
}

void ChromePainter::Paint(const Palette& palette, const ChromeLayout& layout, const ChromeState& state) {
  if (FAILED(EnsureTarget())) {
    return;
  }
  EnsureGeometry();
  target_->BeginDraw();
  target_->SetTransform(D2D1::Matrix3x2F::Identity());
  target_->Clear(ToColor(palette.content));

  const float width = layout.width;
  const float stripBottom = layout.tabStripHeight;
  const float toolbarBottom = static_cast<float>(layout.separatorPx) / layout.scale;  // 含书签栏

  // 标签条：当前标签与工具栏同色并向下延伸，两者连成一体
  Fill(D2D1::RectF(0, 0, width, stripBottom), palette.frame);
  for (size_t i = 0; i < layout.tabs.size() && i < state.tabs.size(); ++i) {
    DrawTab(palette, layout, state, i);
  }
  if (state.hover.kind == ChromeHit::kNewTab) {
    FillRounded(layout.newTab, 6.0f, palette.tabHover);
  }
  SetColor(palette.text);
  const D2D1_POINT_2F plus = Center(layout.newTab);
  Stroke(D2D1::Point2F(plus.x - 5.0f, plus.y), D2D1::Point2F(plus.x + 5.0f, plus.y));
  Stroke(D2D1::Point2F(plus.x, plus.y - 5.0f), D2D1::Point2F(plus.x, plus.y + 5.0f));

  // Window controls share the tab strip; no duplicate native caption row.
  for (const auto& button : {std::pair{layout.minimize, ChromeHit::kMinimize},
                             std::pair{layout.maximize, ChromeHit::kMaximize},
                             std::pair{layout.windowClose, ChromeHit::kWindowClose}}) {
    if (state.hover.kind == button.second) Fill(button.first, button.second == ChromeHit::kWindowClose ? Rgb{196, 43, 28} : palette.buttonHover);
    SetColor(palette.text);
    const auto c = Center(button.first);
    if (button.second == ChromeHit::kMinimize) Stroke(D2D1::Point2F(c.x - 5, c.y), D2D1::Point2F(c.x + 5, c.y), 1.0f);
    else if (button.second == ChromeHit::kWindowClose) DrawCross(c, 4.5f);
    else {
      if (state.maximized) target_->DrawRectangle(D2D1::RectF(c.x - 3, c.y - 6, c.x + 6, c.y + 3), brush_.Get(), 1.0f);
      target_->DrawRectangle(D2D1::RectF(c.x - 5, c.y - 4, c.x + 4, c.y + 5), brush_.Get(), 1.0f);
    }
  }

  // 工具栏：后退、前进、刷新（加载中显示为停止）
  Fill(D2D1::RectF(0, stripBottom, width, toolbarBottom), palette.toolbar);
  DrawToolbarButton(palette, layout.back, state.hover.kind == ChromeHit::kBack, state.canBack);
  DrawArrow(Center(layout.back), true);
  DrawToolbarButton(palette, layout.forward, state.hover.kind == ChromeHit::kForward, state.canForward);
  DrawArrow(Center(layout.forward), false);
  DrawToolbarButton(palette, layout.reload, state.hover.kind == ChromeHit::kReload, true);
  if (state.loading) {
    DrawCross(Center(layout.reload), 4.5f);
  } else {
    DrawReload(Center(layout.reload));
  }

  // 地址框：底和边框在这里画，文字与光标归系统 EDIT 控件
  FillRounded(layout.omnibox, 6.0f, palette.omnibox);
  if (state.canStar) {
    if (state.hover.kind == ChromeHit::kStar) {
      FillRounded(layout.star, 5.0f, palette.buttonHover);
    }
    SetColor(state.starred ? palette.accent : palette.text);
    DrawStar(Center(layout.star), state.starred);
  }
  DrawToolbarButton(palette, layout.menu, state.hover.kind == ChromeHit::kMenu, true);
  DrawMenuDots(Center(layout.menu));
  const float hairline = 1.0f / layout.scale;
  const float inset = state.omniboxFocused ? 0.75f : hairline * 0.5f;
  const D2D1_RECT_F border = D2D1::RectF(layout.omnibox.left + inset, layout.omnibox.top + inset,
                                         layout.omnibox.right - inset, layout.omnibox.bottom - inset);
  SetColor(state.omniboxFocused ? palette.accent : palette.omniboxBorder);
  target_->DrawRoundedRectangle(D2D1::RoundedRect(border, 6.0f, 6.0f), brush_.Get(),
                                state.omniboxFocused ? 1.5f : hairline);

  if (layout.bookmarkBarHeight > 0) {
    DrawBookmarkBar(palette, layout, state);
  }

  // 工具栏（及书签栏）与网页区之间 1 物理像素的分隔线
  Fill(D2D1::RectF(0, toolbarBottom, width, toolbarBottom + hairline), palette.separator);

  if (!state.message.empty() && message_) {
    const D2D1_SIZE_F size = target_->GetSize();
    const float top = static_cast<float>(layout.contentTopPx) / layout.scale;
    SetColor(palette.textMuted);
    target_->DrawText(state.message.c_str(), static_cast<UINT32>(state.message.size()), message_.Get(),
                      D2D1::RectF(24.0f, top, size.width - 24.0f, size.height), brush_.Get());
  }

  if (target_->EndDraw() == D2DERR_RECREATE_TARGET) {
    target_.Reset();
    brush_.Reset();
    faviconBitmaps_.clear();
  }
}

void ChromePainter::DrawTab(const Palette& palette, const ChromeLayout& layout, const ChromeState& state,
                            size_t index) {
  const D2D1_RECT_F r = layout.tabs[index];
  const ChromeTab& tab = state.tabs[index];
  const int i = static_cast<int>(index);
  const bool hovered = IsTabHover(state.hover, i);
  if (tab.active) {
    FillRounded(D2D1::RectF(r.left, r.top, r.right, layout.tabStripHeight + 8.0f), 8.0f, palette.toolbar);
  } else if (hovered) {
    FillRounded(D2D1::RectF(r.left + 2.0f, r.top + 2.0f, r.right - 2.0f, r.bottom - 4.0f), 6.0f, palette.tabHover);
  } else if (index + 1 < state.tabs.size() && !state.tabs[index + 1].active && !IsTabHover(state.hover, i + 1)) {
    // 相邻两个普通标签之间的细竖线
    const float x = std::floor(r.right * layout.scale) / layout.scale;
    Fill(D2D1::RectF(x, r.top + 9.0f, x + 1.0f / layout.scale, r.bottom - 11.0f), palette.separator);
  }

  float textLeft = r.left + 12.0f;
  const D2D1_POINT_2F dot = D2D1::Point2F(r.left + 15.0f, (r.top + r.bottom) * 0.5f - 1.0f);
  if (tab.agent) {
    // 智能体标签：空心圈；正在执行命令或加载中：实心
    SetColor(palette.accent);
    if (tab.agentAwake || tab.loading) {
      target_->FillEllipse(D2D1::Ellipse(dot, 4.0f, 4.0f), brush_.Get());
    } else {
      target_->DrawEllipse(D2D1::Ellipse(dot, 3.5f, 3.5f), brush_.Get(), 1.5f);
    }
    textLeft += 12.0f;
  } else if (tab.loading) {
    SetColor(palette.accent);
    target_->FillEllipse(D2D1::Ellipse(dot, 3.0f, 3.0f), brush_.Get());
    textLeft += 12.0f;
  } else if (DrawFavicon(tab.favicon, dot, 16.0f)) {
    textLeft += 22.0f;
  }
  const bool showClose = tab.active || hovered;
  const D2D1_RECT_F close = layout.tabCloses[index];
  const float textRight = showClose ? close.left - 2.0f : r.right - 10.0f;
  if (textRight > textLeft && tabTitle_) {
    SetColor(tab.active ? palette.text : palette.textMuted);
    target_->DrawText(tab.title.c_str(), static_cast<UINT32>(tab.title.size()), tabTitle_.Get(),
                      D2D1::RectF(textLeft, r.top, textRight, r.bottom - 2.0f), brush_.Get(),
                      D2D1_DRAW_TEXT_OPTIONS_CLIP);
  }
  if (showClose) {
    const D2D1_POINT_2F c = Center(close);
    if (state.hover.kind == ChromeHit::kTabClose && state.hover.index == i) {
      SetColor(palette.buttonHover);
      target_->FillEllipse(D2D1::Ellipse(c, 8.0f, 8.0f), brush_.Get());
    }
    SetColor(tab.active ? palette.text : palette.textMuted);
    DrawCross(c, 3.5f);
  }
}

void ChromePainter::DrawToolbarButton(const Palette& palette, const D2D1_RECT_F& rect, bool hovered, bool enabled) {
  if (hovered && enabled) {
    FillRounded(rect, 6.0f, palette.buttonHover);
  }
  SetColor(enabled ? palette.text : palette.textMuted, enabled ? 1.0f : 0.45f);
}

void ChromePainter::DrawArrow(D2D1_POINT_2F center, bool left) {
  const float s = left ? 1.0f : -1.0f;
  const D2D1_POINT_2F tip = D2D1::Point2F(center.x - 6.0f * s, center.y);
  Stroke(tip, D2D1::Point2F(center.x + 6.0f * s, center.y));
  Stroke(tip, D2D1::Point2F(center.x - 1.0f * s, center.y - 5.0f));
  Stroke(tip, D2D1::Point2F(center.x - 1.0f * s, center.y + 5.0f));
}

void ChromePainter::DrawCross(D2D1_POINT_2F center, float half) {
  Stroke(D2D1::Point2F(center.x - half, center.y - half), D2D1::Point2F(center.x + half, center.y + half));
  Stroke(D2D1::Point2F(center.x - half, center.y + half), D2D1::Point2F(center.x + half, center.y - half));
}

float ChromePainter::MeasureBookmark(const std::wstring& title, bool folder) const {
  constexpr float kPadding = 8.0f;  // 左右各一份
  constexpr float kIcon = 20.0f;    // 图标 14 + 间距 6
  constexpr float kMaxText = 150.0f;
  if (title.empty()) {
    return kPadding * 2.0f + 14.0f;  // 只有图标
  }
  float textWidth = 7.5f * static_cast<float>(title.size());  // 估算：还没有排版对象时
  if (dwrite_ && tabTitle_) {
    ComPtr<IDWriteTextLayout> textLayout;
    if (SUCCEEDED(dwrite_->CreateTextLayout(title.c_str(), static_cast<UINT32>(title.size()), tabTitle_.Get(), 1000.0f,
                                            30.0f, textLayout.GetAddressOf()))) {
      DWRITE_TEXT_METRICS metrics{};
      if (SUCCEEDED(textLayout->GetMetrics(&metrics))) {
        textWidth = metrics.widthIncludingTrailingWhitespace;
      }
    }
  }
  (void)folder;  // 文件夹与网页图标同宽
  return kPadding * 2.0f + kIcon + std::min(textWidth, kMaxText);
}

void ChromePainter::DrawBookmarkBar(const Palette& palette, const ChromeLayout& layout, const ChromeState& state) {
  for (size_t i = 0; i < layout.bookmarks.size() && i < state.bookmarks.size(); ++i) {
    const D2D1_RECT_F r = layout.bookmarks[i];
    const ChromeBookmark& item = state.bookmarks[i];
    if (state.hover.kind == ChromeHit::kBookmark && state.hover.index == static_cast<int>(i)) {
      FillRounded(r, 5.0f, palette.buttonHover);
    }
    if (state.bookmarkDrag && state.bookmarkDropIntoFolder && state.bookmarkDrop == static_cast<int>(i)) {
      FillRounded(r, 5.0f, palette.buttonHover);
      SetColor(palette.accent);
      target_->DrawRoundedRectangle(D2D1::RoundedRect(r,5.0f,5.0f), brush_.Get(), 1.5f);
    }
    const D2D1_POINT_2F icon = D2D1::Point2F(r.left + 8.0f + 7.0f, (r.top + r.bottom) * 0.5f);
    SetColor(palette.textMuted);
    if (item.folder) {
      DrawFolderGlyph(icon);
    } else if (!DrawFavicon(item.favicon, icon, 16.0f)) {
      DrawPageGlyph(icon);
    }
    if (!item.title.empty() && tabTitle_) {
      SetColor(palette.text);
      target_->DrawText(item.title.c_str(), static_cast<UINT32>(item.title.size()), tabTitle_.Get(),
                        D2D1::RectF(r.left + 8.0f + 20.0f, r.top, r.right - 8.0f, r.bottom), brush_.Get(),
                        D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
  }
  if (state.bookmarkDrag && !state.bookmarkDropIntoFolder && state.bookmarkDrop >= 0 && !layout.bookmarks.empty()) {
    const auto index = static_cast<size_t>(state.bookmarkDrop);
    const float x = index < layout.bookmarks.size() ? layout.bookmarks[index].left - 1 : layout.bookmarks.back().right + 1;
    const auto& r = layout.bookmarks.front();
    SetColor(palette.accent);
    Stroke(D2D1::Point2F(x,r.top+2),D2D1::Point2F(x,r.bottom-2),2.0f);
  }
  if (layout.hasOverflow) {
    const D2D1_RECT_F r = layout.bookmarkOverflow;
    if (state.hover.kind == ChromeHit::kBookmarkOverflow) {
      FillRounded(r, 5.0f, palette.buttonHover);
    }
    SetColor(palette.text);
    const D2D1_POINT_2F c = Center(r);
    for (const float dx : {-3.0f, 2.0f}) {
      Stroke(D2D1::Point2F(c.x + dx - 2.0f, c.y - 4.0f), D2D1::Point2F(c.x + dx + 2.0f, c.y), 1.3f);
      Stroke(D2D1::Point2F(c.x + dx + 2.0f, c.y), D2D1::Point2F(c.x + dx - 2.0f, c.y + 4.0f), 1.3f);
    }
  }
}

bool ChromePainter::DrawFavicon(const std::wstring& file, D2D1_POINT_2F center, float size) {
  if (file.empty() || !target_) return false;
  auto found = faviconBitmaps_.find(file);
  if (found == faviconBitmaps_.end()) {
    if (!wic_) CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wic_.GetAddressOf()));
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    ComPtr<ID2D1Bitmap> bitmap;
    if (wic_ && SUCCEEDED(wic_->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, decoder.GetAddressOf())) &&
        SUCCEEDED(decoder->GetFrame(0, frame.GetAddressOf())) && SUCCEEDED(wic_->CreateFormatConverter(converter.GetAddressOf())) &&
        SUCCEEDED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) {
      target_->CreateBitmapFromWicBitmap(converter.Get(), nullptr, bitmap.GetAddressOf());
    }
    if (faviconBitmaps_.size() >= 512) faviconBitmaps_.clear();
    found = faviconBitmaps_.emplace(file, bitmap).first;
  }
  if (!found->second) return false;
  target_->DrawBitmap(found->second.Get(), D2D1::RectF(center.x - size/2, center.y - size/2, center.x + size/2, center.y + size/2),
                      1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
  return true;
}

void ChromePainter::DrawStar(D2D1_POINT_2F center, bool filled) {
  if (!star_) {
    return;
  }
  target_->SetTransform(D2D1::Matrix3x2F::Translation(center.x, center.y));
  if (filled) {
    target_->FillGeometry(star_.Get(), brush_.Get());
  } else {
    target_->DrawGeometry(star_.Get(), brush_.Get(), 1.3f, roundStroke_.Get());
  }
  target_->SetTransform(D2D1::Matrix3x2F::Identity());
}

void ChromePainter::DrawMenuDots(D2D1_POINT_2F center) {
  for (const float dy : {-5.0f, 0.0f, 5.0f}) {
    target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(center.x, center.y + dy), 1.6f, 1.6f), brush_.Get());
  }
}

void ChromePainter::DrawFolderGlyph(D2D1_POINT_2F c) {
  // 文件夹：带页签的圆角矩形轮廓
  const D2D1_RECT_F body = D2D1::RectF(c.x - 7.0f, c.y - 3.5f, c.x + 7.0f, c.y + 5.5f);
  target_->DrawRoundedRectangle(D2D1::RoundedRect(body, 1.5f, 1.5f), brush_.Get(), 1.2f);
  Stroke(D2D1::Point2F(c.x - 7.0f, c.y - 3.5f), D2D1::Point2F(c.x - 7.0f, c.y - 5.5f), 1.2f);
  Stroke(D2D1::Point2F(c.x - 7.0f, c.y - 5.5f), D2D1::Point2F(c.x - 2.0f, c.y - 5.5f), 1.2f);
  Stroke(D2D1::Point2F(c.x - 2.0f, c.y - 5.5f), D2D1::Point2F(c.x - 0.5f, c.y - 3.5f), 1.2f);
}

void ChromePainter::DrawPageGlyph(D2D1_POINT_2F c) {
  // 网页：地球轮廓（一圈 + 一道经线 + 一道纬线）
  target_->DrawEllipse(D2D1::Ellipse(c, 6.0f, 6.0f), brush_.Get(), 1.2f);
  target_->DrawEllipse(D2D1::Ellipse(c, 2.6f, 6.0f), brush_.Get(), 1.0f);
  Stroke(D2D1::Point2F(c.x - 6.0f, c.y), D2D1::Point2F(c.x + 6.0f, c.y), 1.0f);
}

void ChromePainter::DrawReload(D2D1_POINT_2F center) {
  if (!reloadArc_) {
    return;
  }
  target_->SetTransform(D2D1::Matrix3x2F::Translation(center.x, center.y));
  target_->DrawGeometry(reloadArc_.Get(), brush_.Get(), 1.5f, roundStroke_.Get());
  target_->FillGeometry(reloadHead_.Get(), brush_.Get());
  target_->SetTransform(D2D1::Matrix3x2F::Identity());
}

}  // namespace jisu
