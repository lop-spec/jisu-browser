// Jisu native-tab adapter for the existing first-party Auto Close Old Tabs popup.
// No access to ordinary websites, passwords, cookies or authentication data.
'use strict';
const input=document.getElementById('maxTabs');
const status=document.getElementById('status');
const wv=window.chrome?.webview;
const send=value=>wv?.postMessage(JSON.stringify(value));
function save(){const raw=input.value.trim();const n=Number(raw);if(!/^\d+$/.test(raw)||n<1||n>99){status.textContent='请输入 1–99';return;}send({type:'oldtabs.set',maxTabs:n});}
wv?.addEventListener('message',event=>{const m=event.data;if(m?.type==='oldtabs.settings'){input.value=String(m.maxTabs);status.textContent='上限 '+m.maxTabs+' 个普通标签；保护当前页、智能体和扩展页。';}});
document.getElementById('save').addEventListener('click',save);
input.addEventListener('keydown',event=>{if(event.key==='Enter')save();});
send({type:'oldtabs.get'});
