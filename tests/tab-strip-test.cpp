#include "tab_strip.h"
#include <cmath>
#include <iostream>
#include <stdexcept>
using namespace jisu;
void require(bool ok) { if (!ok) throw std::runtime_error("tab strip geometry gate failed"); }
int main() {
  for (const float width : {390.0f, 480.0f, 1024.0f, 1920.0f}) {
    for (const size_t count : {size_t{0}, size_t{1}, size_t{3}, size_t{10}, size_t{50}}) {
      const auto strip = LayoutTabStrip(width, count);
      require(strip.tabs.size() == count && strip.closes.size() == count);
      require(strip.newTab.right <= width - 138.0f - 15.9f);
      for (size_t i = 0; i < count; ++i) {
        const auto& r = strip.tabs[i];
        const auto& c = strip.closes[i];
        require(r.top == 5.0f && r.bottom == 50.0f && r.right >= r.left);
        require(r.right - r.left <= kTabPreferredWidth + 0.01f);
        require(c.left >= r.left - 0.01f && c.right <= r.right + 0.01f);
        require(c.top >= r.top && c.bottom <= r.bottom);
        require(r.right <= strip.newTab.left);
        if (i) require(std::abs(strip.tabs[i-1].right - r.left) < 0.01f);
      }
    }
  }
  const auto reference = LayoutTabStrip(1024, 3);
  for (const auto& r : reference.tabs) require(r.right-r.left == 220.0f);
  require(reference.newTab.left == 676.0f);
  std::cout << "PASS reference 220x45 tabs, 50 high strip; 390/480/1024/1920; 0/1/3/10/50 tabs; close and + never overlap window controls\n";
}
