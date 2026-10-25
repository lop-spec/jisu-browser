#include "palette.h"

namespace jisu {
namespace {

constexpr Palette kLight{
    .dark = false,
    .frame = {0xEB, 0xF2, 0xFA},
    .tabHover = {0xDC, 0xE6, 0xF4},
    .toolbar = {0xFF, 0xFF, 0xFF},
    .buttonHover = {0xEC, 0xED, 0xEF},
    .omnibox = {0xF1, 0xF3, 0xF4},
    .omniboxBorder = {0xDA, 0xDC, 0xE0},
    .separator = {0xDA, 0xDC, 0xE0},
    .text = {0x1F, 0x1F, 0x1F},
    .textMuted = {0x5F, 0x63, 0x68},
    .accent = {0x1A, 0x73, 0xE8},
    .content = {0xFF, 0xFF, 0xFF},
};

}  // namespace

Palette LoadPalette() {
  // The supplied reference is a light blue-gray strip and white connected tab.
  // Keep browser chrome stable even when Windows applications use dark mode;
  // this does not alter the Windows setting or force a website's color scheme.
  return kLight;
}

}  // namespace jisu
