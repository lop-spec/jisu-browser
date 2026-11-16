import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';
const script=fs.readFileSync(new URL('../ntp/translation.js',import.meta.url),'utf8');
class Text {
 constructor(value,parent){this.nodeValue=value;this.parentElement=parent;this.nodeType=3;this.isConnected=true;}
}
class Element {
 constructor(ignored=false){this.children=[];this.ignored=ignored;this.nodeType=1;this.isConnected=true;this.style={};}
 closest(){return this.ignored?this:null;}
 getClientRects(){return [1];}
 contains(n){return this.children.includes(n);}
 append(n){this.children.push(n);}
 attachShadow(){const span={textContent:''},button={onclick:null};return this.shadowRoot={innerHTML:'',querySelector:q=>q==='span'?span:button};}
 remove(){this.isConnected=false;}
}
const body=new Element(), source=new Text('  Hello world  ',body),link=new Text('Learn more',body),code=new Text('const secret = 1',new Element(true));
body.children.push(source,link,code);
let observer;
const context={window:{},Node:{TEXT_NODE:3},NodeFilter:{SHOW_TEXT:4},document:{body,documentElement:body,
 createElement:()=>new Element(),createTreeWalker:root=>{let i=0;const nodes=root.children?.filter(n=>n.nodeType===3)||[];return {nextNode:()=>nodes[i++]||null};}},
 MutationObserver:class{constructor(callback){this.callback=callback;observer=this;}observe(){}disconnect(){this.disconnected=true;}}};
vm.createContext(context);vm.runInContext(script,context);
const api=context.window.__jisuTranslation;
assert.deepEqual(Array.from(api.next().texts),['Hello world','Learn more']);
api.accept(['你好世界','了解更多']);
assert.equal(source.nodeValue,'  你好世界  ');assert.equal(link.nodeValue,'了解更多');assert.equal(code.nodeValue,'const secret = 1');assert.equal(api.count,2);
observer.callback([{type:'characterData',target:source}]);assert.equal(api.next().texts.length,0,'own mutations must not retranslate');
const dynamic=new Text('New content',body);body.children.push(dynamic);observer.callback([{type:'childList',addedNodes:[dynamic]}]);
assert.deepEqual(Array.from(api.next().texts),['New content']);api.accept(['新内容']);assert.equal(dynamic.nodeValue,'新内容');
const stale=new Text('Old value',body);body.children.push(stale);observer.callback([{type:'childList',addedNodes:[stale]}]);api.next();stale.nodeValue='Site updated this';api.accept(['旧值']);assert.equal(stale.nodeValue,'Site updated this');
link.nodeValue='Changed by site';api.restore();
assert.equal(source.nodeValue,'  Hello world  ');assert.equal(dynamic.nodeValue,'New content');assert.equal(link.nodeValue,'Changed by site');assert.equal(api.next().stopped,true);assert.equal(observer.disconnected,true);
console.log('PASS translation: batching, whitespace, code exclusion, own-mutation suppression, dynamic text, stale results and safe restore');
