#include "tab_policy.h"
#include <iostream>
#include <stdexcept>
using namespace jisu;
void require(bool ok) { if (!ok) throw std::runtime_error("tab policy gate failed"); }
int main() {
  require(OldTabsToClose({},10).empty());
  const std::vector<TabPolicyEntry> tabs={{8,false,false,false},{2,false,true,false},{3,false,false,true},{9,true,false,false},{4,false,false,false},{12,false,false,false}};
  require(OldTabsToClose(tabs,10).empty());
  require(OldTabsToClose(tabs,2)==std::vector<uint32_t>({4,8}));
  require(OldTabsToClose(tabs,1)==std::vector<uint32_t>({4,8,12}));
  require(OldTabsToClose(tabs,0)==std::vector<uint32_t>({4,8,12}));
  std::vector<TabPolicyEntry> protectedTabs={{1,true,false,false},{2,false,true,false},{3,false,false,true}};
  require(OldTabsToClose(protectedTabs,1).empty());
  std::cout<<"PASS oldest creation order; active/agent/extension tabs protected; limits clamped\n";
}
