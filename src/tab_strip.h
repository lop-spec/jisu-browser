// Reference tab strip: 220 DIP tabs, 50 DIP strip, 5 DIP top inset.
// Share the same rectangles between drawing and mouse hit testing.
#pragma once

#include <d2d1.h>
#include <algorithm>
#include <cstddef>
#include <vector>

namespace jisu {
inline constexpr float kTabStripHeight = 50.0f;
inline constexpr float kTabTop = 5.0f;
inline constexpr float kTabPreferredWidth = 220.0f;
inline constexpr float kTabCenterY = kTabStripHeight * 0.5f;

struct TabStripLayout {
  std::vector<D2D1_RECT_F> tabs;
  std::vector<D2D1_RECT_F> closes;
  D2D1_RECT_F newTab{};
};

// maxWidth：连续点关闭时先保持关之前的宽度，下一个关闭钮正好落在光标下，鼠标离开标签条再展开
inline TabStripLayout LayoutTabStrip(float width, size_t count, float maxWidth = kTabPreferredWidth) {
  constexpr float left = 12.0f;
  constexpr float controls = 138.0f;
  constexpr float dragSpace = 16.0f;
  constexpr float plusWidth = 28.0f;
  constexpr float plusGap = 4.0f;
  const float available = std::max(0.0f, width - left - controls - dragSpace - plusWidth - plusGap);
  // No hard minimum: even a crowded strip must not cover the + or window buttons.
  const float tabWidth =
      count ? std::min({kTabPreferredWidth, maxWidth, available / static_cast<float>(count)}) : 0.0f;
  TabStripLayout result;
  result.tabs.reserve(count);
  result.closes.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    const float x = left + static_cast<float>(i) * tabWidth;
    const auto tab = D2D1::RectF(x, kTabTop, x + tabWidth, kTabStripHeight);
    result.tabs.push_back(tab);
    const float inset = std::min(17.0f, std::max(0.0f, (tabWidth - 18.0f) * 0.5f));
    result.closes.push_back(tabWidth >= 32.0f
        ? D2D1::RectF(tab.right - inset - 18.0f, kTabCenterY - 9.0f, tab.right - inset, kTabCenterY + 9.0f)
        : D2D1::RectF(tab.right, kTabCenterY, tab.right, kTabCenterY));
  }
  const float plusLeft = left + static_cast<float>(count) * tabWidth + plusGap;
  result.newTab = D2D1::RectF(plusLeft, kTabCenterY - 14.0f, plusLeft + plusWidth, kTabCenterY + 14.0f);
  return result;
}
}  // namespace jisu
