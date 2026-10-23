// 外壳配色：中性面 + 一个主色。content 同时是内容区、新标签页（ntp/index.html）和 WebView 默认底色，
// 三处必须一致，否则网页出来之前会闪一下。
#pragma once

#include <windows.h>

namespace jisu {

struct Rgb {
  BYTE r;
  BYTE g;
  BYTE b;
};

struct Palette {
  bool dark;
  Rgb frame;          // 标签条（Win11 上标题栏也设成这个颜色）
  Rgb tabHover;       // 悬停的非当前标签
  Rgb toolbar;        // 工具栏与当前标签
  Rgb buttonHover;    // 工具栏按钮、当前标签关闭钮的悬停底
  Rgb omnibox;        // 地址框
  Rgb omniboxBorder;
  Rgb separator;
  Rgb text;
  Rgb textMuted;
  Rgb accent;
  Rgb content;
};

// 跟随系统「默认应用模式」（HKCU\...\Themes\Personalize\AppsUseLightTheme），与 WebView2 的判断一致。
Palette LoadPalette();

inline COLORREF ToColorRef(Rgb c) { return RGB(c.r, c.g, c.b); }

}  // namespace jisu
