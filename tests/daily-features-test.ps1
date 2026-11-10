param([Parameter(Mandatory)][string]$Exe,[switch]$Baseline)
$ErrorActionPreference='Stop'
$env:PSModulePath=(Join-Path $PSHOME 'Modules')+';'+(Join-Path $env:ProgramFiles 'WindowsPowerShell/Modules')
Import-Module Microsoft.PowerShell.Utility,Microsoft.PowerShell.Management
$compiler=New-Object CodeDom.Compiler.CompilerParameters
$compiler.GenerateInMemory=$true
$compiler.TempFiles=New-Object CodeDom.Compiler.TempFileCollection([IO.Path]::GetTempPath(),$false)
Add-Type -CompilerParameters $compiler -TypeDefinition @'
using System; using System.Runtime.InteropServices; using System.Text;
public static class DailyTest {
 public delegate bool EnumProc(IntPtr h,IntPtr l);
 [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr h,EnumProc f,IntPtr l);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] static extern int GetClassNameW(IntPtr h,StringBuilder b,int n);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern IntPtr GetPropW(IntPtr h,string name);
 [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h,uint m,IntPtr w,IntPtr l);
 [StructLayout(LayoutKind.Sequential)] public struct COPY {public IntPtr data;public int bytes;public IntPtr ptr;}
 [DllImport("user32.dll")] static extern IntPtr SendMessageW(IntPtr h,uint m,IntPtr w,ref COPY d);
 public static void Open(IntPtr h,string url) {var p=Marshal.StringToHGlobalUni(url);try{var d=new COPY{data=(IntPtr)0x4A495355,bytes=(url.Length+1)*2,ptr=p};SendMessageW(h,0x4A,IntPtr.Zero,ref d);}finally{Marshal.FreeHGlobal(p);}}
 public static bool ClickPage(IntPtr h) {IntPtr target=IntPtr.Zero;EnumChildWindows(h,(c,l)=>{var b=new StringBuilder(128);GetClassNameW(c,b,128);if(b.ToString()=="Chrome_RenderWidgetHostHWND")target=c;return true;},IntPtr.Zero);if(target==IntPtr.Zero)return false;var point=(IntPtr)((35<<16)|35);PostMessageW(target,0x200,IntPtr.Zero,point);PostMessageW(target,0x201,(IntPtr)1,point);PostMessageW(target,0x202,IntPtr.Zero,point);return true;}
}
'@
$root=Join-Path $env:TEMP ('jisu-daily-'+[guid]::NewGuid().ToString('N'))
$downloads=Join-Path $root 'downloads';New-Item -ItemType Directory -Path $downloads -Force|Out-Null
$settings=@{bookmarkBar=$false;importedDefaultBrowser=$true;downloadDirectory=$downloads;askDownload=$false}|ConvertTo-Json
[IO.File]::WriteAllText((Join-Path $root 'jisu-settings.json'),$settings)
$port=Get-Random -Minimum 20000 -Maximum 45000
$server=New-Object Net.HttpListener;$server.Prefixes.Add("http://localhost:$port/");$server.Start();$script:pending=$server.GetContextAsync();$script:payloads=0
$filename='jisu-audit-'+[guid]::NewGuid().ToString('N')+'.txt'
function Pump([int]$ms){
 $until=(Get-Date).AddMilliseconds($ms)
 do{
  if($script:pending.Wait(25)){
   $c=$script:pending.Result;$script:pending=$server.GetContextAsync();$path=$c.Request.Url.AbsolutePath
   if($path -eq '/payload'){$script:payloads++;$c.Response.ContentType='application/octet-stream';$c.Response.Headers.Add('Content-Disposition','attachment; filename="'+$filename+'"');$text='jisu-download-fixture'}
   else{$c.Response.ContentType='text/html';$text='<!doctype html><title>audit-'+$path.TrimStart('/')+'</title><body style="margin:0"><a target="_blank" href="/payload" style="display:block;padding:30px">download</a>'}
   $bytes=[Text.Encoding]::UTF8.GetBytes($text);$c.Response.ContentLength64=$bytes.Length
   try{$c.Response.OutputStream.Write($bytes,0,$bytes.Length);$c.Response.Close()}catch{}
  }
 }while((Get-Date)-lt $until)
}
$process=$null
try{
 $process=Start-Process -FilePath $Exe -ArgumentList @('--udf',('"'+$root+'"'),'--no-agent','--no-activate','--no-spare','--bounds','-3000,0,1024,768',"http://localhost:$port/a") -PassThru
 $deadline=(Get-Date).AddSeconds(35)
 do{Pump 100;$process.Refresh()}while($process.MainWindowTitle -notlike 'audit-a*' -and (Get-Date)-lt $deadline -and -not $process.HasExited)
 if($process.HasExited -or $process.MainWindowTitle -notlike 'audit-a*'){throw 'initial page did not load'}
 $h=$process.MainWindowHandle
 [DailyTest]::Open($h,"http://localhost:$port/b");Pump 2500
 [DailyTest]::PostMessageW($h,0x8001,[IntPtr]19,[IntPtr]1)|Out-Null;Pump 300
 [DailyTest]::Open($h,"http://localhost:$port/c");Pump 2500
 $session=Get-Content -LiteralPath (Join-Path $root 'Session.json') -Raw -Encoding UTF8|ConvertFrom-Json
 $order=@($session.tabs|ForEach-Object{([Uri]$_.url).AbsolutePath}) -join ','
 if(-not $Baseline -and $order -ne '/a,/c,/b'){throw "new-tab order: $order"}
 "ORDER $order"
 if(-not [DailyTest]::ClickPage($h)){throw 'render widget unavailable for download gesture'}
 Pump 6500
 $record=Join-Path $root 'Downloads.json'
 "DOWNLOAD requests=$script:payloads record=$(Test-Path -LiteralPath $record) strip=$([DailyTest]::GetPropW($h,'JisuBrowser.StripTabs').ToInt64()) session=$((Get-Content -LiteralPath (Join-Path $root 'Session.json') -Raw -Encoding UTF8))"
 if(-not $Baseline){
  if(-not (Test-Path -LiteralPath $record)){throw 'download record missing'}
  $rows=(Get-Content -LiteralPath $record -Raw -Encoding UTF8|ConvertFrom-Json).rows
  if(-not @($rows|Where-Object {$_.state -eq 2 -and (Test-Path -LiteralPath $_.path)}).Count){throw 'real download did not complete'}
  [DailyTest]::Open($h,'jisu://downloads');Pump 2000;$process.Refresh()
  if($process.MainWindowTitle -notlike '*downloads*' -and $process.MainWindowTitle -notlike ('*'+[char]0x4E0B+[char]0x8F7D+'*')){throw 'downloads internal page failed'}
  'PASS native right-of-current order, real target-blank download, persisted record, internal download page'
 }
 $process.CloseMainWindow()|Out-Null
 if(-not $process.WaitForExit(15000)){throw 'browser did not close'}
} finally {
 if($process -and -not $process.HasExited){Stop-Process -Id $process.Id -Force}
 $server.Stop()
 $probe=Join-Path (Join-Path $env:USERPROFILE 'Downloads') $filename
 if(Test-Path -LiteralPath $probe){Remove-Item -LiteralPath $probe -Force}
 Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
}
