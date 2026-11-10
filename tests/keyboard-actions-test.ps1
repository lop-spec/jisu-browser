param([Parameter(Mandatory)][string]$Exe)
$ErrorActionPreference='Stop'
[Console]::OutputEncoding=New-Object Text.UTF8Encoding($false)
Add-Type -AssemblyName UIAutomationClient,UIAutomationTypes
$c=New-Object CodeDom.Compiler.CompilerParameters;$c.GenerateInMemory=$true;$c.TempFiles=New-Object CodeDom.Compiler.TempFileCollection([IO.Path]::GetTempPath(),$false)
Add-Type -CompilerParameters $c -TypeDefinition @'
using System;using System.Text;using System.Collections.Generic;using System.Runtime.InteropServices;using System.Threading;
public static class KeyboardActionsTest {
 public delegate bool EnumProc(IntPtr h,IntPtr l);
 [DllImport("user32.dll")]static extern bool EnumWindows(EnumProc f,IntPtr l);
 [DllImport("user32.dll")]static extern bool EnumChildWindows(IntPtr h,EnumProc f,IntPtr l);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)]static extern int GetClassNameW(IntPtr h,StringBuilder b,int n);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)]static extern int GetWindowTextW(IntPtr h,StringBuilder b,int n);
 [DllImport("user32.dll")]static extern bool IsWindowVisible(IntPtr h);
 [DllImport("user32.dll")]public static extern bool PostMessageW(IntPtr h,uint m,IntPtr w,IntPtr l);
 [DllImport("user32.dll")]static extern uint GetWindowThreadProcessId(IntPtr h,out uint p);
 [DllImport("kernel32.dll")]static extern uint GetCurrentThreadId();
 [DllImport("user32.dll")]static extern bool AttachThreadInput(uint a,uint b,bool attach);
 [DllImport("user32.dll")]static extern bool GetKeyboardState(byte[] state);
 [DllImport("user32.dll")]static extern bool SetKeyboardState(byte[] state);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)]static extern IntPtr SendMessageW(IntPtr h,uint m,IntPtr w,StringBuilder text);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)]public static extern IntPtr GetPropW(IntPtr h,string name);
 public static IntPtr Edit(IntPtr root){IntPtr found=IntPtr.Zero;EnumChildWindows(root,(h,l)=>{var b=new StringBuilder(128);GetClassNameW(h,b,128);if(b.ToString().Equals("Edit",StringComparison.OrdinalIgnoreCase))found=h;return true;},IntPtr.Zero);return found;}
 public static string Address(IntPtr root){var text=new StringBuilder(32768);SendMessageW(Edit(root),0xD,(IntPtr)text.Capacity,text);return text.ToString();}
 static uint targetThread,sourceThread;static bool attached,active;static byte[] saved;
 public static void End(){if(!active)return;SetKeyboardState(saved);if(attached)AttachThreadInput(sourceThread,targetThread,false);active=attached=false;}
 public static void Key(IntPtr root,int key,bool ctrl,bool shift,bool alt){End();uint pid;targetThread=GetWindowThreadProcessId(root,out pid);sourceThread=GetCurrentThreadId();saved=new byte[256];try{if(targetThread!=sourceThread){attached=AttachThreadInput(sourceThread,targetThread,true);if(!attached)throw new Exception("input queue attach failed");}GetKeyboardState(saved);active=true;var state=new byte[256];if(ctrl)state[0x11]=0x80;if(shift)state[0x10]=0x80;if(alt)state[0x12]=0x80;SetKeyboardState(state);if(!PostMessageW(Edit(root),alt?0x104u:0x100u,(IntPtr)key,(IntPtr)1))throw new Exception("key post failed");}catch{End();throw;}}
 public static IntPtr[] Windows(uint[] pids){var result=new List<IntPtr>();EnumWindows((h,l)=>{uint p;GetWindowThreadProcessId(h,out p);if(IsWindowVisible(h)&&(pids.Length==0||Array.IndexOf(pids,p)>=0))result.Add(h);return true;},IntPtr.Zero);return result.ToArray();}
 public static string Title(IntPtr h){var b=new StringBuilder(1024);GetWindowTextW(h,b,b.Capacity);return b.ToString();}
}
'@
$root=Join-Path $env:TEMP ('jisu-keyboard-'+[guid]::NewGuid().ToString('N'));New-Item -ItemType Directory -Path $root|Out-Null
[IO.File]::WriteAllText((Join-Path $root 'jisu-settings.json'),'{"bookmarkBar":false,"importedDefaultBrowser":true}')
$port=Get-Random -Minimum 23000 -Maximum 44000;$script:dpr=0.0
$server=New-Object Net.HttpListener;$server.Prefixes.Add("http://localhost:$port/");$server.Start();$script:pending=$server.GetContextAsync()
function Pump([int]$ms){$until=(Get-Date).AddMilliseconds($ms);do{if($script:pending.Wait(20)){$x=$script:pending.Result;$script:pending=$server.GetContextAsync();if($x.Request.Url.AbsolutePath -eq '/metric'){$script:dpr=[double]::Parse($x.Request.QueryString['dpr'],[Globalization.CultureInfo]::InvariantCulture);$text='ok'}else{$text='<!doctype html><title>keyboard-source</title><body>find-fixture-word<script>setInterval(()=>fetch("/metric?dpr="+devicePixelRatio),100)</script>'};$bytes=[Text.Encoding]::UTF8.GetBytes($text);$x.Response.ContentType='text/html';try{$x.Response.OutputStream.Write($bytes,0,$bytes.Length);$x.Response.Close()}catch{}}}while((Get-Date)-lt $until)}
function Family { [uint32[]]@($p.Id)+[uint32[]]@(Get-CimInstance Win32_Process -Filter "Name='msedgewebview2.exe'"|Where-Object {$_.CommandLine -and $_.CommandLine.Contains($root)}|ForEach-Object {$_.ProcessId}) }
function New-Windows($before){$ids=Family;@([KeyboardActionsTest]::Windows($ids)|Where-Object {$_ -ne $p.MainWindowHandle -and $_ -notin $before})}
function Close-New($before){foreach($window in (New-Windows $before)){[KeyboardActionsTest]::PostMessageW($window,0x10,[IntPtr]::Zero,[IntPtr]::Zero)|Out-Null};Pump 300}
$p=$null;$private=$null
try{
 $p=Start-Process -FilePath $Exe -ArgumentList @('--udf',('"'+$root+'"'),'--no-agent','--no-activate','--no-spare','--bounds','-3000,0,1024,768',"http://localhost:$port/") -PassThru
 $deadline=(Get-Date).AddSeconds(35);do{Pump 100;$p.Refresh()}while($script:dpr -le 0 -and (Get-Date)-lt $deadline -and -not $p.HasExited);if($script:dpr -le 0){throw 'fixture did not become ready'}
 $h=$p.MainWindowHandle;$baseline=$script:dpr
 [KeyboardActionsTest]::Key($h,187,$true,$false,$false);Pump 600;if($script:dpr -le $baseline*1.05){throw 'Ctrl+plus did not change actual page DPR'}
 [KeyboardActionsTest]::Key($h,189,$true,$false,$false);Pump 600;if([Math]::Abs($script:dpr-$baseline) -gt 0.02){throw 'Ctrl+minus did not restore actual page DPR'}
 [KeyboardActionsTest]::Key($h,187,$true,$false,$false);Pump 400;[KeyboardActionsTest]::Key($h,48,$true,$false,$false);Pump 600;if([Math]::Abs($script:dpr-$baseline) -gt 0.02){throw 'Ctrl+0 failed'}
 $element=[System.Windows.Automation.AutomationElement]::FromHandle($h);$scope=[System.Windows.Automation.TreeScope]::Descendants;$condition=New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::ControlTypeProperty,[System.Windows.Automation.ControlType]::Edit)
 $beforeFind=@([KeyboardActionsTest]::Windows((Family)));$beforeEdits=0;foreach($window in $beforeFind){$view=[System.Windows.Automation.AutomationElement]::FromHandle($window);$beforeEdits+=$view.FindAll($scope,$condition).Count}
 [KeyboardActionsTest]::Key($h,70,$true,$false,$false);Pump 500;$afterEdits=0;foreach($window in [KeyboardActionsTest]::Windows((Family))){$view=[System.Windows.Automation.AutomationElement]::FromHandle($window);$afterEdits+=$view.FindAll($scope,$condition).Count}
 if($afterEdits -le $beforeEdits){
  Write-Output ('FIND edits before='+$beforeEdits+' after='+$afterEdits)
  foreach($window in [KeyboardActionsTest]::Windows((Family))){Write-Output ('WINDOW '+$window.ToInt64()+': '+[KeyboardActionsTest]::Title($window));$view=[System.Windows.Automation.AutomationElement]::FromHandle($window);$all=$view.FindAll($scope,[System.Windows.Automation.Condition]::TrueCondition);for($i=0;$i -lt [Math]::Min(80,$all.Count);$i++){Write-Output ('CONTROL '+$all[$i].Current.ControlType.ProgrammaticName+': '+$all[$i].Current.Name)}}
  throw 'Ctrl+F did not show actual Find UI edit'
 }
 Close-New $beforeFind
 [KeyboardActionsTest]::Key($h,80,$true,$false,$false);$deadline=(Get-Date).AddSeconds(10);$preview=$null
 $printName=-join [char[]]@(25171,21360);$cancelName=-join [char[]]@(21462,28040)
 do{Pump 100;$windows=$element.FindAll($scope,(New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::ControlTypeProperty,[System.Windows.Automation.ControlType]::Window)));foreach($window in $windows){if($window.Current.Name -like 'Print*' -or $window.Current.Name -like ($printName+'*')){$preview=$window}}}while(-not $preview -and (Get-Date)-lt $deadline)
 if(-not $preview){throw 'Ctrl+P actual embedded print preview missing'}
 do{Pump 100;$cancel=$preview.FindFirst($scope,(New-Object System.Windows.Automation.OrCondition((New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::NameProperty,'Cancel')),(New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::NameProperty,$cancelName)))))}while(-not $cancel -and (Get-Date)-lt $deadline)
 if(-not $cancel){throw 'print preview has no accessible cancel button'};$cancel.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke();Pump 300;'Ctrl+P: actual embedded print preview verified and cancelled'
 foreach($action in @(@{name='Ctrl+S';key=83},@{name='F12';key=123})){
  $before=@([KeyboardActionsTest]::Windows((Family)));[KeyboardActionsTest]::Key($h,$action.key,($action.key -ne 123),$false,$false);$deadline=(Get-Date).AddSeconds(10);do{Pump 100;$opened=New-Windows $before}while($opened.Count -eq 0 -and (Get-Date)-lt $deadline)
  if($action.key -eq 123){do{Pump 100;$opened=@(New-Windows $before|Where-Object {[KeyboardActionsTest]::Title($_) -like '*DevTools*'})}while($opened.Count -eq 0 -and (Get-Date)-lt $deadline)}
  if($opened.Count -eq 0){foreach($window in [KeyboardActionsTest]::Windows([uint32[]]@())){Write-Output ('DIAGNOSTIC WINDOW '+$window.ToInt64()+': '+[KeyboardActionsTest]::Title($window))};$tree=$element.FindAll($scope,[System.Windows.Automation.Condition]::TrueCondition);for($i=0;$i -lt [Math]::Min(100,$tree.Count);$i++){Write-Output ('DIAGNOSTIC CONTROL '+$tree[$i].Current.ControlType.ProgrammaticName+': '+$tree[$i].Current.Name)};throw ($action.name+' did not open its actual native UI')};Write-Output ($action.name+' UI: '+(($opened|ForEach-Object {[KeyboardActionsTest]::Title($_)}) -join ' | '));Close-New $before
 }
 [KeyboardActionsTest]::Key($h,85,$true,$false,$false);Pump 900;if([KeyboardActionsTest]::Address($h) -notlike 'view-source:http://localhost:*'){Write-Output ('SOURCE address='+[KeyboardActionsTest]::Address($h));$sessionPath=Join-Path $root 'Session.json';if(Test-Path -LiteralPath $sessionPath){Get-Content -LiteralPath $sessionPath -Raw -Encoding UTF8|Write-Output};throw 'Ctrl+U did not open source'}
 [KeyboardActionsTest]::Key($h,36,$false,$false,$true);Pump 600;if([KeyboardActionsTest]::Address($h) -ne ''){throw 'Alt+Home did not open NTP'}
 [KeyboardActionsTest]::Key($h,46,$true,$true,$false);Pump 700;if([KeyboardActionsTest]::Address($h) -ne 'jisu://settings'){throw 'Ctrl+Shift+Delete did not open settings'}
 $before=@(Get-CimInstance Win32_Process -Filter "Name='jisu-browser.exe'"|ForEach-Object {$_.ProcessId});[KeyboardActionsTest]::Key($h,78,$true,$true,$false)
 $deadline=(Get-Date).AddSeconds(15);do{Pump 100;$candidate=@(Get-CimInstance Win32_Process -Filter "Name='jisu-browser.exe'"|Where-Object {$_.ProcessId -notin $before -and $_.CommandLine -and $_.CommandLine.Contains($root) -and $_.CommandLine.Contains('--inprivate')});if($candidate.Count){$private=Get-Process -Id $candidate[0].ProcessId;$private.Refresh()}}while(((-not $private) -or $private.MainWindowHandle -eq [IntPtr]::Zero -or [KeyboardActionsTest]::GetPropW($private.MainWindowHandle,'JisuBrowser.PrivateVerified').ToInt64() -ne 1) -and (Get-Date)-lt $deadline)
 if(-not $private -or [KeyboardActionsTest]::GetPropW($private.MainWindowHandle,'JisuBrowser.PrivateVerified').ToInt64() -ne 1){throw 'Ctrl+Shift+N did not create a verified private window'}
 'PASS actual shortcut route: Find/Print/SaveAs/DevTools UI; page DPR zoom/reset; source; Home; clear settings; verified InPrivate; no system SendInput or completed print/save'
 [KeyboardActionsTest]::End()
 foreach($process in @($private,$p)){$process.Refresh();$closing=if($process.Id -eq $p.Id){$h}else{$process.MainWindowHandle};[KeyboardActionsTest]::PostMessageW($closing,0x10,[IntPtr]::Zero,[IntPtr]::Zero)|Out-Null;if(-not $process.WaitForExit(15000)){throw ('fixture did not close pid='+$process.Id+' handle='+$process.MainWindowHandle+' windows='+(([KeyboardActionsTest]::Windows([uint32[]]@($process.Id))) -join ','))}}
}finally{[KeyboardActionsTest]::End();foreach($process in @($private,$p)){if($process -and -not $process.HasExited){Stop-Process -Id $process.Id -Force}};$server.Stop();Start-Sleep -Milliseconds 500;Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue}
