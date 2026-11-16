// Runs only after an explicit native Translate command. No page-wide startup hooks.
(() => {
  window.__jisuTranslation?.restore();
  const originals = new Map(), seen = new WeakMap(), queue = [], cache = new Map();
  let stopped = false, pending = [], translated = 0;
  const host = document.createElement('div');
  host.id = 'jisu-translation';
  host.title = '本页正文会发送给 Google；不发送输入框、密码和代码。';
  host.style.cssText = 'position:fixed;right:16px;bottom:16px;z-index:2147483647';
  const shadow = host.attachShadow({mode:'open'});
  shadow.innerHTML = '<style>:host{color-scheme:light}div{font:13px system-ui;color:#202124;background:#fff;border:1px solid #dadce0;border-radius:6px;padding:10px;display:flex;gap:12px;align-items:center}button{font:inherit;cursor:pointer;border:0;background:transparent;color:#1967d2;white-space:nowrap}span{max-width:260px}</style><div><span role="status">Google 翻译准备中</span><button type="button">恢复原文</button></div>';
  const status = shadow.querySelector('span');
  (document.body || document.documentElement).append(host);
  const ignored = 'script,style,noscript,textarea,input,select,option,pre,code,kbd,samp,svg,math,[contenteditable]:not([contenteditable="false"]),[translate="no"],.notranslate';
  function eligible(node) {
    const p = node.parentElement, text = node.nodeValue;
    return p && node.isConnected && !p.closest(ignored) && !host.contains(p) && text.trim() &&
      /[\p{L}]/u.test(text) && text.trim().length <= 1800 && !text.includes('\uE000') && p.getClientRects().length;
  }
  function scan(root) {
    if (stopped || queue.length > 10000) return;
    const walker = document.createTreeWalker(root, NodeFilter.SHOW_TEXT);
    function add(node) {
      if (!eligible(node) || seen.get(node) === node.nodeValue) return;
      seen.set(node,node.nodeValue); queue.push(node);
    }
    if (root.nodeType === Node.TEXT_NODE) add(root);
    else {let node; while ((node = walker.nextNode())) add(node);}
  }
  const observer = new MutationObserver(records => {
    for (const r of records) {
      if (r.type === 'characterData') scan(r.target);
      else for (const n of r.addedNodes) scan(n);
    }
  });
  observer.observe(document.body || document.documentElement,{childList:true,characterData:true,subtree:true});
  scan(document.body || document.documentElement);
  const api = {
    next() {
      if (stopped) return {stopped:true};
      pending = [];
      let size = 0;
      while (queue.length && pending.length < 20 && size < 1800) {
        const node = queue.shift(); if (!eligible(node)) continue;
        const source = node.nodeValue, key = source.trim();
        if (cache.has(key)) {apply(node,source,cache.get(key));continue;}
        if (pending.length && size + key.length > 2400) {queue.unshift(node);break;}
        pending.push({node,source,key}); size += key.length;
      }
      if (!pending.length) {status.textContent = translated ? `Google · 已翻译 ${translated} 处` : '没有可翻译的正文';return {texts:[]};}
      status.textContent = `Google · 翻译中（${translated}）`;
      return {texts:pending.map(p=>p.key)};
    },
    accept(values) {
      if (stopped || values.length !== pending.length) return;
      pending.forEach((p,i)=> {cache.set(p.key,values[i]);apply(p.node,p.source,values[i]);});
      pending = [];
      status.textContent = `Google · 已翻译 ${translated} 处`;
    },
    error(message) {status.textContent = message;},
    restore() {
      if(stopped)return;stopped=true;observer.disconnect();queue.length=0;
      for (const [node,pair] of originals) if(node.isConnected && node.nodeValue===pair.output) node.nodeValue=pair.source;
      originals.clear();cache.clear();pending=[];host.remove();
    },
    get count(){return translated;}
  };
  function apply(node,source,value) {
    if(stopped || !node.isConnected || node.nodeValue!==source) return;
    const leading=source.match(/^\s*/)[0], trailing=source.match(/\s*$/)[0];
    const output=leading+value.trim()+trailing;
    const previous=originals.get(node);
    originals.set(node,{source:previous && previous.output===source ? previous.source : source,output});
    seen.set(node,output);node.nodeValue=output;++translated;
  }
  shadow.querySelector('button').onclick=()=>api.restore();
  window.__jisuTranslation=api;
})();
