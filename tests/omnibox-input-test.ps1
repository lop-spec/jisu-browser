param([Parameter(Mandatory)][string]$Exe)
$ErrorActionPreference='Stop'
$c=New-Object CodeDom.Compiler.CompilerParameters;$c.GenerateInMemory=$true;$c.TempFiles=New-Object CodeDom.Compiler.TempFileCollection([IO.Path]::GetTempPath(),$false)
Add-Type -CompilerParameters $c -TypeDefinition @'
using System;using System.Text;using System.Runtime.InteropServices;using System.Threading;
public static class OmniboxInputTest {
 [DllImport("user32.dll")]static extern IntPtr SetThreadDpiAwarenessContext(IntPtr context);
 static OmniboxInputTest(){SetThreadDpiAwarenessContext((IntPtr)(-4));}
 public delegate bool EnumProc(IntPtr h,IntPtr l);
 [DllImport("user32.dll")]static extern bool EnumChildWindows(IntPtr h,EnumProc f,IntPtr l);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)]static extern int GetClassNameW(IntPtr h,StringBuilder b,int n);
 [DllImport("user32.dll")]public static extern bool PostMessageW(IntPtr h,uint m,IntPtr w,IntPtr l);
 [DllImport("user32.dll")]static extern IntPtr SendMessageW(IntPtr h,uint m,IntPtr w,IntPtr l);
 [DllImport("user32.dll",EntryPoint="SendMessageW",CharSet=CharSet.Unicode)]static extern IntPtr Text(IntPtr h,uint m,IntPtr w,StringBuilder value);
 [DllImport("user32.dll",EntryPoint="SendMessageW")]static extern IntPtr Sel(IntPtr h,uint m,out uint start,out uint end);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)]static extern int GetWindowTextLengthW(IntPtr h);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)]public static extern IntPtr GetPropW(IntPtr h,string name);
 [DllImport("user32.dll")]static extern uint GetDpiForWindow(IntPtr h);
 [DllImport("user32.dll")]static extern uint GetWindowThreadProcessId(IntPtr h,out uint pid);
 [StructLayout(LayoutKind.Sequential)]struct GUI{public int size,flags;public IntPtr active,focus,capture,menu,move,caret;public RECT rect;}
 [DllImport("user32.dll")]static extern bool GetGUIThreadInfo(uint thread,ref GUI info);
 static string State(IntPtr root){uint pid;var g=new GUI{size=Marshal.SizeOf(typeof(GUI))};GetGUIThreadInfo(GetWindowThreadProcessId(root,out pid),ref g);return " focus="+g.focus+" capture="+g.capture;}
 [StructLayout(LayoutKind.Sequential)]struct POINT{public int x,y;}
 [StructLayout(LayoutKind.Sequential)]struct RECT{public int l,t,r,b;}
 [DllImport("user32.dll")]static extern bool GetClientRect(IntPtr h,out RECT r);
 [DllImport("user32.dll")]static extern int MapWindowPoints(IntPtr from,IntPtr to,ref POINT p,uint count);
 public static IntPtr Edit(IntPtr root){IntPtr found=IntPtr.Zero;EnumChildWindows(root,(h,l)=>{var b=new StringBuilder(128);GetClassNameW(h,b,128);if(b.ToString()=="Edit"||b.ToString()=="EDIT")found=h;return true;},IntPtr.Zero);return found;}
 static IntPtr Pack(int x,int y){return (IntPtr)((y<<16)|(x&65535));}
 static IntPtr CharPoint(IntPtr h,int index){RECT r;GetClientRect(h,out r);long p=SendMessageW(h,0xD6,(IntPtr)index,IntPtr.Zero).ToInt64();return Pack((short)(p&65535)+1,(r.b-r.t)/2);}
 public static void Reset(IntPtr edit){SendMessageW(edit,0xB1,IntPtr.Zero,IntPtr.Zero);SendMessageW(edit,0xB7,IntPtr.Zero,IntPtr.Zero);}
 public static void MouseSelection(IntPtr edit,bool drag){var a=CharPoint(edit,3);var b=CharPoint(edit,14);SendMessageW(edit,0x201,(IntPtr)1,a);if(drag)SendMessageW(edit,0x200,(IntPtr)1,b);SendMessageW(edit,0x202,IntPtr.Zero,drag?b:a);}
 public static int[] Selection(IntPtr edit){uint a,b;Sel(edit,0xB0,out a,out b);return new[]{(int)a,(int)b,(int)SendMessageW(edit,0xE,IntPtr.Zero,IntPtr.Zero).ToInt64()};}
 public static void BookmarkDrag(IntPtr root,IntPtr edit,bool cancel){SendMessageW(edit,0xB1,IntPtr.Zero,(IntPtr)(-1));RECT box;GetClientRect(edit,out box);var a=Pack(Math.Max(2,(box.r-box.l)/3),(box.b-box.t)/2);var scale=GetDpiForWindow(root)/96.0;var p=new POINT{x=(int)(300*scale),y=(int)(105*scale)};MapWindowPoints(root,edit,ref p,1);var text=new StringBuilder(32768);Text(edit,0xD,(IntPtr)text.Capacity,text);Console.WriteLine("DRAG dpi="+GetDpiForWindow(root)+" editText="+text+" target="+p.x+","+p.y+" cancel="+cancel+State(root)+" sel="+String.Join(",",Selection(edit))+" char="+SendMessageW(edit,0xD7,IntPtr.Zero,a));SendMessageW(edit,0x201,(IntPtr)1,a);Console.WriteLine("DOWN edit="+edit+State(root));SendMessageW(edit,0x200,(IntPtr)1,Pack(p.x,p.y));Console.WriteLine("MOVE"+State(root));if(cancel){PostMessageW(edit,0x100,(IntPtr)27,(IntPtr)1);Thread.Sleep(150);}SendMessageW(edit,0x202,IntPtr.Zero,Pack(p.x,p.y));}
}
'@
$root=Join-Path $env:TEMP ('jisu-omnibox-input-'+[guid]::NewGuid().ToString('N'));New-Item -ItemType Directory -Path $root|Out-Null
[IO.File]::WriteAllText((Join-Path $root 'jisu-settings.json'),'{"bookmarkBar":true,"importedDefaultBrowser":true}')
$port=Get-Random -Minimum 23000 -Maximum 44000;$url="http://localhost:$port/omnibox-source-abcdefghijklmnopqrstuvwxyz"
$server=New-Object Net.HttpListener;$server.Prefixes.Add("http://localhost:$port/");$server.Start();$script:pending=$server.GetContextAsync()
function Pump([int]$ms){$until=(Get-Date).AddMilliseconds($ms);do{if($script:pending.Wait(20)){$x=$script:pending.Result;$script:pending=$server.GetContextAsync();$bytes=[Text.Encoding]::UTF8.GetBytes('<!doctype html><title>omnibox-source</title><body>fixture</body>');$x.Response.ContentType='text/html';try{$x.Response.OutputStream.Write($bytes,0,$bytes.Length);$x.Response.Close()}catch{}}}while((Get-Date)-lt $until)}
$p=$null
try{
 $p=Start-Process -FilePath $Exe -ArgumentList @('--udf',('"'+$root+'"'),'--no-agent','--no-activate','--no-spare','--bounds','-3000,0,1024,768',$url) -PassThru
 $deadline=(Get-Date).AddSeconds(35);do{Pump 100;$p.Refresh()}while($p.MainWindowTitle -notlike 'omnibox-source*' -and (Get-Date)-lt $deadline -and -not $p.HasExited)
 if($p.MainWindowTitle -notlike 'omnibox-source*'){throw 'fixture did not load'}
 $h=$p.MainWindowHandle;$edit=[OmniboxInputTest]::Edit($h);if($edit -eq [IntPtr]::Zero){throw 'native EDIT missing'}
 [OmniboxInputTest]::PostMessageW($h,0x8001,[IntPtr]5,[IntPtr]::Zero)|Out-Null;Pump 100
 [OmniboxInputTest]::PostMessageW($edit,0x100,[IntPtr]117,[IntPtr]1)|Out-Null;Pump 100
 [OmniboxInputTest]::Reset($edit);[OmniboxInputTest]::MouseSelection($edit,$true);Pump 200
 $sel=[OmniboxInputTest]::Selection($edit);if($sel[1]-$sel[0] -le 0 -or $sel[1]-$sel[0] -ge $sel[2]){throw ('first drag overwritten by select-all: '+($sel -join ','))}
 [OmniboxInputTest]::PostMessageW($edit,0x100,[IntPtr]117,[IntPtr]1)|Out-Null;Pump 100
 [OmniboxInputTest]::Reset($edit);[OmniboxInputTest]::MouseSelection($edit,$false);Pump 200
 $sel=[OmniboxInputTest]::Selection($edit);if($sel[0] -ne 0 -or $sel[1] -ne $sel[2]){throw ('first click did not select all: '+($sel -join ','))}
 if([OmniboxInputTest]::GetPropW($h,'JisuBrowser.BookmarkDrop').ToInt64() -ne 1){throw 'native OLE drop target was not registered'}
 [OmniboxInputTest]::BookmarkDrag($h,$edit,$false)
 $path=Join-Path $root 'Bookmarks';$deadline=(Get-Date).AddSeconds(8);do{Pump 100}while(-not (Test-Path -LiteralPath $path) -and (Get-Date)-lt $deadline)
 if(-not (Test-Path -LiteralPath $path)){
  Add-Type -AssemblyName UIAutomationClient,UIAutomationTypes,UIAutomationClientsideProviders;$null=[System.Windows.Automation.AutomationElement]::RootElement;[System.Windows.Automation.ClientSettings]::RegisterClientSideProviders([UIAutomationClientsideProviders.UIAutomationClientSideProviders]::ClientSideProviderDescriptionTable)
  $view=[System.Windows.Automation.AutomationElement]::FromHandle($h);$items=$view.FindAll([System.Windows.Automation.TreeScope]::Descendants,[System.Windows.Automation.Condition]::TrueCondition);for($i=0;$i -lt [Math]::Min(35,$items.Count);$i++){Write-Output ('LAYOUT '+$items[$i].Current.ControlType.ProgrammaticName+': '+$items[$i].Current.Name+' rect='+$items[$i].Current.BoundingRectangle)}
  Get-Content -LiteralPath (Join-Path $root 'jisu-settings.json') -Raw -Encoding UTF8|Write-Output
 }
 $bookmarks=Get-Content -LiteralPath $path -Raw -Encoding UTF8|ConvertFrom-Json
 $rows=@($bookmarks.roots.bookmark_bar.children);if($rows.Count -ne 1 -or $rows[0].url -ne $url){throw 'selected native URL did not copy into bookmark bar'}
 [OmniboxInputTest]::BookmarkDrag($h,$edit,$true);Pump 700
 $bookmarks=Get-Content -LiteralPath $path -Raw -Encoding UTF8|ConvertFrom-Json;if(@($bookmarks.roots.bookmark_bar.children).Count -ne 1){throw 'Esc failed to cancel URL drag'}
 'PASS production EDIT: first drag retains partial selection; first click selects all; OLE target registered; selected URL copied to bar; Esc cancels without extra bookmark'
 [OmniboxInputTest]::PostMessageW($h,0x10,[IntPtr]::Zero,[IntPtr]::Zero)|Out-Null;if(-not $p.WaitForExit(15000)){throw 'fixture did not close'}
}finally{if($p -and -not $p.HasExited){Stop-Process -Id $p.Id -Force};$server.Stop();Start-Sleep -Milliseconds 500;Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue}
