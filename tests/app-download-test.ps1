param([Parameter(Mandatory)][string]$Exe)
$ErrorActionPreference='Stop'
$c=New-Object CodeDom.Compiler.CompilerParameters;$c.GenerateInMemory=$true;$c.TempFiles=New-Object CodeDom.Compiler.TempFileCollection([IO.Path]::GetTempPath(),$false)
Add-Type -CompilerParameters $c -TypeDefinition @'
using System;using System.Text;using System.Runtime.InteropServices;
public static class AppDownloadTest {
 public delegate bool EnumProc(IntPtr h,IntPtr l);
 [DllImport("user32.dll")]static extern bool EnumChildWindows(IntPtr h,EnumProc f,IntPtr l);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)]static extern int GetClassNameW(IntPtr h,StringBuilder b,int n);
 [DllImport("user32.dll")]public static extern bool PostMessageW(IntPtr h,uint m,IntPtr w,IntPtr l);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)]public static extern IntPtr GetPropW(IntPtr h,string name);
 public static bool Click(IntPtr h){IntPtr target=IntPtr.Zero;EnumChildWindows(h,(c,l)=>{var b=new StringBuilder(128);GetClassNameW(c,b,128);if(b.ToString()=="Chrome_RenderWidgetHostHWND")target=c;return true;},IntPtr.Zero);if(target==IntPtr.Zero)return false;var p=(IntPtr)((35<<16)|35);PostMessageW(target,0x200,IntPtr.Zero,p);PostMessageW(target,0x201,(IntPtr)1,p);PostMessageW(target,0x202,IntPtr.Zero,p);return true;}
}
'@
$root=Join-Path $env:TEMP ('jisu-app-download-'+[guid]::NewGuid().ToString('N'));$downloads=Join-Path $root 'downloads';New-Item -ItemType Directory -Path $downloads -Force|Out-Null
[IO.File]::WriteAllText((Join-Path $root 'jisu-settings.json'),(@{downloadDirectory=$downloads;askDownload=$false}|ConvertTo-Json))
$port=Get-Random -Minimum 23000 -Maximum 44000;$server=New-Object Net.HttpListener;$server.Prefixes.Add("http://localhost:$port/");$server.Start();$script:pending=$server.GetContextAsync();$script:ready=$false;$script:slow=$null;$script:offset=0;$script:next=Get-Date;$fixture=New-Object byte[] 262144
function Pump([int]$ms){$until=(Get-Date).AddMilliseconds($ms);do{
 if($script:pending.Wait(20)){$x=$script:pending.Result;$script:pending=$server.GetContextAsync();if($x.Request.Url.AbsolutePath -eq '/ready'){$script:ready=$true};if($x.Request.Url.AbsolutePath -eq '/payload'){$x.Response.ContentType='application/octet-stream';$x.Response.Headers.Add('Content-Disposition','attachment; filename="app-fixture.txt"');$x.Response.ContentLength64=$fixture.Length;$script:slow=$x.Response}else{$bytes=[Text.Encoding]::UTF8.GetBytes('<!doctype html><body style="margin:0"><a href="/payload" download style="display:block;padding:30px">download</a><script>fetch("/ready")</script>');$x.Response.ContentType='text/html';try{$x.Response.OutputStream.Write($bytes,0,$bytes.Length);$x.Response.Close()}catch{}}}
 if($script:slow -and (Get-Date)-ge $script:next){try{$n=[Math]::Min(8192,$fixture.Length-$script:offset);$script:slow.OutputStream.Write($fixture,$script:offset,$n);$script:slow.OutputStream.Flush();$script:offset+=$n;$script:next=(Get-Date).AddMilliseconds(40);if($script:offset -eq $fixture.Length){$script:slow.Close();$script:slow=$null}}catch{$script:slow=$null}}
}while((Get-Date)-lt $until)}
$p=$null
try{
 $p=Start-Process -FilePath $Exe -ArgumentList @('--udf',('"'+$root+'"'),'--app',"http://localhost:$port/",'--app-id','audit-download','--app-title','app-download-fixture','--no-activate','--bounds','-3000,0,900,700') -PassThru
 $deadline=(Get-Date).AddSeconds(35);do{Pump 100;$p.Refresh()}while(-not $script:ready -and (Get-Date)-lt $deadline -and -not $p.HasExited)
 if(-not $script:ready){throw 'application page did not become ready'}
 $p.Refresh();if($p.HasExited -or -not [AppDownloadTest]::Click($p.MainWindowHandle)){throw 'application render input unavailable'}
 Pump 5000
 $record=Join-Path $root 'Downloads-app-audit-download.json';if(-not (Test-Path -LiteralPath $record)){throw 'per-app download record missing'}
 $rows=(Get-Content -LiteralPath $record -Raw -Encoding UTF8|ConvertFrom-Json).rows
 if(-not @($rows|Where-Object {$_.state -eq 2 -and (Test-Path -LiteralPath $_.path) -and (Get-Item -LiteralPath $_.path).Length -eq $fixture.Length}).Count){throw 'application download did not complete'}
 if([AppDownloadTest]::GetPropW($p.MainWindowHandle,'JisuBrowser.DownloadDialogSeen').ToInt64() -ne 1){throw 'actual SDK floating download UI was not shown'}
 if(Test-Path -LiteralPath (Join-Path $root 'Downloads.json')){throw 'application overwrote the daily download list'}
 'PASS application: actual SDK floating download UI seen; complete 256KiB download; app-id record isolated; no daily download overwrite'
 [AppDownloadTest]::PostMessageW($p.MainWindowHandle,0x10,[IntPtr]::Zero,[IntPtr]::Zero)|Out-Null;if(-not $p.WaitForExit(15000)){throw 'application did not close'}
}finally{if($p -and -not $p.HasExited){Stop-Process -Id $p.Id -Force};$server.Stop();Start-Sleep -Milliseconds 500;Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue}
