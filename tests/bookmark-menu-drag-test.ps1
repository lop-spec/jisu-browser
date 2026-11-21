param([Parameter(Mandatory)][string]$Exe)
$ErrorActionPreference='Stop'
# Uses real mouse input. Run only on a disposable interactive Windows runner.
Add-Type -AssemblyName UIAutomationClient,UIAutomationTypes
Add-Type -TypeDefinition @'
using System; using System.Text; using System.Runtime.InteropServices;
public static class BookmarkDragTest {
 public delegate bool EnumProc(IntPtr h,IntPtr arg);
 [StructLayout(LayoutKind.Sequential)] public struct RECT {public int L,T,R,B;}
 [DllImport("user32.dll")] static extern bool EnumThreadWindows(uint id,EnumProc cb,IntPtr arg);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] static extern int GetClassNameW(IntPtr h,StringBuilder s,int n);
 [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h,out uint p);
 [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
 [DllImport("user32.dll")] static extern IntPtr SendMessageW(IntPtr h,uint m,IntPtr w,IntPtr l);
 [DllImport("user32.dll")] static extern bool SetCursorPos(int x,int y);
 [DllImport("user32.dll")] public static extern void mouse_event(uint flags,uint x,uint y,uint data,IntPtr extra);
 [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h,uint m,IntPtr w,IntPtr l);
 [DllImport("user32.dll")] public static extern int GetMenuItemCount(IntPtr menu);
 [DllImport("user32.dll")] public static extern uint GetMenuItemID(IntPtr menu,int index);
 [DllImport("user32.dll")] static extern bool GetMenuItemRect(IntPtr h,IntPtr menu,uint index,out RECT rect);
 [DllImport("user32.dll")] static extern bool SetProcessDPIAware();
 public static void Dpi(){SetProcessDPIAware();}
 public static IntPtr Popup(IntPtr owner){uint pid;var result=IntPtr.Zero;EnumThreadWindows(GetWindowThreadProcessId(owner,out pid),(h,o)=>{var b=new StringBuilder(32);GetClassNameW(h,b,b.Capacity);if(b.ToString()=="#32768" && IsWindowVisible(h)){result=SendMessageW(h,0x1E1,IntPtr.Zero,IntPtr.Zero);return false;}return true;},IntPtr.Zero);return result;}
 public static void Move(IntPtr menu,uint row,bool bottom){RECT r;if(!GetMenuItemRect(IntPtr.Zero,menu,row,out r)||!SetCursorPos((r.L+r.R)/2,bottom?r.B-3:r.T+3))throw new Exception("Cannot position mouse in the real popup");}
 public static void Click(int x,int y){if(!SetCursorPos(x,y))throw new Exception("No interactive mouse desktop");mouse_event(2,0,0,0,IntPtr.Zero);mouse_event(4,0,0,0,IntPtr.Zero);}
}
'@
[BookmarkDragTest]::Dpi()
$root=Join-Path $env:TEMP ('jisu-bookmark-menu-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root|Out-Null
$file=Join-Path $root 'Bookmarks'
[IO.File]::WriteAllText($file,'{"roots":{"bookmark_bar":{"id":"1","children":[{"type":"folder","id":"10","name":"Fixture","children":[{"type":"url","id":"11","name":"First","url":"https://example.invalid/a"},{"type":"url","id":"12","name":"Second","url":"https://example.invalid/b"},{"type":"url","id":"13","name":"Third","url":"https://example.invalid/c"}]}]},"other":{"id":"2","children":[]},"synced":{"id":"3","children":[]}},"version":1}')
[IO.File]::WriteAllText((Join-Path $root 'jisu-settings.json'),'{"bookmarkBar":true,"importedDefaultBrowser":true}')
$p=$null
function WaitPopup($h){
 $until=(Get-Date).AddSeconds(5)
 do{$m=[BookmarkDragTest]::Popup($h);if($m -ne [IntPtr]::Zero){return $m};Start-Sleep -Milliseconds 100}while((Get-Date)-lt $until)
 throw 'Win32 popup did not open'
}
function AssertOrder([string]$expected){
 $until=(Get-Date).AddSeconds(8)
 do{$actual=((Get-Content -LiteralPath $file -Raw|ConvertFrom-Json).roots.bookmark_bar.children[0].children.id -join ',');if($actual -eq $expected){return};Start-Sleep -Milliseconds 100}while((Get-Date)-lt $until)
 throw "expected bookmark order $expected; got $actual"
}
function ClickChrome([string]$label){
 $until=(Get-Date).AddSeconds(5)
 $condition=New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::NameProperty,$label)
 do{
  $element=[System.Windows.Automation.AutomationElement]::FromHandle($h).FindFirst([System.Windows.Automation.TreeScope]::Descendants,$condition)
  if($element){$r=$element.Current.BoundingRectangle;if($r.Width -gt 0){[BookmarkDragTest]::Click([int]($r.X+$r.Width/2),[int]($r.Y+$r.Height/2));return}}
  Start-Sleep -Milliseconds 100
 }while((Get-Date)-lt $until)
 throw "chrome control not found: $label"
}
function DragRows([int]$from,[int]$to,[bool]$after){
 ClickChrome 'Fixture'
 $m=WaitPopup $h
 if([BookmarkDragTest]::GetMenuItemCount($m) -ne 5){throw 'expected 3 bookmarks, separator and open-all'}
 [BookmarkDragTest]::Move($m,$from,$false);Start-Sleep -Milliseconds 130
 [BookmarkDragTest]::mouse_event(2,0,0,0,[IntPtr]::Zero)
 Start-Sleep -Milliseconds 100
 [BookmarkDragTest]::Move($m,$to,$after);Start-Sleep -Milliseconds 250
 [BookmarkDragTest]::mouse_event(4,0,0,0,[IntPtr]::Zero)
 Start-Sleep -Milliseconds 350
 if([BookmarkDragTest]::Popup($h) -ne [IntPtr]::Zero){throw 'popup stayed open after a completed drag'}
}
try {
 $p=Start-Process -FilePath $Exe -ArgumentList @('--udf',('"'+$root+'"'),'--no-agent','--no-spare','--bounds','80,80,900,650') -PassThru
 $until=(Get-Date).AddSeconds(20)
 do{Start-Sleep -Milliseconds 100;$p.Refresh()}while($p.MainWindowHandle -eq [IntPtr]::Zero -and (Get-Date)-lt $until -and -not $p.HasExited)
 $h=$p.MainWindowHandle;if($h -eq [IntPtr]::Zero){throw 'browser window not found'}
 Start-Sleep -Milliseconds 1800
 ClickChrome (-join [char[]]@(33756,21333))
 $main=WaitPopup $h
 if([BookmarkDragTest]::GetMenuItemCount($main) -ne 5){throw 'main menu must have exactly five actions'}
 $ids=(0..4|ForEach-Object {[BookmarkDragTest]::GetMenuItemID($main,$_)}) -join ','
 if($ids -ne '1002,1011,1008,1012,1005'){throw "unexpected main menu order: $ids"}
 [BookmarkDragTest]::PostMessageW($h,0x1f,[IntPtr]::Zero,[IntPtr]::Zero)|Out-Null
 Start-Sleep -Milliseconds 300
 DragRows 0 2 $true;AssertOrder '12,13,11'
 DragRows 2 0 $false;AssertOrder '11,12,13'
 'PASS actual Win32 main menu order; bookmark mouse drag both directions; order durable on disk'
} finally {
 [BookmarkDragTest]::mouse_event(4,0,0,0,[IntPtr]::Zero)
 if($p -and -not $p.HasExited){Stop-Process -Id $p.Id -Force}
 Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
}