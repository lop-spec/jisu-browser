param([Parameter(Mandatory)][string]$Exe)
$ErrorActionPreference='Stop'
$env:PSModulePath=(Join-Path $PSHOME 'Modules')+';'+(Join-Path $env:ProgramFiles 'WindowsPowerShell/Modules')
Import-Module Microsoft.PowerShell.Utility,Microsoft.PowerShell.Management
$c=New-Object CodeDom.Compiler.CompilerParameters;$c.GenerateInMemory=$true;$c.TempFiles=New-Object CodeDom.Compiler.TempFileCollection([IO.Path]::GetTempPath(),$false)
Add-Type -CompilerParameters $c -TypeDefinition @'
using System;using System.Collections.Generic;using System.Runtime.InteropServices;using System.Text;
public static class WindowSessionTest {
 public delegate bool EnumProc(IntPtr h,IntPtr l);
 [DllImport("user32.dll")]static extern bool EnumWindows(EnumProc f,IntPtr l);
 [DllImport("user32.dll")]static extern uint GetWindowThreadProcessId(IntPtr h,out uint p);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)]static extern int GetClassNameW(IntPtr h,StringBuilder b,int n);
 [DllImport("user32.dll")]public static extern bool PostMessageW(IntPtr h,uint m,IntPtr w,IntPtr l);
 [DllImport("user32.dll")]public static extern IntPtr SendMessageTimeoutW(IntPtr h,uint m,IntPtr w,IntPtr l,uint flags,uint timeout,out IntPtr result);
 public static IntPtr[] Windows(uint pid){var rows=new List<IntPtr>();EnumWindows((h,l)=>{uint p;GetWindowThreadProcessId(h,out p);var b=new StringBuilder(128);GetClassNameW(h,b,128);if(p==pid&&b.ToString()=="JisuBrowserWindow")rows.Add(h);return true;},IntPtr.Zero);return rows.ToArray();}
}
'@
$root=Join-Path $env:TEMP ('jisu-windows-'+[guid]::NewGuid().ToString('N'));New-Item -ItemType Directory -Path $root|Out-Null
[IO.File]::WriteAllText((Join-Path $root 'jisu-settings.json'),' {"bookmarkBar":false,"importedDefaultBrowser":true}')
$port=Get-Random -Minimum 21000 -Maximum 44000
$server=New-Object Net.HttpListener;$server.Prefixes.Add("http://localhost:$port/");$server.Start();$script:pending=$server.GetContextAsync();$script:served=New-Object System.Collections.Generic.List[string]
function Pump([int]$ms){
 $until=(Get-Date).AddMilliseconds($ms)
 do{if($script:pending.Wait(30)){$x=$script:pending.Result;$script:pending=$server.GetContextAsync();$script:served.Add($x.Request.Url.AbsolutePath);$bytes=[Text.Encoding]::UTF8.GetBytes('<!doctype html><title>window-'+$x.Request.Url.AbsolutePath+'</title><p>window</p>');$x.Response.ContentType='text/html';try{$x.Response.OutputStream.Write($bytes,0,$bytes.Length);$x.Response.Close()}catch{}}}while((Get-Date)-lt $until)
}
$session=Join-Path $root 'Session.json'
$seed=@{clean=$false;tabs=@(@{url="http://localhost:$port/a";title='a';window=0;active=$true;pinned=$false},@{url="http://localhost:$port/b";title='b';window=1;active=$true;pinned=$false})}|ConvertTo-Json -Depth 5
[IO.File]::WriteAllText($session,$seed)
$process=$null
try{
 $process=Start-Process -FilePath $Exe -ArgumentList @('--udf',('"'+$root+'"'),'--no-agent','--no-activate','--no-spare','--bounds','-3000,0,900,700') -PassThru
 $deadline=(Get-Date).AddSeconds(40)
 do{Pump 100;$windows=@([WindowSessionTest]::Windows([uint32]$process.Id))}while(($windows.Count -ne 2 -or -not ($script:served -contains '/a') -or -not ($script:served -contains '/b')) -and (Get-Date)-lt $deadline -and -not $process.HasExited)
 if($windows.Count -ne 2 -or $process.HasExited){throw 'saved windows did not restore into two native windows'}
 Pump 2000
 $before=Get-Content -LiteralPath $session -Raw -Encoding UTF8|ConvertFrom-Json
 if(@($before.tabs).Count -ne 2 -or @($before.tabs.window|Sort-Object -Unique).Count -ne 2 -or @($before.tabs|Where-Object {$_.active}).Count -ne 2){throw 'restored window groups/active flags were flattened'}
 [IntPtr]$result=[IntPtr]::Zero
 if([WindowSessionTest]::SendMessageTimeoutW($windows[0],0x16,[IntPtr]1,[IntPtr]::Zero,2,10000,[ref]$result) -eq [IntPtr]::Zero){throw 'end-session preparation failed'}
 # Deliberately close one window and wait longer than the session debounce: it must not overwrite the full shutdown snapshot.
 [WindowSessionTest]::PostMessageW($windows[0],0x10,[IntPtr]::Zero,[IntPtr]::Zero)|Out-Null
 Pump 2300
 [WindowSessionTest]::PostMessageW($windows[1],0x10,[IntPtr]::Zero,[IntPtr]::Zero)|Out-Null
 if(-not $process.WaitForExit(15000)){throw 'multi-window process did not exit'}
 $after=Get-Content -LiteralPath $session -Raw -Encoding UTF8|ConvertFrom-Json
 if($after.clean -or @($after.tabs).Count -ne 2 -or @($after.tabs.window|Sort-Object -Unique).Count -ne 2){throw 'shutdown lost a window or marked the session clean'}
 'PASS two native windows restored; current tab per window kept; delayed close cannot overwrite complete shutdown session'
}finally{
 if($process -and -not $process.HasExited){Stop-Process -Id $process.Id -Force}
 $server.Stop();Start-Sleep -Milliseconds 500;Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
}
