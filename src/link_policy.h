#pragma once
#include <cstdint>
namespace jisu {
struct LinkGesture {
  uint64_t at = 0;
  int button = 0;
  bool ctrl = false, shift = false;
  bool TakeBackground(bool userInitiated,bool agent,uint64_t now) {
    const bool valid=at&&now>=at&&now-at<=5000;
    const bool result=agent||(userInitiated&&(!valid||(!shift&&(ctrl||button==1||button==2))));
    at=0;return result;
  }
};
inline constexpr wchar_t kLinkGestureScript[]=LR"JS((()=>{
 const bridge=window.chrome?.webview;if(!bridge||window.__jisuInputInstalled)return;window.__jisuInputInstalled=true;
 const post=bridge.postMessage.bind(bridge);
 const capture=e=>{if(!e.isTrusted)return;if(e.type==='keydown'&&e.key!=='Enter'&&e.key!==' ')return;
  post(JSON.stringify({type:'jisu.input',button:e.type==='keydown'?0:e.button,ctrl:!!e.ctrlKey,shift:!!e.shiftKey}));};
 window.addEventListener('pointerdown',capture,true);window.addEventListener('keydown',capture,true);
})();)JS";
}  // namespace jisu
