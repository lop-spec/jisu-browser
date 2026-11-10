param([Parameter(Mandatory)][string]$Exe)
$ErrorActionPreference='Stop'
$c=New-Object CodeDom.Compiler.CompilerParameters;$c.GenerateInMemory=$true;$c.TempFiles=New-Object CodeDom.Compiler.TempFileCollection([IO.Path]::GetTempPath(),$false)
Add-Type -CompilerParameters $c -TypeDefinition @'
using System;using System.Runtime.InteropServices;
public static class PrivateTest {
 public static string Hash(string path){using(var stream=System.IO.File.OpenRead(path))using(var sha=System.Security.Cryptography.SHA256.Create())return BitConverter.ToString(sha.ComputeHash(stream)).Replace("-","");}
 [DllImport("user32.dll")]public static extern bool PostMessageW(IntPtr h,uint m,IntPtr w,IntPtr l);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)]public static extern IntPtr GetPropW(IntPtr h,string name);
}
'@
$root=Join-Path $env:TEMP ('jisu-private-'+[guid]::NewGuid().ToString('N'));New-Item -ItemType Directory -Path $root|Out-Null
[IO.File]::WriteAllText((Join-Path $root 'jisu-settings.json'),'{"bookmarkBar":false,"importedDefaultBrowser":true}')
$normalName='normal_'+[guid]::NewGuid().ToString('N');$privateName='private_'+[guid]::NewGuid().ToString('N')
$port=Get-Random -Minimum 22000 -Maximum 45000
$server=New-Object Net.HttpListener;$server.Prefixes.Add("http://localhost:$port/");$server.Start();$script:pending=$server.GetContextAsync()
$script:observed=@{};$script:leaked=$false;$script:privateWorks=$false
function Pump([int]$ms){
 $until=(Get-Date).AddMilliseconds($ms)
 do{if($script:pending.Wait(25)){
  $x=$script:pending.Result;$script:pending=$server.GetContextAsync();$path=$x.Request.Url.AbsolutePath
  if($path -eq '/observed'){
   $phase=$x.Request.QueryString['phase'];$script:observed[$phase]=$true
   if($phase -eq 'private'){$script:leaked=$script:leaked -or ($x.Request.QueryString['leak'] -eq '1');$script:privateWorks=([string]$x.Request.Headers['Cookie']).Contains($privateName)}
   $text='ok'
  }elseif($path -eq '/normal' -or $path -eq '/private'){
   $phase=$path.TrimStart('/');$name=if($phase -eq 'normal'){$normalName}else{$privateName}
   if($phase -eq 'private' -and ([string]$x.Request.Headers['Cookie']).Contains($normalName)){$script:leaked=$true}
   $text='<!doctype html><title>'+ $phase +'</title><script>const leak=document.cookie.includes("'+$normalName+'");document.cookie="'+$name+'=1;max-age=3600;path=/";fetch("/observed?phase='+$phase+'&leak="+(leak?1:0));</script>'
  }else{$text='<!doctype html><p>fixture</p>'}
  $bytes=[Text.Encoding]::UTF8.GetBytes($text);$x.Response.ContentType='text/html';try{$x.Response.OutputStream.Write($bytes,0,$bytes.Length);$x.Response.Close()}catch{}
 }}while((Get-Date)-lt $until)
}
function Start-Browser([switch]$Private){
 $phase=if($Private){'private'}else{'normal'}
 $arguments=@('--udf',('"'+$root+'"'),'--no-agent','--no-activate','--no-spare','--bounds','-3000,0,900,700')
 if($Private){$arguments+='--inprivate'};$arguments+="http://localhost:$port/$phase"
 $p=Start-Process -FilePath $Exe -ArgumentList $arguments -PassThru
 $deadline=(Get-Date).AddSeconds(35)
 do{Pump 100;$p.Refresh()}while(-not $script:observed.ContainsKey($phase) -and (Get-Date)-lt $deadline -and -not $p.HasExited)
 if(-not $script:observed.ContainsKey($phase)){if(-not $p.HasExited){Stop-Process -Id $p.Id -Force};throw ($phase+' fixture did not run')}
 return $p
}
function Close-Browser($p){
 $p.Refresh();[PrivateTest]::PostMessageW($p.MainWindowHandle,0x10,[IntPtr]::Zero,[IntPtr]::Zero)|Out-Null
 if(-not $p.WaitForExit(15000)){throw 'graceful close failed'}
}
function Cookie-Contains([string]$name){
 foreach($f in Get-ChildItem -LiteralPath $root -Recurse -File -ErrorAction SilentlyContinue|Where-Object {$_.Name -like '*Cookies*'}){
  try{$s=[IO.File]::Open($f.FullName,[IO.FileMode]::Open,[IO.FileAccess]::Read,([IO.FileShare]::ReadWrite-bor [IO.FileShare]::Delete));try{$m=New-Object IO.MemoryStream;$s.CopyTo($m);if([Text.Encoding]::ASCII.GetString($m.ToArray()).Contains($name)){return $true}}finally{$s.Dispose()}}catch{}
 }
 return $false
}
$normal=$null;$private=$null
try{
 $normal=Start-Browser;Close-Browser $normal;Pump 1200
 if(-not (Cookie-Contains $normalName)){throw 'positive control: normal persistent cookie not found on disk'}
 $files=@('History.json','Session.json','jisu-settings.json','Bookmarks','Downloads.json','extensions-status.json','cookie-guard-status.json','Crash.log')
 $before=@{};foreach($name in $files){$path=Join-Path $root $name;if(Test-Path -LiteralPath $path){$before[$name]=[PrivateTest]::Hash($path)}}
 $private=Start-Browser -Private
 if([PrivateTest]::GetPropW($private.MainWindowHandle,'JisuBrowser.PrivateVerified').ToInt64() -ne 1){throw 'actual SDK Profile was not InPrivate'}
 if($script:leaked -or -not $script:privateWorks){throw 'private network cookie isolation failed'}
 Close-Browser $private;Pump 1500
 foreach($name in $files){$path=Join-Path $root $name;$after=if(Test-Path -LiteralPath $path){[PrivateTest]::Hash($path)}else{$null};if($after -ne $before[$name]){throw ('private mode changed native metadata: '+$name)}}
 if(Cookie-Contains $privateName){throw 'private persistent cookie reached a disk cookie database'}
 if(-not (Cookie-Contains $normalName)){throw 'private mode altered normal cookie store'}
 'PASS actual InPrivate Profile; normal persistent-cookie positive control; private HTTP cookie isolation; private cookie not persisted; native JSON unchanged; normal login store intact'
}finally{
 foreach($p in @($private,$normal)){if($p -and -not $p.HasExited){Stop-Process -Id $p.Id -Force}}
 $server.Stop();Start-Sleep -Milliseconds 500;Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
}
