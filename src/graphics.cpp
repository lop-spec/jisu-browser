#include "graphics.h"

#include "metrics.h"

#include <dwmapi.h>

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
  if (thread_) {
    SetEvent(firstPaint_);  // 没画出首帧就退出时也让线程收尾
    WaitForSingleObject(thread_, 2000);
    CloseHandle(thread_);
  }
  if (ready_) {
    CloseHandle(ready_);
  }
  if (firstPaint_) {
    CloseHandle(firstPaint_);
  }
}

void GraphicsWarmup::Start(const Options& options) {
  warmD3D_ = options.warmD3D && options.renderTarget == RenderTargetKind::kHardware;
  ready_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  firstPaint_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  thread_ = CreateThread(nullptr, 0, &GraphicsWarmup::ThreadMain, this, 0, nullptr);
  if (!thread_) {
    Prepare();  // 建线程失败：退回 UI 线程同步准备
    SetEvent(ready_);
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

void GraphicsWarmup::Prepare() {
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
  if (SUCCEEDED(hr) && warmD3D_) {
    static constexpr D3D_FEATURE_LEVEL kLevels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                                                    D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, kLevels,
                      static_cast<UINT>(ARRAYSIZE(kLevels)), D3D11_SDK_VERSION, d3dWarm.GetAddressOf(), nullptr,
                      nullptr);
    metrics.Mark("d3dWarmReady");
  }
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
  const float toolbarBottom = static_cast<float>(layout.separatorPx) / layout.scale;

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
  const float hairline = 1.0f / layout.scale;
  const float inset = state.omniboxFocused ? 0.75f : hairline * 0.5f;
  const D2D1_RECT_F border = D2D1::RectF(layout.omnibox.left + inset, layout.omnibox.top + inset,
                                         layout.omnibox.right - inset, layout.omnibox.bottom - inset);
  SetColor(state.omniboxFocused ? palette.accent : palette.omniboxBorder);
  target_->DrawRoundedRectangle(D2D1::RoundedRect(border, 6.0f, 6.0f), brush_.Get(),
                                state.omniboxFocused ? 1.5f : hairline);

  // 工具栏与网页区之间 1 物理像素的分隔线
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
  if (tab.loading) {
    SetColor(palette.accent);
    target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(r.left + 15.0f, (r.top + r.bottom) * 0.5f - 1.0f), 3.0f, 3.0f),
                         brush_.Get());
    textLeft += 12.0f;
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
