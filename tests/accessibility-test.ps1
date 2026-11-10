param([Parameter(Mandatory)][string]$Exe)
$ErrorActionPreference='Stop'
Add-Type -AssemblyName UIAutomationClient,UIAutomationTypes
$c=New-Object CodeDom.Compiler.CompilerParameters;$c.GenerateInMemory=$true;$c.TempFiles=New-Object CodeDom.Compiler.TempFileCollection([IO.Path]::GetTempPath(),$false)
Add-Type -CompilerParameters $c -TypeDefinition @'
using System;using System.Runtime.InteropServices;
public static class AccessibilityTest {
 [DllImport("user32.dll")]public static extern bool PostMessageW(IntPtr h,uint m,IntPtr w,IntPtr l);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)]public static extern IntPtr GetPropW(IntPtr h,string name);
 [DllImport("user32.dll")]static extern uint GetWindowThreadProcessId(IntPtr h,out uint p);
 [StructLayout(LayoutKind.Sequential)]struct RECT{public int l,t,r,b;}
 [StructLayout(LayoutKind.Sequential)]struct GUI{public int size,flags;public IntPtr active,focus,capture,menu,move,caret;public RECT rect;}
 [DllImport("user32.dll")]static extern bool GetGUIThreadInfo(uint thread,ref GUI info);
 public static bool OwnFocus(IntPtr h){uint p;var t=GetWindowThreadProcessId(h,out p);var g=new GUI{size=Marshal.SizeOf(typeof(GUI))};return GetGUIThreadInfo(t,ref g)&&g.focus==h;}
}
'@
$root=Join-Path $env:TEMP ('jisu-accessibility-'+[guid]::NewGuid().ToString('N'));New-Item -ItemType Directory -Path $root|Out-Null
[IO.File]::WriteAllText((Join-Path $root 'jisu-settings.json'),'{"bookmarkBar":false,"importedDefaultBrowser":true}')
$port=Get-Random -Minimum 23000 -Maximum 44000;$server=New-Object Net.HttpListener;$server.Prefixes.Add("http://localhost:$port/");$server.Start();$script:pending=$server.GetContextAsync()
function Pump([int]$ms){$until=(Get-Date).AddMilliseconds($ms);do{if($script:pending.Wait(25)){$x=$script:pending.Result;$script:pending=$server.GetContextAsync();$bytes=[Text.Encoding]::UTF8.GetBytes('<!doctype html><title>a11y-source</title><body><button>fixture-action</button></body>');$x.Response.ContentType='text/html';try{$x.Response.OutputStream.Write($bytes,0,$bytes.Length);$x.Response.Close()}catch{}}}while((Get-Date)-lt $until)}
$p=$null
try{
 $p=Start-Process -FilePath $Exe -ArgumentList @('--udf',('"'+$root+'"'),'--no-agent','--no-activate','--no-spare','--bounds','-3000,0,1024,768',"http://localhost:$port/") -PassThru
 $deadline=(Get-Date).AddSeconds(35);do{Pump 100;$p.Refresh()}while($p.MainWindowTitle -notlike 'a11y-source*' -and (Get-Date)-lt $deadline -and -not $p.HasExited)
 if($p.MainWindowTitle -notlike 'a11y-source*'){throw 'fixture did not load'}
 $h=$p.MainWindowHandle;$element=[System.Windows.Automation.AutomationElement]::FromHandle($h)
 $scope=[System.Windows.Automation.TreeScope]::Descendants
 $tabs=$element.FindAll($scope,(New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::ControlTypeProperty,[System.Windows.Automation.ControlType]::TabItem)))
 if($tabs.Count -lt 1){throw 'native tab not exposed to Windows UIA'}
 $newName=-join [char[]]@(26032,24314,26631,31614,39029)
 $button=$element.FindFirst($scope,(New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::NameProperty,$newName)))
 if(-not $button){throw 'native new-tab button name missing in Windows UIA'}
 $invoke=$button.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern)
 $button.SetFocus();Pump 100
 if(-not [AccessibilityTest]::OwnFocus($h)){throw 'accessible focus did not actually reach native GUI thread'}
 [AccessibilityTest]::PostMessageW($h,0x100,[IntPtr]9,[IntPtr]1)|Out-Null;Pump 100
 if(-not [AccessibilityTest]::OwnFocus($h)){throw 'Tab keyboard navigation left native toolbar unexpectedly'}
 $invoke.Invoke();Pump 500
 if([AccessibilityTest]::GetPropW($h,'JisuBrowser.StripTabs').ToInt64() -ne 2){throw 'Windows UIA action did not create exactly one tab'}
 $edits=$element.FindAll($scope,(New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::ControlTypeProperty,[System.Windows.Automation.ControlType]::Edit)))
 $docs=$element.FindAll($scope,(New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::ControlTypeProperty,[System.Windows.Automation.ControlType]::Document)))
 if($edits.Count -lt 1 -or $docs.Count -lt 1){throw 'custom provider hid native EDIT or WebView document accessibility'}
 'PASS real Windows UIA: native tabs/button names; actual GUI focus and Tab navigation; invoke opens one tab; EDIT and webpage document preserved'
 [AccessibilityTest]::PostMessageW($h,0x10,[IntPtr]::Zero,[IntPtr]::Zero)|Out-Null;if(-not $p.WaitForExit(15000)){throw 'fixture did not close'}
}finally{if($p -and -not $p.HasExited){Stop-Process -Id $p.Id -Force};$server.Stop();Start-Sleep -Milliseconds 500;Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue}
