param([Parameter(Mandatory)][string]$Exe)
$ErrorActionPreference='Stop'
$c=New-Object CodeDom.Compiler.CompilerParameters;$c.GenerateInMemory=$true;$c.TempFiles=New-Object CodeDom.Compiler.TempFileCollection([IO.Path]::GetTempPath(),$false)
Add-Type -CompilerParameters $c -TypeDefinition @'
using System;using System.Text;using System.Runtime.InteropServices;
public static class ProtocolIsolationTest {
 public delegate bool EnumProc(IntPtr h,IntPtr l);
 [DllImport("user32.dll")]static extern bool EnumChildWindows(IntPtr h,EnumProc f,IntPtr l);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)]static extern int GetClassNameW(IntPtr h,StringBuilder b,int n);
 [DllImport("user32.dll")]public static extern bool PostMessageW(IntPtr h,uint m,IntPtr w,IntPtr l);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)]static extern IntPtr SendMessageW(IntPtr h,uint m,IntPtr w,string value);
 [DllImport("user32.dll",EntryPoint="SendMessageW",CharSet=CharSet.Unicode)]static extern IntPtr ReadText(IntPtr h,uint m,IntPtr w,StringBuilder value);
 public static void Submit(IntPtr root,string url){IntPtr edit=IntPtr.Zero;EnumChildWindows(root,(h,l)=>{var b=new StringBuilder(128);GetClassNameW(h,b,128);if(b.ToString().Equals("Edit",StringComparison.OrdinalIgnoreCase))edit=h;return true;},IntPtr.Zero);if(edit==IntPtr.Zero)throw new Exception("native EDIT missing");SendMessageW(edit,0xC,IntPtr.Zero,url);var read=new StringBuilder(32768);ReadText(edit,0xD,(IntPtr)read.Capacity,read);if(read.ToString()!=url)throw new Exception("EDIT input positive control failed");PostMessageW(edit,0x100,(IntPtr)13,(IntPtr)1);}
}
'@
$root=Join-Path $env:TEMP ('jisu-protocol-isolation-'+[guid]::NewGuid().ToString('N'));New-Item -ItemType Directory -Path $root|Out-Null
[IO.File]::WriteAllText((Join-Path $root 'jisu-settings.json'),'{"bookmarkBar":false,"importedDefaultBrowser":true}')
$port=Get-Random -Minimum 23000 -Maximum 44000;$base="http://localhost:$port";$script:executed=$false;$script:seen=@{}
$server=New-Object Net.HttpListener;$server.Prefixes.Add($base+'/');$server.Start();$script:pending=$server.GetContextAsync()
function Pump([int]$ms){$until=(Get-Date).AddMilliseconds($ms);do{if($script:pending.Wait(20)){$x=$script:pending.Result;$script:pending=$server.GetContextAsync();if($x.Request.Url.AbsolutePath -eq '/executed'){$script:executed=$true};$phase=$x.Request.QueryString['phase'];if($phase){$script:seen[$phase]=$true};$bytes=[Text.Encoding]::UTF8.GetBytes('<!doctype html><title>protocol-safe</title><body>safe fixture</body>');$x.Response.ContentType='text/html';try{$x.Response.OutputStream.Write($bytes,0,$bytes.Length);$x.Response.Close()}catch{}}}while((Get-Date)-lt $until)}
$p=$null
try{
 $p=Start-Process -FilePath $Exe -ArgumentList @('--udf',('"'+$root+'"'),'--no-agent','--no-activate','--no-spare','--bounds','-3000,0,1024,768',($base+'/safe?phase=start')) -PassThru
 $deadline=(Get-Date).AddSeconds(35);do{Pump 100;$p.Refresh()}while($p.MainWindowTitle -notlike 'protocol-safe*' -and (Get-Date)-lt $deadline -and -not $p.HasExited)
 if($p.MainWindowTitle -notlike 'protocol-safe*'){throw 'positive control did not load'}
 $blocked=@('edge://crash','edge://hang','edge://quit','chrome://crash','about:crash',('data:text/html,<script>fetch("'+$base+'/executed")</script>'),('javascript:fetch("'+$base+'/executed")'),'view-source:edge://crash')
 for($i=0;$i -lt $blocked.Count;$i++){
  [ProtocolIsolationTest]::Submit($p.MainWindowHandle,$blocked[$i]);Pump 1800;$p.Refresh();if($p.HasExited){throw ('dangerous protocol exited host: '+$blocked[$i])}
  $session=Join-Path $root 'Session.json';if(Test-Path -LiteralPath $session){$data=Get-Content -LiteralPath $session -Raw -Encoding UTF8|ConvertFrom-Json;foreach($tab in $data.tabs){if($tab.url -eq $blocked[$i]){throw 'blocked address entered session'}}}
  $phase='after-'+$i;[ProtocolIsolationTest]::Submit($p.MainWindowHandle,($base+'/safe?phase='+$phase));$deadline=(Get-Date).AddSeconds(8);do{Pump 100}while(-not $script:seen.ContainsKey($phase) -and (Get-Date)-lt $deadline)
  if(-not $script:seen.ContainsKey($phase)){throw ('engine did not survive: '+$blocked[$i])}
 }
 if($script:executed){throw 'data/javascript script executed despite navigation guard'}
 if(Test-Path -LiteralPath (Join-Path $root 'Crash.log')){throw 'native crash was recorded'}
 'PASS native address entry: crash/hang/quit and data/javascript blocked; engine navigates positive control after every refusal; no script or native crash'
 [ProtocolIsolationTest]::PostMessageW($p.MainWindowHandle,0x10,[IntPtr]::Zero,[IntPtr]::Zero)|Out-Null;if(-not $p.WaitForExit(15000)){throw 'fixture did not close'}
}finally{if($p -and -not $p.HasExited){Stop-Process -Id $p.Id -Force};$server.Stop();Start-Sleep -Milliseconds 500;Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue}
