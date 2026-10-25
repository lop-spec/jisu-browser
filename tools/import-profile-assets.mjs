// Only the three extensions requested by lop; no browser credential or extension storage is read/copied.
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import crypto from 'node:crypto';
import {DatabaseSync} from 'node:sqlite';
import {spawnSync} from 'node:child_process';
const apply=process.argv.includes('--apply');
const local=path.join(os.userInfo().homedir,'AppData','Local');
const profile=path.join(local,'JisuBrowser','User Data');
const configFile=path.join(profile,'extensions.json');
const approved=new Set(['hpmbccepehpoanjpjkamfdpdkbmfmhek','auto-close-old-tabs','authenticator','bhghoamapcdpbohphigoooaddinpkbai']);
const previous=fs.existsSync(configFile)?JSON.parse(fs.readFileSync(configFile,'utf8')):{extensions:[]};
const extensions=new Map(previous.extensions.filter(e=>approved.has(e.sourceId)).map(e=>[e.sourceId,{...e,enabled:true}]));
const candidates=[],skipped=[];let icons=0;
function publicId(m,fallback){return m.key?crypto.createHash('sha256').update(Buffer.from(m.key,'base64')).digest('hex').slice(0,32).replace(/[0-9a-f]/g,c=>String.fromCharCode(97+parseInt(c,16))):fallback;}
function nameOf(m,folder){let name=m.name;if(!/^__MSG_.+__$/.test(name))return name;const key=name.slice(6,-2);for(const locale of [m.default_locale,'zh_CN','en'].filter(Boolean)){try{const messages=JSON.parse(fs.readFileSync(path.join(folder,'_locales',locale,'messages.json'),'utf8'));const hit=Object.entries(messages).find(([k])=>k.toLowerCase()===key.toLowerCase());if(hit)return hit[1].message;}catch{}}return name;}
function originKey(url){try{const u=new URL(url);if(!['http:','https:'].includes(u.protocol)||u.username||u.password)return null;let h=14695981039346656037n;for(const b of Buffer.from(u.origin.toLowerCase()))h=BigInt.asUintN(64,(h^BigInt(b))*1099511628211n);return h.toString(16).padStart(16,'0');}catch{return null;}}
function backup(file){if(!fs.existsSync(file))return;const tool=process.env.PI_BACKUP_TOOL||'D:/Downloads/pi-protable/tools/backup.mjs';const r=spawnSync(process.execPath,[tool,file],{windowsHide:true,encoding:'utf8'});if(r.status!==0)throw Error('Physical backup failed');}
for(const browser of ['Thorium','Quark']){
 const root=path.join(local,browser,'User Data');
 const unpacked=path.join(local,browser,'Extensions');
 for(const id of ['auto-close-old-tabs','authenticator']){const folder=path.join(unpacked,id);if(fs.existsSync(path.join(folder,'manifest.json')))candidates.push({id,folder,browser});}
 if(!fs.existsSync(root))continue;
 for(const p of fs.readdirSync(root).filter(p=>p==='Default'||p.startsWith('Profile '))){
  const source=path.join(root,p),extRoot=path.join(source,'Extensions');
  if(fs.existsSync(extRoot))for(const id of fs.readdirSync(extRoot)){if(!approved.has(id))continue;const v=fs.readdirSync(path.join(extRoot,id)).filter(v=>fs.existsSync(path.join(extRoot,id,v,'manifest.json'))).sort((a,b)=>b.localeCompare(a,undefined,{numeric:true}))[0];if(v)candidates.push({id,folder:path.join(extRoot,id,v),browser});}
  const file=path.join(source,'Favicons');if(!fs.existsSync(file))continue;
  let db;const snapshot=path.join(os.tmpdir(),'jisu-favicons-'+process.pid+'-'+browser+'-'+p.replaceAll(' ','')+'.db');
  try{fs.copyFileSync(file,snapshot);db=new DatabaseSync(snapshot,{readOnly:true});if(db.prepare('PRAGMA quick_check').get().quick_check!=='ok')throw Error('Inconsistent favicon snapshot');const rows=db.prepare('SELECT m.page_url, b.image_data, b.width FROM icon_mapping m JOIN favicon_bitmaps b ON b.icon_id=m.icon_id WHERE b.width>=16 AND length(b.image_data)>0 AND length(b.image_data)<=524288 ORDER BY b.width DESC').iterate();const seen=new Set();for(const r of rows){const key=originKey(r.page_url);if(!key||seen.has(key))continue;seen.add(key);const dest=path.join(profile,'Favicons',key+'.png');if(fs.existsSync(dest))continue;if(apply){fs.mkdirSync(path.dirname(dest),{recursive:true});fs.writeFileSync(dest,r.image_data,{flag:'wx'});}icons++;}}
  catch(e){skipped.push({source:browser,asset:'favicons',reason:e.code||'Source image database unavailable'});}finally{db?.close();fs.rmSync(snapshot,{force:true});}
 }
}
// Exact program-only package directory, populated by a verified official/local installed package.
for(const id of ['hpmbccepehpoanjpjkamfdpdkbmfmhek','auto-close-old-tabs','authenticator']){const parent=path.join(local,'JisuBrowser','ExtensionPackages',id);if(!fs.existsSync(parent))continue;for(const v of fs.readdirSync(parent)){const folder=path.join(parent,v);if(fs.existsSync(path.join(folder,'manifest.json')))candidates.push({id,folder,browser:'program-package'});}}
for(const {id,folder,browser} of candidates){if(extensions.has(id))continue;const m=JSON.parse(fs.readFileSync(path.join(folder,'manifest.json'),'utf8'));if(m.manifest_version!==3)throw Error('Approved extension requires MV3');const name=nameOf(m,folder);const dest=path.join(profile,'Extensions',id,m.version);if(apply){fs.mkdirSync(path.dirname(dest),{recursive:true});fs.cpSync(folder,dest,{recursive:true,dereference:false,filter:file=>{if(fs.lstatSync(file).isSymbolicLink())throw Error('Extension symlink refused');return true;}});}extensions.set(id,{sourceId:id,expectedId:publicId(m,id),name,path:dest,popup:m.action?.default_popup||m.browser_action?.default_popup||'',options:m.options_page||m.options_ui?.page||'',enabled:true,source:browser,version:m.version});}
if(apply&&extensions.has('auto-close-old-tabs')){
 const e=extensions.get('auto-close-old-tabs');
 for(const file of ['background.js','popup.js'])backup(path.join(e.path,file));
 // Chrome rejects unknown directories beginning with _. Keep physical backups outside the program snapshot.
 const history=path.join(e.path,'_历史版本');
 if(fs.existsSync(history)){const safe=path.join(profile,'_历史版本','old-tabs-adapter-'+Date.now());fs.mkdirSync(path.dirname(safe),{recursive:true});fs.renameSync(history,safe);}
 fs.writeFileSync(path.join(e.path,'background.js'),'// Native Jisu policy manages ordinary tabs; never touch agent leases.\n');
 fs.writeFileSync(path.join(e.path,'popup.js'),fs.readFileSync(new URL('./old-tabs-webview-popup.js',import.meta.url)));
 e.nativeAdapter=true;
}
const report={apply,extensions:[...extensions.values()].map(({path,...e})=>e),removedFromConfig:previous.extensions.filter(e=>!approved.has(e.sourceId)).length,faviconOrigins:icons,skipped,passwords:'Not accessed. WebView2 has no supported password-import API.',seedsOrVaultsCopied:false,cdp:'Native implementation; no extension installed'};
if(apply){fs.mkdirSync(profile,{recursive:true});backup(configFile);fs.writeFileSync(configFile,JSON.stringify({removeUnlisted:true,extensions:[...extensions.values()]},null,2));fs.writeFileSync(path.join(profile,'migration-report.json'),JSON.stringify(report,null,2));}
console.log(JSON.stringify(report,null,2));
