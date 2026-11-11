param([Parameter(Mandatory)][string]$Exe)
$ErrorActionPreference='Stop'
$c=New-Object CodeDom.Compiler.CompilerParameters;$c.GenerateInMemory=$true;$c.TempFiles=New-Object CodeDom.Compiler.TempFileCollection([IO.Path]::GetTempPath(),$false)
Add-Type -CompilerParameters $c -TypeDefinition @'
using System;using System.Text;using System.Runtime.InteropServices;using System.Threading;
public static class CaptionButtonsTest {
 [DllImport("user32.dll")]static extern IntPtr SetThreadDpiAwarenessContext(IntPtr context);
 static CaptionButtonsTest(){SetThreadDpiAwarenessContext((IntPtr)(-4));}
 [DllImport("user32.dll")]public static extern bool PostMessageW(IntPtr h,uint m,IntPtr w,IntPtr l);
 [DllImport("user32.dll")]static extern IntPtr SendMessageW(IntPtr h,uint m,IntPtr w,IntPtr l);
 [DllImport("user32.dll")]public static extern bool IsZoomed(IntPtr h);
 [DllImport("user32.dll")]public static extern bool IsIconic(IntPtr h);
 [DllImport("user32.dll")]static extern bool GetClientRect(IntPtr h,out RECT r);
 [DllImport("user32.dll")]static extern bool ClientToScreen(IntPtr h,ref POINT p);
 [DllImport("user32.dll")]static extern uint GetDpiForWindow(IntPtr h);
 [DllImport("user32.dll")]static extern uint GetWindowThreadProcessId(IntPtr h,out uint pid);
 [DllImport("user32.dll")]static extern bool GetGUIThreadInfo(uint thread,ref GUI info);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)]static extern int GetClassNameW(IntPtr h,StringBuilder text,int size);
 [DllImport("user32.dll",EntryPoint="GetWindowLongPtrW")]static extern IntPtr GetWindowLongPtrW(IntPtr h,int index);
 [StructLayout(LayoutKind.Sequential)]struct POINT{public int x,y;}
 [StructLayout(LayoutKind.Sequential)]struct RECT{public int l,t,r,b;}
 [StructLayout(LayoutKind.Sequential)]struct GUI{public int size,flags;public IntPtr active,focus,capture,menu,move,caret;public RECT rect;}
 static IntPtr Pack(POINT p){return (IntPtr)((p.y<<16)|(p.x&65535));}
 static POINT Button(IntPtr h,int offset){RECT r;GetClientRect(h,out r);double scale=GetDpiForWindow(h)/96.0;return new POINT{x=r.r-(int)Math.Round(offset*scale),y=(int)Math.Round(25*scale)};}
 public static string State(IntPtr h){uint pid;var g=new GUI{size=Marshal.SizeOf(typeof(GUI))};GetGUIThreadInfo(GetWindowThreadProcessId(h,out pid),ref g);var text=new StringBuilder(128);GetClassNameW(g.focus,text,128);return "dpi="+GetDpiForWindow(h)+" style="+GetWindowLongPtrW(h,-16).ToInt64().ToString("X")+" focus="+text+" capture="+g.capture;}
 public static int Hit(IntPtr h){var p=Button(h,69);ClientToScreen(h,ref p);return SendMessageW(h,0x84,IntPtr.Zero,Pack(p)).ToInt32();}
 public static void Click(IntPtr h,bool cancel){var client=Button(h,69);var p=client;ClientToScreen(h,ref p);PostMessageW(h,0xA1,(IntPtr)9,Pack(p));Thread.Sleep(80);if(cancel){PostMessageW(h,0x1F,IntPtr.Zero,IntPtr.Zero);Thread.Sleep(30);}PostMessageW(h,0x202,IntPtr.Zero,Pack(client));Thread.Sleep(40);PostMessageW(h,0xA2,(IntPtr)9,Pack(p));}
 public static void ClientClick(IntPtr h,int offset){var p=Button(h,offset);PostMessageW(h,0x201,(IntPtr)1,Pack(p));Thread.Sleep(50);PostMessageW(h,0x202,IntPtr.Zero,Pack(p));}
 public static void ReleaseOutside(IntPtr h){var p=Button(h,69);ClientToScreen(h,ref p);PostMessageW(h,0xA1,(IntPtr)9,Pack(p));Thread.Sleep(80);p.x-=200;PostMessageW(h,0x200,(IntPtr)1,ClientPoint(h,p));PostMessageW(h,0x202,IntPtr.Zero,ClientPoint(h,p));}
 [DllImport("user32.dll")]static extern bool ScreenToClient(IntPtr h,ref POINT p);
 static IntPtr ClientPoint(IntPtr h,POINT p){ScreenToClient(h,ref p);return Pack(p);}
 public static void Command(IntPtr h,int value){PostMessageW(h,0x112,(IntPtr)value,IntPtr.Zero);}
}
'@
$root=Join-Path $env:TEMP ('jisu-caption-buttons-'+[guid]::NewGuid().ToString('N'));New-Item -ItemType Directory -Path $root|Out-Null
[IO.File]::WriteAllText((Join-Path $root 'jisu-settings.json'),'{"bookmarkBar":true,"importedDefaultBrowser":true}')
$p=$null
try{
 $p=Start-Process -FilePath $Exe -ArgumentList @('--udf',('"'+$root+'"'),'--no-agent','--no-activate','--no-spare','--bounds','100,100,1040,696') -PassThru
 $deadline=(Get-Date).AddSeconds(30);do{Start-Sleep -Milliseconds 100;$p.Refresh()}while($p.MainWindowHandle -eq [IntPtr]::Zero -and (Get-Date)-lt $deadline -and -not $p.HasExited)
 $h=$p.MainWindowHandle;if($h -eq [IntPtr]::Zero){throw 'browser window missing'};Start-Sleep -Milliseconds 1500
 Write-Output ('INITIAL '+[CaptionButtonsTest]::State($h))
 if([CaptionButtonsTest]::Hit($h) -ne 9){throw 'maximize button no longer exposes HTMAXBUTTON for native snap layout'}
 # Independent controls run first: the old default non-client handler can enter a modal tracking loop.
 [CaptionButtonsTest]::Command($h,0xF030);Start-Sleep -Milliseconds 300
 if(-not [CaptionButtonsTest]::IsZoomed($h)){throw 'system maximize command failed'}
 Write-Output 'SYSTEM_COMMAND maximized=True'
 [CaptionButtonsTest]::Command($h,0xF120);Start-Sleep -Milliseconds 300
 [CaptionButtonsTest]::ClientClick($h,69);Start-Sleep -Milliseconds 300
 if(-not [CaptionButtonsTest]::IsZoomed($h)){throw 'client maximize control failed'}
 Write-Output 'CLIENT_CONTROL maximized=True'
 [CaptionButtonsTest]::Command($h,0xF120);Start-Sleep -Milliseconds 300
 [CaptionButtonsTest]::Click($h,$false);Start-Sleep -Milliseconds 400
 $clicked=[CaptionButtonsTest]::IsZoomed($h);Write-Output ('NC_CLICK maximized='+$clicked+' '+[CaptionButtonsTest]::State($h))
 if(-not $clicked){throw 'non-client maximize click was swallowed while direct/system maximize controls both worked'}
 [CaptionButtonsTest]::Click($h,$false);Start-Sleep -Milliseconds 300
 if([CaptionButtonsTest]::IsZoomed($h)){throw 'initial restore click failed'}
 [CaptionButtonsTest]::Click($h,$false);Start-Sleep -Milliseconds 300
 if(-not [CaptionButtonsTest]::IsZoomed($h)){throw 'second maximize click failed'}
 [CaptionButtonsTest]::Click($h,$false);Start-Sleep -Milliseconds 300
 if([CaptionButtonsTest]::IsZoomed($h)){throw 'restore click failed'}
 [CaptionButtonsTest]::ReleaseOutside($h);Start-Sleep -Milliseconds 200
 if([CaptionButtonsTest]::IsZoomed($h)){throw 'release outside the button unexpectedly maximized'}
 [CaptionButtonsTest]::Click($h,$true);Start-Sleep -Milliseconds 200
 if([CaptionButtonsTest]::IsZoomed($h)){throw 'cancelled click unexpectedly maximized'}
 [CaptionButtonsTest]::Click($h,$false);Start-Sleep -Milliseconds 300
 if(-not [CaptionButtonsTest]::IsZoomed($h)){throw 'click failed after cancellation'}
 [CaptionButtonsTest]::Command($h,0xF120);Start-Sleep -Milliseconds 200
 # The other custom caption buttons must keep their existing client-message path.
 [CaptionButtonsTest]::ClientClick($h,115);Start-Sleep -Milliseconds 200
 if(-not [CaptionButtonsTest]::IsIconic($h)){throw 'minimize caption button failed'}
 [CaptionButtonsTest]::Command($h,0xF120);Start-Sleep -Milliseconds 200
 [CaptionButtonsTest]::ClientClick($h,23)
 if(-not $p.WaitForExit(15000)){throw 'close caption button failed'}
 'PASS production caption buttons: native hit-test preserved; maximize/restore; client/system controls; release outside; cancellation; minimize/close paths'
}finally{if($p -and -not $p.HasExited){Stop-Process -Id $p.Id -Force};Start-Sleep -Milliseconds 300;Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue}
