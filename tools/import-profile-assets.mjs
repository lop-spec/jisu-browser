// Machine-local extension PROGRAMS and favicon images only. Never reads Login Data,
// Cookies, Local State, extension storage, browser credentials or passwords.
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import {DatabaseSync} from 'node:sqlite';
import {spawnSync} from 'node:child_process';
const apply=process.argv.includes('--apply');
const profile=path.join(os.userInfo().homedir,'AppData','Local','JisuBrowser','User Data');
const local=path.join(os.userInfo().homedir,'AppData','Local');
const configFile=path.join(profile,'extensions.json');
const existing=fs.existsSync(configFile)?JSON.parse(fs.readFileSync(configFile,'utf8')):{extensions:[]};
const extensions=new Map(existing.extensions.map(e=>[e.sourceId,e]));
// Exclusive native features or redundant implementations are intentionally excluded.
const excluded=new Map([
 ['bgnkhhnnamicmpeenaelnjfhikgbkllg','Use one blocker (uBlock Origin Lite), not three'],
 ['cfhdojbkjhnklbpkdaibdccddilifddb','Use one blocker (uBlock Origin Lite), not three'],
 ['noogafoofpebimajpfpamcfhoaifemoa','Conflicts with owned agent tabs and native suspension'],
 ['gbkeegbaiigmenfmjfclcdgdpimamgkj','Requires private Chrome/Google filesystem and identity APIs'],
 ['mcbpblocgmgfnpjjppndjkmgjaogfceg','Requires separately installed native messaging host'],
 ['ijejnggjjphlenbhmjhhgcdpehhacaal','Requires desktop/tab capture or debugger integration'],
 ['mapjgeachilmcbbokkgcbgpbakaaeehi','Native bookmarks/history are not the WebView2 profile database'],
 ['egehpkpgpgooebopjihjmnpejnjafefi','Native history is not exposed to this extension'],
 ['gjldcdngmdknpinoemndlidpcabkggco','Use the native extension manager instead'],
 ['ieehkmoiljghfkejgahoheemdjpdinml','Closed native tabs are not extension tabs'],
]);
const defaultOn=new Set(['ddkjiahejlhfcafbddmgiahcphecmpfh','bpoadfkcbjbfhfodiogcnhhhpibjhbnh']);
const skipped=[];let added=0,icons=0;
function nameOf(m,folder){let name=m.name;if(!/^__MSG_.+__$/.test(name))return name;const key=name.slice(6,-2);for(const locale of [m.default_locale,'zh_CN','en'].filter(Boolean)){try{const messages=JSON.parse(fs.readFileSync(path.join(folder,'_locales',locale,'messages.json'),'utf8'));const hit=Object.entries(messages).find(([k])=>k.toLowerCase()===key.toLowerCase());if(hit)return hit[1].message;}catch{}}return name;}
function originKey(url){try{const u=new URL(url);if(!['http:','https:'].includes(u.protocol)||u.username||u.password)return null;let h=14695981039346656037n;for(const b of Buffer.from(u.origin.toLowerCase()))h=BigInt.asUintN(64,(h^BigInt(b))*1099511628211n);return h.toString(16).padStart(16,'0');}catch{return null;}}
function backup(file){if(!fs.existsSync(file))return;const tool=process.env.PI_BACKUP_TOOL||'D:/Downloads/pi-protable/tools/backup.mjs';const r=spawnSync(process.execPath,[tool,file],{windowsHide:true,encoding:'utf8'});if(r.status!==0)throw Error('Physical backup failed');}
for(const browser of ['Thorium','Quark']){const root=path.join(local,browser,'User Data');if(!fs.existsSync(root))continue;for(const p of fs.readdirSync(root).filter(p=>p==='Default'||p.startsWith('Profile '))){const source=path.join(root,p);const extRoot=path.join(source,'Extensions');if(fs.existsSync(extRoot))for(const id of fs.readdirSync(extRoot)){const v=fs.readdirSync(path.join(extRoot,id)).filter(v=>fs.existsSync(path.join(extRoot,id,v,'manifest.json'))).sort((a,b)=>b.localeCompare(a,undefined,{numeric:true}))[0];if(!v)continue;const folder=path.join(extRoot,id,v);const m=JSON.parse(fs.readFileSync(path.join(folder,'manifest.json'),'utf8'));const name=nameOf(m,folder);if(excluded.has(id)||m.manifest_version!==3||(m.permissions||[]).some(p=>['nativeMessaging','debugger','desktopCapture'].includes(p))){skipped.push({id,name,reason:excluded.get(id)||'Requires unsupported browser integration'});continue;}if(extensions.has(id))continue;const dest=path.join(profile,'Extensions',id,v);if(apply){fs.mkdirSync(path.dirname(dest),{recursive:true});fs.cpSync(folder,dest,{recursive:true,dereference:false,filter:file=>{if(fs.lstatSync(file).isSymbolicLink())throw Error('Extension symlink refused');return true;}});}extensions.set(id,{sourceId:id,name,path:dest,popup:m.action?.default_popup||m.browser_action?.default_popup||'',options:m.options_page||m.options_ui?.page||'',enabled:defaultOn.has(id),source:browser,version:m.version});added++;}
 const file=path.join(source,'Favicons');if(!fs.existsSync(file))continue;
 let db;const snapshot=path.join(os.tmpdir(),'jisu-favicons-'+process.pid+'-'+browser+'-'+p.replaceAll(' ','')+'.db');try{fs.copyFileSync(file,snapshot);db=new DatabaseSync(snapshot,{readOnly:true});if(db.prepare('PRAGMA quick_check').get().quick_check!=='ok')throw Error('Inconsistent favicon snapshot');const rows=db.prepare('SELECT m.page_url, b.image_data, b.width FROM icon_mapping m JOIN favicon_bitmaps b ON b.icon_id=m.icon_id WHERE b.width>=16 AND length(b.image_data)>0 AND length(b.image_data)<=524288 ORDER BY b.width DESC').iterate();const seen=new Set();for(const r of rows){const key=originKey(r.page_url);if(!key||seen.has(key))continue;seen.add(key);const dest=path.join(profile,'Favicons',key+'.png');if(fs.existsSync(dest))continue;if(apply){fs.mkdirSync(path.dirname(dest),{recursive:true});fs.writeFileSync(dest,r.image_data,{flag:'wx'});}icons++;}}catch(e){skipped.push({source:browser,asset:'favicons',reason:e.code||'Source database unavailable'});}finally{db?.close();fs.rmSync(snapshot,{force:true});}
}}
const report={apply,addedExtensions:added,extensions:[...extensions.values()].map(({path,...x})=>x),faviconOrigins:icons,skipped,passwords:'Not accessed. WebView2 has no supported password-import API.',credentialStorageCopied:false};
if(apply){fs.mkdirSync(profile,{recursive:true});backup(configFile);fs.writeFileSync(configFile,JSON.stringify({extensions:[...extensions.values()]},null,2));fs.writeFileSync(path.join(profile,'migration-report.json'),JSON.stringify(report,null,2));}
console.log(JSON.stringify(report,null,2));
