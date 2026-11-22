param([Parameter(Mandatory)][string]$Exe)
$ErrorActionPreference='Stop'
[Console]::OutputEncoding=New-Object Text.UTF8Encoding($false)
Add-Type -AssemblyName UIAutomationClient,UIAutomationTypes
$c=New-Object CodeDom.Compiler.CompilerParameters;$c.GenerateInMemory=$true
Add-Type -CompilerParameters $c -TypeDefinition @'
using System;using System.Text;using System.Collections.Generic;using System.Runtime.InteropServices;using System.Threading;
public static class TranslationMenuTest {
 delegate bool EnumProc(IntPtr h,IntPtr l);
 [DllImport("user32.dll")]static extern bool EnumChildWindows(IntPtr h,EnumProc f,IntPtr l);
 [DllImport("user32.dll")]static extern bool EnumWindows(EnumProc f,IntPtr l);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)]static extern int GetClassNameW(IntPtr h,StringBuilder b,int n);
 [DllImport("user32.dll")]static extern bool IsWindowVisible(IntPtr h);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)]static extern int GetWindowTextW(IntPtr h,StringBuilder b,int n);
 public static string Describe(IntPtr h){var cls=new StringBuilder(128);var title=new StringBuilder(512);GetClassNameW(h,cls,128);GetWindowTextW(h,title,512);return h.ToInt64()+" "+cls+" "+title;}
 [DllImport("user32.dll")]public static extern bool PostMessageW(IntPtr h,uint m,IntPtr w,IntPtr l);
 [StructLayout(LayoutKind.Sequential)]struct RECT {public int left,top,right,bottom;}
 [DllImport("user32.dll")]static extern bool GetClientRect(IntPtr h,out RECT rect);
 [DllImport("user32.dll")]static extern uint GetWindowThreadProcessId(IntPtr h,out uint pid);
 [DllImport("kernel32.dll")]static extern uint GetCurrentThreadId();
 [DllImport("user32.dll")]static extern bool AttachThreadInput(uint source,uint target,bool attach);
 [DllImport("user32.dll")]static extern IntPtr SetFocus(IntPtr h);
 public static void RightClick(IntPtr root){IntPtr render=IntPtr.Zero;EnumChildWindows(root,(h,l)=>{var b=new StringBuilder(128);GetClassNameW(h,b,128);if(b.ToString()=="Chrome_RenderWidgetHostHWND"){RECT r;GetClientRect(h,out r);Console.WriteLine("RENDER "+h+" visible="+IsWindowVisible(h)+" size="+r.right+"x"+r.bottom);if(IsWindowVisible(h)&&r.right>35&&r.bottom>35)render=h;}return true;},IntPtr.Zero);if(render==IntPtr.Zero)throw new Exception("render input HWND missing");uint pid;var target=GetWindowThreadProcessId(render,out pid);var source=GetCurrentThreadId();bool attached=target!=source&&AttachThreadInput(source,target,true);try{SetFocus(render);var p=(IntPtr)((35<<16)|35);PostMessageW(render,0x200,IntPtr.Zero,p);PostMessageW(render,0x204,(IntPtr)2,p);Thread.Sleep(80);PostMessageW(render,0x205,IntPtr.Zero,p);Thread.Sleep(80);}finally{if(attached)AttachThreadInput(source,target,false);}}
 public static IntPtr[] Windows(){var result=new List<IntPtr>();EnumWindows((h,l)=>{if(IsWindowVisible(h))result.Add(h);return true;},IntPtr.Zero);return result.ToArray();}
}
'@
$root=Join-Path $env:TEMP ('jisu-context-translation-'+[guid]::NewGuid().ToString('N'));New-Item -ItemType Directory -Path $root|Out-Null
[IO.File]::WriteAllText((Join-Path $root 'jisu-settings.json'),'{"bookmarkBar":false,"importedDefaultBrowser":true}')
$port=Get-Random -Minimum 23000 -Maximum 44000
$server=New-Object Net.HttpListener;$server.Prefixes.Add("http://localhost:$port/");$server.Start();$script:pending=$server.GetContextAsync();$script:ready=$false;$script:started=$false;$script:context=0
# A painted code block is a real hit-test target but is excluded from translation: no external provider needed.
$html='<!doctype html><title>translation-menu-fixture</title><body style="margin:0"><pre style="margin:0;padding:30px;min-height:100vh;background:#f5f5f5">fixture</pre><script>let contexts=0,ready=false;addEventListener("contextmenu",()=>contexts++);addEventListener("pointerdown",e=>fetch("/input?button="+e.button));requestAnimationFrame(()=>requestAnimationFrame(()=>ready=true));setInterval(()=>{if(ready)fetch("/metric?context="+contexts+"&started="+(!!document.getElementById("jisu-translation")))},100)</script>'
function Pump([int]$ms){$until=(Get-Date).AddMilliseconds($ms);do{if($script:pending.Wait(20)){$x=$script:pending.Result;$script:pending=$server.GetContextAsync();if($x.Request.Url.AbsolutePath -eq '/input'){Write-Output ('DOM INPUT '+$x.Request.Url.Query);$text='ok'}elseif($x.Request.Url.AbsolutePath -eq '/metric'){$script:ready=$true;$script:started=$x.Request.QueryString['started'] -eq 'true';$script:context=[int]$x.Request.QueryString['context'];$text='ok'}else{$text=$html};$bytes=[Text.Encoding]::UTF8.GetBytes($text);$x.Response.ContentType='text/html; charset=utf-8';try{$x.Response.OutputStream.Write($bytes,0,$bytes.Length);$x.Response.Close()}catch{}}}while((Get-Date)-lt $until)}
function Select-TranslationMenu([string]$name){
 [TranslationMenuTest]::RightClick($p.MainWindowHandle)
 $deadline=(Get-Date).AddSeconds(8);$found=$null;$names=@()
 do{Pump 100;foreach($window in [TranslationMenuTest]::Windows()){
  try{$element=[System.Windows.Automation.AutomationElement]::FromHandle($window);$items=$element.FindAll([System.Windows.Automation.TreeScope]::Subtree,(New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::ControlTypeProperty,[System.Windows.Automation.ControlType]::MenuItem)))
   foreach($item in $items){$label=$item.Current.Name;$names+=$label;if($label.Replace('&','').Split("`t")[0] -eq $name){$found=$item;break}}
  }catch{Write-Output ('UIA read error: '+$_.Exception.Message)}
  if($found){break}
 }}while(-not $found -and (Get-Date)-lt $deadline)
 if(-not $found){foreach($window in [TranslationMenuTest]::Windows()){
  Write-Output ('WINDOW '+[TranslationMenuTest]::Describe($window))
  try{$element=[System.Windows.Automation.AutomationElement]::FromHandle($window);$nodes=$element.FindAll([System.Windows.Automation.TreeScope]::Subtree,[System.Windows.Automation.Condition]::TrueCondition);for($i=0;$i -lt [Math]::Min($nodes.Count,60);$i++){Write-Output ('NODE '+$nodes[$i].Current.ControlType.ProgrammaticName+': '+$nodes[$i].Current.Name)}}catch{Write-Output $_.Exception.Message}
 };throw ('Actual right-click menu missing '+$name+'; DOM context events='+$script:context+'; items='+ (($names|Select-Object -Unique) -join ', '))}
 $found.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
}
$p=$null
try{
 $p=Start-Process -FilePath $Exe -ArgumentList @('--udf',('"'+$root+'"'),'--no-agent','--no-activate','--no-spare','--bounds','0,0,1024,768',"http://localhost:$port/") -PassThru
 $deadline=(Get-Date).AddSeconds(35);do{Pump 100;$p.Refresh()}while(-not $script:ready -and (Get-Date)-lt $deadline -and -not $p.HasExited)
 if(-not $script:ready){throw 'fixture not ready'}
 $translate=-join [char[]]@(32763,35793,25104,20013,25991);$restore=-join [char[]]@(24674,22797,21407,25991)
 Select-TranslationMenu $translate
 $deadline=(Get-Date).AddSeconds(8);do{Pump 100}while(-not $script:started -and (Get-Date)-lt $deadline)
 if(-not $script:started){throw 'Right-click Translate did not start page translator'}
 Select-TranslationMenu $restore
 $deadline=(Get-Date).AddSeconds(8);do{Pump 100}while($script:started -and (Get-Date)-lt $deadline)
 if($script:started){throw 'Right-click Restore did not restore the page'}
 'PASS actual WebView right-click -> Translate menu -> page translator; second right-click -> Restore menu -> restored; no external network required'
 [TranslationMenuTest]::PostMessageW($p.MainWindowHandle,0x10,[IntPtr]::Zero,[IntPtr]::Zero)|Out-Null
 if(-not $p.WaitForExit(15000)){throw 'fixture did not close'}
}finally{if($p -and -not $p.HasExited){Stop-Process -Id $p.Id -Force};$server.Stop();Start-Sleep -Milliseconds 500;Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue}
