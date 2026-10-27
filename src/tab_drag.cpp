#include "browser_window.h"
#include <algorithm>
#include <cmath>

namespace jisu {
void BrowserWindow::MoveTab(size_t from, size_t to) {
  if (from >= tabs_.size() || to >= tabs_.size() || from == to) return;
  Tab* selected = ActiveTab();
  auto moving = std::move(tabs_[from]);
  tabs_.erase(tabs_.begin() + static_cast<std::ptrdiff_t>(from));
  tabs_.insert(tabs_.begin() + static_cast<std::ptrdiff_t>(to), std::move(moving));
  if (selected) active_ = IndexOf(*selected);
  pressed_ = {}; hover_ = {};
  Relayout();
  InvalidateChrome();
}

bool BrowserWindow::OutsideTabStrip(POINT point) const {
  const float x = point.x / layout_.scale, y = point.y / layout_.scale;
  // A small slop band prevents a slightly diagonal reorder from tearing out.
  constexpr float slop = 16.0f;
  return x < -slop || x >= layout_.width + slop || y < -slop || y >= layout_.tabStripHeight + slop;
}

void BrowserWindow::UpdateTabDrag(POINT point) {
  Tab* tab = FindTab(dragTabId_);
  if (!tab || IndexOf(*tab) == tabs_.size()) { CancelTabDrag(false); return; }
  if (!draggingTab_) {
    if (std::abs(point.x - dragTabStart_.x) < GetSystemMetricsForDpi(SM_CXDRAG, dpi_) &&
        std::abs(point.y - dragTabStart_.y) < GetSystemMetricsForDpi(SM_CYDRAG, dpi_)) return;
    draggingTab_ = true;
  }
  SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
  if (OutsideTabStrip(point) || layout_.tabs.empty()) return;
  const size_t from = IndexOf(*tab);
  const float width = layout_.tabs[from].right - layout_.tabs[from].left;
  if (width <= 0) return;
  const float center = point.x / layout_.scale - dragTabGrabX_ + width * 0.5f;
  const float slot = (center - layout_.tabs.front().left) / width;
  const size_t to = static_cast<size_t>(std::clamp(std::floor(slot), 0.0f, static_cast<float>(tabs_.size() - 1)));
  MoveTab(from, to);
}

void BrowserWindow::CancelTabDrag(bool restore) {
  if (!dragTabId_) return;
  const uint32_t id = dragTabId_;
  dragTabId_ = 0;
  if (restore && draggingTab_) {
    if (Tab* tab = FindTab(id); tab && IndexOf(*tab) < tabs_.size())
      MoveTab(IndexOf(*tab), std::min(dragTabOriginalIndex_, tabs_.size() - 1));
  }
  draggingTab_ = false;
  pressed_ = {}; hover_ = {};
  if (GetCapture() == hwnd_) ReleaseCapture();
  SetCursor(LoadCursorW(nullptr, IDC_ARROW));
  InvalidateChrome();
}

bool BrowserWindow::MoveTabTo(uint32_t id, BrowserWindow& destination) {
  Tab* tab = FindTab(id);
  if (!tab || &destination == this || !tab->controller || tab->creating || tab->agent) return false;
  const size_t from = IndexOf(*tab);
  if (from >= tabs_.size() || destination.profile_ != profile_) return false;
  if (fullscreen_.Active()) ExitFullscreen();
  if (FAILED(tab->controller->put_ParentWindow(destination.hwnd_))) return false;
  if (tab->mediaSurface) {
    SetParent(tab->mediaSurface, destination.hwnd_);
    if (GetParent(tab->mediaSurface) != destination.hwnd_) {
      tab->controller->put_ParentWindow(hwnd_);
      return false;
    }
    SetTimer(destination.hwnd_, 7, 250, nullptr);  // Same media polling timer as EnsureMedia.
  }
  tab->parent = destination.hwnd_;
  const size_t previous = active_;
  auto moving = std::move(tabs_[from]);
  tabs_.erase(tabs_.begin() + static_cast<std::ptrdiff_t>(from));
  *moving->owner = &destination;
  destination.tabs_.push_back(std::move(moving));
  destination.Relayout();
  destination.Activate(destination.tabs_.size() - 1);
  if (tabs_.empty()) DestroyWindow(hwnd_);
  else {
    hover_ = {}; pressed_ = {};
    Relayout();
    const size_t next = from < previous ? previous - 1 : std::min(previous, tabs_.size() - 1);
    Activate(next);
  }
  return true;
}

void BrowserWindow::FinishTabDrag(POINT point) {
  UpdateTabDrag(point);
  if (!dragTabId_) return;
  const uint32_t id = dragTabId_;
  const bool detach = draggingTab_ && OutsideTabStrip(point);
  const float grab = dragTabGrabX_;
  CancelTabDrag(false);
  if (!detach) return;
  Tab* tab = FindTab(id);
  if (!tab || tab->agent || !tab->controller || tab->creating) return;
  POINT screen = point;
  ClientToScreen(hwnd_, &screen);
  RECT bounds{};
  GetWindowRect(hwnd_, &bounds);
  if (tabs_.size() == 1) {
    if (IsZoomed(hwnd_)) ShowWindow(hwnd_, options_.noActivate ? SW_SHOWNOACTIVATE : SW_RESTORE);
    SetWindowPos(hwnd_, nullptr, screen.x - static_cast<int>(grab * layout_.scale),
                 screen.y - static_cast<int>(kTabCenterY * layout_.scale), 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    return;
  }
  Options options = options_;
  options.hasBounds = true;
  options.x = screen.x - static_cast<int>(grab * layout_.scale);
  options.y = screen.y - static_cast<int>(kTabCenterY * layout_.scale);
  options.width = bounds.right - bounds.left;
  options.height = bounds.bottom - bounds.top;
  options.startUrl.clear();
  options.metricsPath.clear();
  options.exitAfterMetrics = false;
  options.preheatSeconds = 0;
  auto window = std::make_unique<BrowserWindow>(options, warmup_, root_);
  window->environment_ = environment_;
  window->profileConfigured_ = true;  // Reuse the already-configured shared WebView profile.
  window->extensionProfile_ = extensionProfile_;
  for (const auto& source : extensions_) {
    ExtensionEntry entry = source;
    entry.button = nullptr; entry.toolbarIcon = nullptr;
    window->extensions_.push_back(std::move(entry));
  }
  if (!window->Create(instance_)) return;
  auto* destination = window.get();
  root_->detachedWindows_.push_back(std::move(window));
  if (!MoveTabTo(id, *destination)) {
    DestroyWindow(destination->hwnd_);
    return;
  }
  destination->ShowFirstFrame(SW_SHOWNORMAL);
  destination->RefreshExtensionToolbar();
  destination->PlaceAllTabs();
  destination->UpdateAddressBar();
  destination->UpdateWindowTitle();
  // Shared-profile windows keep the same WebView, DOM, history and native media process.
}
}  // namespace jisu
