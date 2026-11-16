# Explicit live-provider acceptance; run with tools/run-on-hidden-desktop.ps1. Not a CI/network-free gate.
param([Parameter(Mandatory)][string]$Exe)
$ErrorActionPreference='Stop'
[Console]::OutputEncoding=New-Object Text.UTF8Encoding($false)
$c=New-Object CodeDom.Compiler.CompilerParameters;$c.GenerateInMemory=$true
Add-Type -CompilerParameters $c -TypeDefinition @'
using System;using System.Text;using System.Runtime.InteropServices;
public static class TranslationLive {
 delegate bool EnumProc(IntPtr h,IntPtr l);
 [DllImport("user32.dll")]static extern bool EnumChildWindows(IntPtr h,EnumProc f,IntPtr l);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)]static extern int GetClassNameW(IntPtr h,StringBuilder b,int n);
 [DllImport("user32.dll")]public static extern bool PostMessageW(IntPtr h,uint m,IntPtr w,IntPtr l);
 [DllImport("user32.dll")]static extern uint GetWindowThreadProcessId(IntPtr h,out uint p);
 [DllImport("kernel32.dll")]static extern uint GetCurrentThreadId();
 [DllImport("user32.dll")]static extern bool AttachThreadInput(uint a,uint b,bool attach);
 [DllImport("user32.dll")]static extern bool GetKeyboardState(byte[] state);
 [DllImport("user32.dll")]static extern bool SetKeyboardState(byte[] state);
 static uint src,dst;static bool attached,active;static byte[] saved;
 public static void End(){if(!active)return;SetKeyboardState(saved);if(attached)AttachThreadInput(src,dst,false);active=attached=false;}
 public static void Toggle(IntPtr root){End();uint pid;dst=GetWindowThreadProcessId(root,out pid);src=GetCurrentThreadId();saved=new byte[256];if(src!=dst){attached=AttachThreadInput(src,dst,true);if(!attached)throw new Exception("attach failed");}GetKeyboardState(saved);active=true;var state=new byte[256];state[0x12]=0x80;SetKeyboardState(state);IntPtr edit=IntPtr.Zero;EnumChildWindows(root,(h,l)=>{var b=new StringBuilder(128);GetClassNameW(h,b,128);if(b.ToString().Equals("Edit",StringComparison.OrdinalIgnoreCase))edit=h;return true;},IntPtr.Zero);if(edit==IntPtr.Zero||!PostMessageW(edit,0x104,(IntPtr)84,(IntPtr)1))throw new Exception("Alt+T post failed");}
}
'@
$root=Join-Path $env:TEMP ('jisu-translate-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root|Out-Null
[IO.File]::WriteAllText((Join-Path $root 'jisu-settings.json'),'{"bookmarkBar":false,"importedDefaultBrowser":true}')
$port=Get-Random -Minimum 24000 -Maximum 44000
$server=New-Object Net.HttpListener;$server.Prefixes.Add("http://localhost:$port/");$server.Start();$script:pending=$server.GetContextAsync();$script:metric=$null
$html=@'
<!doctype html><meta charset="utf-8"><title>translation fixture</title>
<h1 id="heading">Machine translation</h1><p id="text">Neural machine translation uses artificial intelligence.</p>
<a id="link" href="/keep-link">Learn more</a><input value="Do not translate my draft"><pre>const privateValue = 42;</pre><div id="dynamic"></div>
<script>
let added=false;
setInterval(()=>{if(!added&&/[\u4e00-\u9fff]/.test(heading.textContent)){added=true;dynamic.textContent='This sentence was added later.';}
fetch('/metric?value='+encodeURIComponent(JSON.stringify({h:heading.textContent,p:text.textContent,link:link.getAttribute('href'),input:document.querySelector('input').value,code:document.querySelector('pre').textContent,d:dynamic.textContent,status:document.getElementById('jisu-translation')?.shadowRoot.querySelector('span').textContent||''}))).catch(()=>{});},200);
</script>
'@
function Pump([int]$ms){$until=(Get-Date).AddMilliseconds($ms);do{if($script:pending.Wait(20)){$x=$script:pending.Result;$script:pending=$server.GetContextAsync();if($x.Request.Url.AbsolutePath -eq '/metric'){$script:metric=$x.Request.QueryString['value']|ConvertFrom-Json;$text='ok'}else{$text=$html};$bytes=[Text.Encoding]::UTF8.GetBytes($text);$x.Response.ContentType='text/html; charset=utf-8';try{$x.Response.OutputStream.Write($bytes,0,$bytes.Length);$x.Response.Close()}catch{}}}while((Get-Date)-lt $until)}
$p=$null
try{
 $p=Start-Process -FilePath $Exe -ArgumentList @('--udf',('"'+$root+'"'),'--no-agent','--no-activate','--no-spare','--bounds','0,0,1024,768',"http://localhost:$port/") -PassThru
 $deadline=(Get-Date).AddSeconds(40);do{Pump 100;$p.Refresh()}while(-not $script:metric -and (Get-Date)-lt $deadline -and -not $p.HasExited)
 if(-not $script:metric){throw 'fixture not ready'}
 $h=$p.MainWindowHandle
 [TranslationLive]::Toggle($h);Pump 300;[TranslationLive]::End()
 $deadline=(Get-Date).AddSeconds(80);do{Pump 100}while(($script:metric.h -notmatch '[\u4e00-\u9fff]' -or $script:metric.d -notmatch '[\u4e00-\u9fff]') -and (Get-Date)-lt $deadline)
 if($script:metric.h -notmatch '[\u4e00-\u9fff]' -or $script:metric.d -notmatch '[\u4e00-\u9fff]'){throw ('translation failed: '+($script:metric|ConvertTo-Json -Compress))}
 if($script:metric.input -ne 'Do not translate my draft' -or $script:metric.code -ne 'const privateValue = 42;' -or $script:metric.link -ne '/keep-link'){throw 'translation changed protected content/structure'}
 'TRANSLATED '+($script:metric|ConvertTo-Json -Compress)
 [TranslationLive]::Toggle($h);Pump 300;[TranslationLive]::End()
 $deadline=(Get-Date).AddSeconds(10);do{Pump 100}while(($script:metric.h -ne 'Machine translation' -or $script:metric.status) -and (Get-Date)-lt $deadline)
 if($script:metric.h -ne 'Machine translation' -or $script:metric.p -ne 'Neural machine translation uses artificial intelligence.' -or $script:metric.d -ne 'This sentence was added later.' -or $script:metric.status){throw 'restore failed'}
 'PASS native Alt+T -> real Google -> DOM + dynamic content; link/input/code preserved; second Alt+T restores original'
 [TranslationLive]::PostMessageW($h,0x10,[IntPtr]::Zero,[IntPtr]::Zero)|Out-Null
 if(-not $p.WaitForExit(15000)){throw 'fixture did not close'}
}finally{[TranslationLive]::End();if($p -and -not $p.HasExited){Stop-Process -Id $p.Id -Force};$server.Stop();Start-Sleep -Milliseconds 500;Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue}
