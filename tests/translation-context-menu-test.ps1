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
 [DllImport("user32.dll")]public static extern bool PostMessageW(IntPtr h,uint m,IntPtr w,IntPtr l);
 public static void RightClick(IntPtr root){IntPtr render=IntPtr.Zero;EnumChildWindows(root,(h,l)=>{var b=new StringBuilder(128);GetClassNameW(h,b,128);if(b.ToString()=="Chrome_RenderWidgetHostHWND")render=h;return true;},IntPtr.Zero);if(render==IntPtr.Zero)throw new Exception("render HWND missing");var p=(IntPtr)((300<<16)|400);PostMessageW(render,0x200,IntPtr.Zero,p);PostMessageW(render,0x204,(IntPtr)2,p);Thread.Sleep(50);PostMessageW(render,0x205,IntPtr.Zero,p);}
 public static IntPtr[] Windows(){var result=new List<IntPtr>();EnumWindows((h,l)=>{if(IsWindowVisible(h))result.Add(h);return true;},IntPtr.Zero);return result.ToArray();}
}
'@
$root=Join-Path $env:TEMP ('jisu-context-translation-'+[guid]::NewGuid().ToString('N'));New-Item -ItemType Directory -Path $root|Out-Null
[IO.File]::WriteAllText((Join-Path $root 'jisu-settings.json'),'{"bookmarkBar":false,"importedDefaultBrowser":true}')
$port=Get-Random -Minimum 23000 -Maximum 44000
$server=New-Object Net.HttpListener;$server.Prefixes.Add("http://localhost:$port/");$server.Start();$script:pending=$server.GetContextAsync();$script:ready=$false;$script:started=$false
# Empty body means this native-menu test never needs an external translation provider.
$html='<!doctype html><title>translation-menu-fixture</title><body style="min-height:100vh"><script>setInterval(()=>fetch("/metric?started="+(!!document.getElementById("jisu-translation"))),100)</script>'
function Pump([int]$ms){$until=(Get-Date).AddMilliseconds($ms);do{if($script:pending.Wait(20)){$x=$script:pending.Result;$script:pending=$server.GetContextAsync();if($x.Request.Url.AbsolutePath -eq '/metric'){$script:ready=$true;$script:started=$x.Request.QueryString['started'] -eq 'true';$text='ok'}else{$text=$html};$bytes=[Text.Encoding]::UTF8.GetBytes($text);$x.Response.ContentType='text/html; charset=utf-8';try{$x.Response.OutputStream.Write($bytes,0,$bytes.Length);$x.Response.Close()}catch{}}}while((Get-Date)-lt $until)}
function Select-TranslationMenu([string]$name){
 [TranslationMenuTest]::RightClick($p.MainWindowHandle)
 $deadline=(Get-Date).AddSeconds(8);$found=$null;$names=@()
 do{Pump 100;foreach($window in [TranslationMenuTest]::Windows()){
  try{$element=[System.Windows.Automation.AutomationElement]::FromHandle($window);$items=$element.FindAll([System.Windows.Automation.TreeScope]::Subtree,(New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::ControlTypeProperty,[System.Windows.Automation.ControlType]::MenuItem)))
   foreach($item in $items){$label=$item.Current.Name;$names+=$label;if($label.Replace('&','').Split("`t")[0] -eq $name){$found=$item;break}}
  }catch{}
  if($found){break}
 }}while(-not $found -and (Get-Date)-lt $deadline)
 if(-not $found){throw ('Actual right-click menu missing '+$name+'; items='+ (($names|Select-Object -Unique) -join ', '))}
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
