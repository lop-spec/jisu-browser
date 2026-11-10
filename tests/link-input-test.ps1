param([Parameter(Mandatory)][string]$Exe,[switch]$Baseline)
$ErrorActionPreference='Stop'
$c=New-Object CodeDom.Compiler.CompilerParameters;$c.GenerateInMemory=$true;$c.TempFiles=New-Object CodeDom.Compiler.TempFileCollection([IO.Path]::GetTempPath(),$false)
Add-Type -CompilerParameters $c -TypeDefinition @'
using System;using System.Text;using System.Runtime.InteropServices;using System.Threading;
public static class LinkInputTest {
 public delegate bool EnumProc(IntPtr h,IntPtr l);
 [DllImport("user32.dll")]static extern bool EnumChildWindows(IntPtr h,EnumProc f,IntPtr l);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)]static extern int GetClassNameW(IntPtr h,StringBuilder b,int n);
 [DllImport("user32.dll")]public static extern bool PostMessageW(IntPtr h,uint m,IntPtr w,IntPtr l);
 [DllImport("user32.dll")]static extern uint GetWindowThreadProcessId(IntPtr h,out uint p);
 [DllImport("kernel32.dll")]static extern uint GetCurrentThreadId();
 [DllImport("user32.dll",SetLastError=true)]static extern bool AttachThreadInput(uint a,uint b,bool attach);
 [DllImport("user32.dll")]static extern bool GetKeyboardState(byte[] state);
 [DllImport("user32.dll")]static extern bool SetKeyboardState(byte[] state);
 public static void Click(IntPtr root,bool ctrl,bool middle){
  IntPtr target=IntPtr.Zero;EnumChildWindows(root,(h,l)=>{var b=new StringBuilder(128);GetClassNameW(h,b,128);if(b.ToString()=="Chrome_RenderWidgetHostHWND")target=h;return true;},IntPtr.Zero);
  if(target==IntPtr.Zero)throw new Exception("render input HWND missing");
  uint pid;uint thread=GetWindowThreadProcessId(target,out pid),self=GetCurrentThreadId();bool attached=false;var old=new byte[256];
  try{
   if(ctrl&&thread!=self){attached=AttachThreadInput(self,thread,true);if(!attached)throw new Exception("cannot attach private desktop input queues: "+Marshal.GetLastWin32Error());}
   GetKeyboardState(old);var keys=(byte[])old.Clone();if(ctrl){keys[0x11]=0x80;keys[0xA2]=0x80;SetKeyboardState(keys);}
   var p=(IntPtr)((35<<16)|35);var flags=ctrl?8:0;
   PostMessageW(target,0x200,(IntPtr)flags,p);
   PostMessageW(target,middle?0x207u:0x201u,(IntPtr)(flags+(middle?16:1)),p);Thread.Sleep(60);
   PostMessageW(target,middle?0x208u:0x202u,(IntPtr)flags,p);Thread.Sleep(20);
  }finally{SetKeyboardState(old);if(attached)AttachThreadInput(self,thread,false);}
 }
}
'@
$root=Join-Path $env:TEMP ('jisu-link-input-'+[guid]::NewGuid().ToString('N'));New-Item -ItemType Directory -Path $root|Out-Null
$port=Get-Random -Minimum 23000 -Maximum 44000
$server=New-Object Net.HttpListener;$server.Prefixes.Add("http://localhost:$port/");$server.Start();$script:pending=$server.GetContextAsync();$script:inputs=@{};$script:opened=@{}
function Pump([int]$ms){$until=(Get-Date).AddMilliseconds($ms);do{if($script:pending.Wait(20)){
 $x=$script:pending.Result;$script:pending=$server.GetContextAsync();$path=$x.Request.Url.AbsolutePath;$case=$x.Request.QueryString['case']
 if($path -eq '/input'){$script:inputs[$case]=@{button=$x.Request.QueryString['button'];ctrl=$x.Request.QueryString['ctrl']};$text='ok'}
 elseif($path -eq '/opened'){$script:opened[$case]=$true;$text='<!doctype html><title>opened</title>'}
 else{$text='<!doctype html><title>source</title><body style="margin:0"><a target="_blank" href="/opened?case='+$case+'" style="display:block;padding:30px">open</a><script>addEventListener("pointerdown",e=>fetch("/input?case='+$case+'&button="+e.button+"&ctrl="+(e.ctrlKey?1:0)),true)</script>'}
 $bytes=[Text.Encoding]::UTF8.GetBytes($text);$x.Response.ContentType='text/html';try{$x.Response.OutputStream.Write($bytes,0,$bytes.Length);$x.Response.Close()}catch{}
}}while((Get-Date)-lt $until)}
$process=$null
try{
 foreach($case in @('middle','ctrl')){
  $udf=Join-Path $root $case;New-Item -ItemType Directory -Path $udf|Out-Null
  [IO.File]::WriteAllText((Join-Path $udf 'jisu-settings.json'),'{"bookmarkBar":false,"importedDefaultBrowser":true}')
  $process=Start-Process -FilePath $Exe -ArgumentList @('--udf',('"'+$udf+'"'),'--no-agent','--no-activate','--no-spare','--bounds','-3000,0,1024,768',"http://localhost:$port/source?case=$case") -PassThru
  $deadline=(Get-Date).AddSeconds(35);do{Pump 100;$process.Refresh()}while($process.MainWindowTitle -notlike 'source*' -and (Get-Date)-lt $deadline -and -not $process.HasExited)
  if($process.MainWindowTitle -notlike 'source*'){throw 'source page did not load'}
  [LinkInputTest]::Click($process.MainWindowHandle,($case -eq 'ctrl'),($case -eq 'middle'))
  $deadline=(Get-Date).AddSeconds(8);do{Pump 100}while(-not $script:opened.ContainsKey($case) -and (Get-Date)-lt $deadline)
  if(-not $script:opened.ContainsKey($case)){throw ($case+' platform gesture did not open a link')}
  $input=$script:inputs[$case]
  if(-not $input -or ($case -eq 'middle' -and $input.button -ne '1') -or ($case -eq 'ctrl' -and $input.ctrl -ne '1')){throw ($case+' DOM trusted-input positive control failed')}
  Pump 1900;$session=Get-Content -LiteralPath (Join-Path $udf 'Session.json') -Raw -Encoding UTF8|ConvertFrom-Json
  $active=@($session.tabs|Where-Object {$_.active})[0].url
  "INPUT case=$case button=$($input.button) ctrl=$($input.ctrl) sourceStayedActive=$($active -like '*source*')"
  if(-not $Baseline -and $active -notlike '*source*'){throw ($case+' activated the new tab after original input modifiers were released')}
  [LinkInputTest]::PostMessageW($process.MainWindowHandle,0x10,[IntPtr]::Zero,[IntPtr]::Zero)|Out-Null
  if(-not $process.WaitForExit(15000)){throw 'fixture did not close'}
 }
 if(-not $Baseline){'PASS actual WebView HWND middle/Ctrl input, DOM modifier positive controls, background links after release; no system SendInput'}
}finally{
 if($process -and -not $process.HasExited){Stop-Process -Id $process.Id -Force}
 $server.Stop();Start-Sleep -Milliseconds 500;Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
}
