#include "tab_policy.h"
#include <iostream>
#include <stdexcept>
using namespace jisu;
void require(bool ok) { if (!ok) throw std::runtime_error("tab LRU policy gate failed"); }
int main() {
  require(OldTabsToClose({},10).empty());
  const std::vector<TabPolicyEntry> tabs={{8,false,false,false,100},{2,false,true,false,0},{3,false,false,true,0},{9,true,false,false,300},{4,false,false,false,200},{12,false,false,false,250}};
  require(OldTabsToClose(tabs,10).empty());
  require(OldTabsToClose(tabs,2)==std::vector<uint32_t>({8,4}));
  require(OldTabsToClose(tabs,1)==std::vector<uint32_t>({8,4,12}));
  require(OldTabsToClose(tabs,0)==std::vector<uint32_t>({8,4,12}));
  // A pre-created spare has the smallest id but becomes the newest user tab when adopted.
  require(OldTabsToClose({{1,false,false,false,400},{20,false,false,false,100},{21,true,false,false,300}},2)==std::vector<uint32_t>({20}));
  // A startup URL gets id 1; restored tabs get larger ids and earlier persisted usage stamps.
  require(OldTabsToClose({{1,true,false,false,500},{2,false,false,false,50},{3,false,false,false,200}},2)==std::vector<uint32_t>({2}));
  // Activating an old tab protects its new recency after it goes to the background again.
  require(OldTabsToClose({{2,false,false,false,1000},{3,false,false,false,200},{4,true,false,false,1100}},2)==std::vector<uint32_t>({3}));
  require(OldTabsToClose({{1,true,false,false,0},{2,false,true,false,0},{3,false,false,true,0}},1).empty());
  std::cout<<"PASS LRU independent of ids; adopted spare; restored timestamps; startup URL; recent reactivation; protections\n";
}
