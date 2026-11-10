#include "link_policy.h"
#include <iostream>
#include <cstdlib>
using namespace jisu;
void Check(bool ok,const char* text){if(!ok){std::cerr<<"FAIL link policy: "<<text<<'\n';std::exit(1);}}
int main(){
  LinkGesture ctrl{100,0,true,false};Check(ctrl.TakeBackground(true,false,400),"original Ctrl survives release before callback");Check(ctrl.at==0,"gesture consumed once");
  LinkGesture shift{100,0,true,true};Check(!shift.TakeBackground(true,false,400),"Ctrl+Shift remains foreground");
  LinkGesture middle{100,1,false,false};Check(middle.TakeBackground(true,false,400),"middle background");
  LinkGesture right{100,2,false,false};Check(right.TakeBackground(true,false,400),"native new-tab context background");
  LinkGesture ordinary{100,0,false,false};Check(!ordinary.TakeBackground(true,false,400),"plain target blank foreground");
  LinkGesture fake{100,0,true,false};Check(!fake.TakeBackground(false,false,400),"snapshot cannot grant user activation");
  LinkGesture expired{100,0,false,true};Check(expired.TakeBackground(true,false,6000),"missing/expired snapshot conservatively background");
  LinkGesture agent{};Check(agent.TakeBackground(false,true,400),"agent never activates a new window");
  std::cout<<"PASS original gesture snapshots, Shift override, middle/context background, consume, expiry and no authority escalation\n";
}
