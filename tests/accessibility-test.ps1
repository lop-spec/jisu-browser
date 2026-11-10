param([Parameter(Mandatory)][string]$Exe)
$ErrorActionPreference='Stop'
[Console]::OutputEncoding=New-Object Text.UTF8Encoding($false)
Add-Type -AssemblyName UIAutomationClient,UIAutomationTypes,Accessibility,UIAutomationClientsideProviders
$null=[System.Windows.Automation.AutomationElement]::RootElement
[System.Windows.Automation.ClientSettings]::RegisterClientSideProviders([UIAutomationClientsideProviders.UIAutomationClientSideProviders]::ClientSideProviderDescriptionTable)
$c=New-Object CodeDom.Compiler.CompilerParameters;$c.GenerateInMemory=$true;$c.TempFiles=New-Object CodeDom.Compiler.TempFileCollection([IO.Path]::GetTempPath(),$false);$c.ReferencedAssemblies.Add([Accessibility.IAccessible].Assembly.Location)|Out-Null
Add-Type -CompilerParameters $c -TypeDefinition @'
using System;using System.Runtime.InteropServices;
public static class AccessibilityTest {
 [DllImport("user32.dll")]public static extern bool PostMessageW(IntPtr h,uint m,IntPtr w,IntPtr l);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)]public static extern IntPtr GetPropW(IntPtr h,string name);
 [DllImport("user32.dll")]static extern uint GetWindowThreadProcessId(IntPtr h,out uint p);
 [StructLayout(LayoutKind.Sequential)]struct RECT{public int l,t,r,b;}
 [StructLayout(LayoutKind.Sequential)]struct GUI{public int size,flags;public IntPtr active,focus,capture,menu,move,caret;public RECT rect;}
 [DllImport("user32.dll")]static extern bool GetGUIThreadInfo(uint thread,ref GUI info);
 [DllImport("oleacc.dll")]static extern int AccessibleObjectFromWindow(IntPtr h,uint id,ref Guid iid,[MarshalAs(UnmanagedType.Interface)]out Accessibility.IAccessible value);
 [DllImport("oleacc.dll")]static extern int AccessibleChildren(Accessibility.IAccessible root,int start,int count,[Out,MarshalAs(UnmanagedType.LPArray,ArraySubType=UnmanagedType.Struct)]object[] children,out int got);
 public static string Tree(IntPtr h){try{var id=new Guid("618736E0-3C3D-11CF-810C-00AA00389B71");Accessibility.IAccessible root;int hr=AccessibleObjectFromWindow(h,unchecked((uint)-4),ref id,out root);if(hr<0)return "MSAA HRESULT="+hr;int n=Math.Min(root.accChildCount,256),got;var children=new object[n];AccessibleChildren(root,0,n,children,out got);var text="MSAA children="+got;for(int i=0;i<got;i++){var a=children[i] as Accessibility.IAccessible;var obj=a??root;object child=a!=null?(object)0:children[i];try{text+=" | "+obj.get_accRole(child)+":"+obj.get_accName(child);}catch(Exception e){text+=" | ERROR "+e.Message;}}return text;}catch(Exception e){return "MSAA ERROR "+e.Message;}}
 public delegate bool EnumProc(IntPtr h,IntPtr l);
 [DllImport("user32.dll")]static extern bool EnumChildWindows(IntPtr h,EnumProc f,IntPtr l);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)]static extern int GetClassNameW(IntPtr h,System.Text.StringBuilder b,int n);
 public static string NativeClients(IntPtr root){string text="NATIVE CLIENTS";EnumChildWindows(root,(h,l)=>{var b=new System.Text.StringBuilder(128);GetClassNameW(h,b,128);if(b.ToString().Equals("Edit",StringComparison.OrdinalIgnoreCase)){var id=new Guid("618736E0-3C3D-11CF-810C-00AA00389B71");Accessibility.IAccessible a;int hr=AccessibleObjectFromWindow(h,unchecked((uint)-4),ref id,out a);text+=" | EDIT hr="+hr+" role="+(hr>=0?a.get_accRole(0):null);}return true;},IntPtr.Zero);return text;}
 public static IntPtr Edit(IntPtr root){IntPtr edit=IntPtr.Zero;EnumChildWindows(root,(h,l)=>{var b=new System.Text.StringBuilder(128);GetClassNameW(h,b,128);if(b.ToString().Equals("Edit",StringComparison.OrdinalIgnoreCase))edit=h;return true;},IntPtr.Zero);return edit;}
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
 $direct=[System.Windows.Automation.AutomationElement]::FromHandle([AccessibilityTest]::Edit($h));Write-Output ('DIRECT EDIT type='+$direct.Current.ControlType.ProgrammaticName+' name='+$direct.Current.Name+' patterns='+(($direct.GetSupportedPatterns()|ForEach-Object {$_.ProgrammaticName}) -join ','))
 $tabs=$element.FindAll($scope,(New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::ControlTypeProperty,[System.Windows.Automation.ControlType]::TabItem)))
 if($tabs.Count -lt 1){
  [AccessibilityTest]::Tree($h)|Write-Output
  $actual=$element.FindAll($scope,[System.Windows.Automation.Condition]::TrueCondition);for($i=0;$i -lt [Math]::Min(80,$actual.Count);$i++){Write-Output ('UIA '+$actual[$i].Current.ControlType.ProgrammaticName+': '+$actual[$i].Current.Name)}
  throw 'native tab not exposed to Windows UIA'
 }
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
 if($edits.Count -lt 1 -or $docs.Count -lt 1){
  $direct=[System.Windows.Automation.AutomationElement]::FromHandle([AccessibilityTest]::Edit($h));Write-Output ('DIRECT EDIT type='+$direct.Current.ControlType.ProgrammaticName+' name='+$direct.Current.Name+' patterns='+(($direct.GetSupportedPatterns()|ForEach-Object {$_.ProgrammaticName}) -join ','));
  Write-Output ('COUNTS Edit='+$edits.Count+' Document='+$docs.Count);[AccessibilityTest]::Tree($h)|Write-Output;[AccessibilityTest]::NativeClients($h)|Write-Output
  $actual=$element.FindAll($scope,[System.Windows.Automation.Condition]::TrueCondition);for($i=0;$i -lt [Math]::Min(80,$actual.Count);$i++){Write-Output ('UIA '+$actual[$i].Current.ControlType.ProgrammaticName+': '+$actual[$i].Current.Name)}
  throw 'custom provider hid native EDIT or WebView document accessibility'
 }
 'PASS real Windows UIA: native tabs/button names; actual GUI focus and Tab navigation; invoke opens one tab; EDIT and webpage document preserved'
 [AccessibilityTest]::PostMessageW($h,0x10,[IntPtr]::Zero,[IntPtr]::Zero)|Out-Null;if(-not $p.WaitForExit(15000)){throw 'fixture did not close'}
}finally{if($p -and -not $p.HasExited){Stop-Process -Id $p.Id -Force};$server.Stop();Start-Sleep -Milliseconds 500;Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue}
