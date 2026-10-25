// Close Old Tab native adapter: extension tabs APIs do not own native WebView2 tabs.
#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>
namespace jisu {
struct TabPolicyEntry { uint32_t id; bool active = false; bool agent = false; bool privileged = false; };
inline std::vector<uint32_t> OldTabsToClose(const std::vector<TabPolicyEntry>& entries, size_t limit) {
  limit = std::clamp<size_t>(limit, 1, 99);
  size_t normal = 0;
  for (const auto& e : entries) if (!e.agent && !e.privileged) ++normal;
  std::vector<uint32_t> result;
  if (normal <= limit) return result;
  std::vector<TabPolicyEntry> ordered = entries;
  std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
  for (const auto& e : ordered) {
    if (normal <= limit) break;
    if (e.active || e.agent || e.privileged) continue;
    result.push_back(e.id); --normal;
  }
  return result;
}
}  // namespace jisu
