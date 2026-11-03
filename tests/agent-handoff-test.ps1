param([Parameter(Mandatory)][string]$Exe)
$ErrorActionPreference='Stop'
# CTest is launched by pwsh on CI; PS 5.1 must not inherit PS 7-only modules.
$env:PSModulePath=(Join-Path $PSHOME 'Modules')+';'+(Join-Path $env:ProgramFiles 'WindowsPowerShell/Modules')
Import-Module Microsoft.PowerShell.Utility,Microsoft.PowerShell.Management
# Normal CDP work must not open or show anything; only an explicit page.bringToFront() (the agent needs the user)
# surfaces the page and the window. A fake broker + MCP relay drive the real agent bridge of a real browser process.
#   phase 1 (--background): no visible window at all while the agent navigates; bringToFront reveals + adopts it
#   phase 2 (visible window, default): agent tabs stay out of the tab strip, the pool is reused, a hand-off page
#           the user is not looking at goes back to the pool
#   phase 3 (showTabs=true): the old behaviour, agent tabs listed in the strip
Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
public static class JisuTestWin {
  delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc p, IntPtr l);
  [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] static extern bool IsIconic(IntPtr h);
  [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] static extern IntPtr MonitorFromRect(ref RECT r, uint flags);
  [DllImport("user32.dll")] static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern IntPtr GetPropW(IntPtr h, string name);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern IntPtr SendMessageW(IntPtr h, uint msg, IntPtr w, ref COPYDATASTRUCT l);
  [StructLayout(LayoutKind.Sequential)] struct RECT { public int L, T, R, B; }
  [StructLayout(LayoutKind.Sequential)] struct COPYDATASTRUCT { public IntPtr dwData; public int cbData; public IntPtr lpData; }
  public static IntPtr MainWindow(uint pid) {
    IntPtr found = IntPtr.Zero;
    EnumWindows((h, l) => {
      uint owner; GetWindowThreadProcessId(h, out owner);
      var cls = new StringBuilder(64); GetClassNameW(h, cls, 64);
      if (owner == pid && cls.ToString() == "JisuBrowserWindow") { found = h; return false; }
      return true;
    }, IntPtr.Zero);
    return found;
  }
  // visible top-level windows of the process that sit on a monitor (the off-screen agent host does not count)
  public static string OnScreenWindows(uint pid) {
    var rows = new List<string>();
    EnumWindows((h, l) => {
      uint owner; GetWindowThreadProcessId(h, out owner);
      if (owner != pid || !IsWindowVisible(h)) return true;
      RECT r; GetWindowRect(h, out r);
      if (MonitorFromRect(ref r, 0) == IntPtr.Zero) return true;
      var cls = new StringBuilder(64); GetClassNameW(h, cls, 64);
      rows.Add(cls.ToString());
      return true;
    }, IntPtr.Zero);
    return string.Join(",", rows);
  }
  public static bool Visible(IntPtr h) { return IsWindowVisible(h); }
  public static bool Minimized(IntPtr h) { return IsIconic(h); }
  public static bool Foreground(IntPtr h) { return GetForegroundWindow() == h; }
  public static long Prop(IntPtr h, string name) { return GetPropW(h, name).ToInt64(); }
  // what a second launch of the browser (double click, link) sends to the running one
  public static void OpenUrl(IntPtr h, string url) {
    IntPtr memory = Marshal.StringToHGlobalUni(url);
    try {
      var data = new COPYDATASTRUCT { dwData = (IntPtr)0x4A495355, cbData = (url.Length + 1) * 2, lpData = memory };
      SendMessageW(h, 0x4A, IntPtr.Zero, ref data);
    } finally { Marshal.FreeHGlobal(memory); }
  }
}
'@

$token='A'*43
$root=Join-Path $env:TEMP ('jisu-handoff-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root|Out-Null
$brokerPort=Get-Random -Minimum 20000 -Maximum 30000
$relayPort=$brokerPort+1
$pagePort=$brokerPort+2
[IO.File]::WriteAllText((Join-Path $root 'auth.json'),('{"token":"'+$token+'"}'))
$profileCount=0

$broker=New-Object Net.HttpListener;$broker.Prefixes.Add("http://127.0.0.1:$brokerPort/");$broker.Start()
$relay=New-Object Net.HttpListener;$relay.Prefixes.Add("http://127.0.0.1:$relayPort/");$relay.Start()
$page=New-Object Net.HttpListener;$page.Prefixes.Add("http://localhost:$pagePort/");$page.Start()
$script:brokerTask=$broker.GetContextAsync();$script:pageTask=$page.GetContextAsync();$script:relayTask=$relay.GetContextAsync()
$script:ticket=$null;$script:acks=New-Object System.Collections.Generic.List[object]
$script:pages=New-Object System.Collections.Generic.List[string]
$script:socket=$null;$script:inbox=New-Object System.Collections.Generic.List[string]
$script:receive=$null;$script:buffer=New-Object byte[] 262144
$script:process=$null;$script:hwnd=[IntPtr]::Zero

function Reply-Text($context,[string]$text,[string]$type='application/json'){
 $bytes=[Text.Encoding]::UTF8.GetBytes($text)
 try{  # the client may be gone already (a killed browser leaves requests behind)
  $context.Response.ContentType=$type
  $context.Response.OutputStream.Write($bytes,0,$bytes.Length);$context.Response.Close()
 }catch{try{$context.Response.Abort()}catch{}}
}
function Next-Receive{
 $script:receive=$script:socket.ReceiveAsync((New-Object ArraySegment[byte] -ArgumentList (,$script:buffer)),[Threading.CancellationToken]::None)
}
function Pump([int]$milliseconds){
 $until=(Get-Date).AddMilliseconds($milliseconds)
 do{
  if($script:brokerTask.Wait(10)){
   $context=$script:brokerTask.Result;$script:brokerTask=$broker.GetContextAsync()
   $path=$context.Request.Url.AbsolutePath
   if($path -eq '/poll'){
    $tickets=if($script:ticket){'[{"id":"'+$script:ticket.id+'","url":"'+$script:ticket.url+'"}]'}else{'[]'}
    Reply-Text $context ('{"tickets":'+$tickets+'}')
   }elseif($path -eq '/ack'){
    $body=(New-Object IO.StreamReader($context.Request.InputStream)).ReadToEnd()
    $script:acks.Add(($body|ConvertFrom-Json));$script:ticket=$null
    Reply-Text $context '{"ok":true}'
   }else{Reply-Text $context '{}'}
  }
  if($script:pageTask.Wait(10)){
   $context=$script:pageTask.Result;$script:pageTask=$page.GetContextAsync()
   $script:pages.Add($context.Request.Url.AbsolutePath)
   Reply-Text $context '<!doctype html><title>agent-page</title><p>agent</p>' 'text/html'
  }
  if($script:relayTask.Wait(10)){
   $context=$script:relayTask.Result;$script:relayTask=$relay.GetContextAsync()
   if($context.Request.IsWebSocketRequest){
    $script:socket=$context.AcceptWebSocketAsync([NullString]::Value).Result.WebSocket
    Next-Receive
   }else{$context.Response.StatusCode=400;$context.Response.Close()}
  }
  if($script:receive){
   try{
    if($script:receive.Wait(10)){
     $result=$script:receive.Result
     if($result.MessageType -eq 'Close'){$script:receive=$null}
     else{$script:inbox.Add([Text.Encoding]::UTF8.GetString($script:buffer,0,$result.Count));Next-Receive}
    }
   }catch{$script:receive=$null}  # the browser went away
  }
 }while((Get-Date) -lt $until)
}
function Wait-For([scriptblock]$condition,[string]$what,[int]$seconds=25){
 $deadline=(Get-Date).AddSeconds($seconds)
 while((Get-Date) -lt $deadline){
  if(& $condition){return}
  Pump 100
  if($script:process -and $script:process.HasExited){throw ('Browser exited 0x{0:X8} while waiting for: {1}' -f $script:process.ExitCode,$what)}
 }
 $detail=if($script:hwnd -ne [IntPtr]::Zero -and $script:process){$s=State;"strip=$($s.Strip) stripAgents=$($s.StripAgents) hidden=$($s.Hidden) visible=$($s.Visible) pages=[$($script:pages -join ',')]"}else{'no browser'}
 throw "Timed out waiting for: $what ($detail)"
}
function Send-Relay([string]$json){
 $bytes=[Text.Encoding]::UTF8.GetBytes($json)
 $script:socket.SendAsync((New-Object ArraySegment[byte] -ArgumentList (,$bytes)),'Text',$true,[Threading.CancellationToken]::None).Wait()
}
# Anchored: a tab id inside chrome.tabs.onCreated must not be mistaken for the reply to request 1
function Replies([int]$id){@($script:inbox|Where-Object {$_ -match ('^\{"id":'+$id+'[,}]')})}
function Request([int]$id,[string]$json,[string]$what){
 Send-Relay $json
 Wait-For {@(Replies $id).Count -gt 0} "reply to $what"
 $raw=@(Replies $id)[0]  # @(): a single match would otherwise be unrolled into a string and [0] would be its first character
 try{$reply=$raw|ConvertFrom-Json}catch{throw "Unreadable reply to ${what}: $raw"}
 if($reply.error){throw "$what failed: $($reply.error|ConvertTo-Json -Compress)"}
 $reply
}
function Command([int]$id,[int]$tab,[string]$method,[string]$params=''){
 $extra=if($params){','+$params}else{''}
 Request $id ('{"id":'+$id+',"method":"chrome.debugger.sendCommand","params":[{"tabId":'+$tab+'},"'+$method+'"'+$extra+']}') $method
}
function Start-Browser([string]$config,[string[]]$extra){
 $script:profileCount++
 $profileDir=Join-Path $root ('profile'+$script:profileCount)
 New-Item -ItemType Directory -Path $profileDir|Out-Null
 [IO.File]::WriteAllText((Join-Path $profileDir 'jisu-settings.json'),'{"bookmarkBar":false,"importedDefaultBrowser":true}')
 $configFile=Join-Path $root 'agent-bridge.json'
 [IO.File]::WriteAllText($configFile,$config)
 $arguments=@('--udf',"`"$profileDir`"",'--agent-config',"`"$configFile`"",'--no-spare','--bounds','-3000,0,900,700')+$extra
 $script:process=Start-Process -FilePath $Exe -ArgumentList $arguments -PassThru
 Wait-For {[JisuTestWin]::MainWindow([uint32]$script:process.Id) -ne [IntPtr]::Zero} 'browser window'
 $script:hwnd=[JisuTestWin]::MainWindow([uint32]$script:process.Id)
 Wait-For {[JisuTestWin]::Prop($script:hwnd,'JisuBrowser.StripTabs') -ge 1} 'tab state published'
}
function Stop-Browser{
 if($script:process -and -not $script:process.HasExited){Stop-Process -Id $script:process.Id -Force;$script:process.WaitForExit(10000)|Out-Null}
 $script:process=$null;$script:socket=$null;$script:receive=$null
}
function State{
 [pscustomobject]@{
  Strip=[JisuTestWin]::Prop($script:hwnd,'JisuBrowser.StripTabs')
  StripAgents=[JisuTestWin]::Prop($script:hwnd,'JisuBrowser.StripAgentTabs')
  Hidden=[JisuTestWin]::Prop($script:hwnd,'JisuBrowser.HiddenAgentTabs')
  Visible=[JisuTestWin]::Visible($script:hwnd)
  OnScreen=[JisuTestWin]::OnScreenWindows([uint32]$script:process.Id)
 }
}
# Invitation -> lease -> handshake, then attach + navigate. Returns the leased tab id.
function Open-Session([string]$ticketId){
 $relayUrl=[Uri]::EscapeDataString("ws://127.0.0.1:$relayPort/relay")
 $script:inbox.Clear();$script:acks.Clear();$script:socket=$null
 $script:ticket=@{id=$ticketId;url="chrome-extension://test/connect.html?protocolVersion=2&token=$token&mcpRelayUrl=$relayUrl"}
 Wait-For {$script:acks.Count -gt 0} "invitation ack ($ticketId)"
 if(-not $script:acks[0].success){throw "Invitation refused: $($script:acks[0].error)"}
 Wait-For {@($script:inbox|Where-Object {$_ -match 'extension.initialized'}).Count -gt 0} "handshake ($ticketId)"
 $created=@($script:inbox|Where-Object {$_ -match 'chrome.tabs.onCreated'})[0]|ConvertFrom-Json
 $tab=[int]$created.params[0].id
 Request 1 ('{"id":1,"method":"chrome.debugger.attach","params":[{"tabId":'+$tab+'},"1.3"]}') 'attach'|Out-Null
 Command 2 $tab 'Page.navigate' ('{"url":"http://localhost:'+$pagePort+'/agent"}')|Out-Null
 $tab
}
function Close-Session{
 if($script:socket){try{$script:socket.CloseAsync('NormalClosure','done',[Threading.CancellationToken]::None).Wait(3000)|Out-Null}catch{}}
 $script:socket=$null;$script:receive=$null
 Pump 1200  # the bridge notices the close and returns the lease
}
$config='{"broker":"http://127.0.0.1:'+$brokerPort+'","tokenFile":"'+(Join-Path $root 'auth.json').Replace('\','\\')+'","pollMs":200,"graceMs":300,"freezeMs":0,"pageFreezeMs":0,"yieldTo":[]}'
try{
 # ---------------------------------------------------------------- phase 1: --background
 Start-Browser $config @('--background')
 $s=State
 if($s.Visible -or $s.OnScreen){throw "A --background browser showed a window: visible=$($s.Visible) onScreen=$($s.OnScreen)"}
 if($s.Strip -ne 1 -or $s.Hidden -ne 0){throw "Unexpected initial state: strip=$($s.Strip) hidden=$($s.Hidden)"}
 $tab=Open-Session 'bg1'
 Wait-For {$script:pages -contains '/agent'} 'the agent page request'
 $s=State
 if($s.Visible -or $s.OnScreen){throw "CDP work showed a window: visible=$($s.Visible) onScreen=$($s.OnScreen)"}
 if($s.Strip -ne 1 -or $s.StripAgents -ne 0 -or $s.Hidden -ne 1){throw "Agent tab leaked into the strip: strip=$($s.Strip) stripAgents=$($s.StripAgents) hidden=$($s.Hidden)"}
 'PASS --background: the agent leased a tab and navigated it with no visible window and nothing in the tab strip'

 Command 3 $tab 'Page.bringToFront'|Out-Null
 Wait-For {[JisuTestWin]::Visible($script:hwnd)} 'window revealed by the hand-off'
 Wait-For {[JisuTestWin]::Prop($script:hwnd,'JisuBrowser.StripAgentTabs') -eq 1} 'agent tab in the strip'
 $s=State
 if($s.Strip -ne 2 -or $s.Hidden -ne 0 -or [JisuTestWin]::Minimized($script:hwnd)){throw "Hand-off state wrong: strip=$($s.Strip) hidden=$($s.Hidden)"}
 'PASS page.bringToFront reveals the window and moves exactly that page into the tab strip (foreground=' + [JisuTestWin]::Foreground($script:hwnd) + ')'

 # The session ends while the user is looking at the page: it becomes the user's own tab.
 Close-Session
 Wait-For {[JisuTestWin]::Prop($script:hwnd,'JisuBrowser.StripAgentTabs') -eq 0} 'page adopted by the user'
 $s=State
 if($s.Strip -ne 2 -or $s.Hidden -ne 0){throw "Adoption wrong: strip=$($s.Strip) hidden=$($s.Hidden)"}
 'PASS a page the user is looking at stays as a normal tab when the agent session ends'
 Stop-Browser

 # ---------------------------------------------------------------- phase 2: visible window, default
 Start-Browser $config @('--no-activate')
 Wait-For {[JisuTestWin]::Visible($script:hwnd)} 'the window of a normal start'
 $script:pages.Clear()
 $tab=Open-Session 'v1'
 Wait-For {$script:pages -contains '/agent'} 'the agent page request (visible window)'
 $s=State
 if($s.Strip -ne 1 -or $s.StripAgents -ne 0 -or $s.Hidden -ne 1){throw "Visible window: agent tab shown in the strip: strip=$($s.Strip) stripAgents=$($s.StripAgents) hidden=$($s.Hidden)"}
 'PASS visible window: agent work leaves the user''s tab strip untouched'
 Close-Session
 $second=Open-Session 'v2'
 $s=State
 if($second -ne $tab -or $s.Hidden -ne 1 -or $s.Strip -ne 1){throw "Pool was not reused: first=$tab second=$second hidden=$($s.Hidden) strip=$($s.Strip)"}
 'PASS the idle agent tab is reused by the next session'
 Command 9 $tab 'Page.bringToFront'|Out-Null
 Wait-For {[JisuTestWin]::Prop($script:hwnd,'JisuBrowser.StripAgentTabs') -eq 1} 'agent tab in the strip (visible window)'
 $s=State
 if($s.Strip -ne 2 -or $s.Hidden -ne 0){throw "Hand-off in a visible window: strip=$($s.Strip) hidden=$($s.Hidden)"}
 # The user moves on to another page (what a link click or a second launch does) before the session ends:
 # the hand-off page goes back to the pool instead of cluttering the strip.
 [JisuTestWin]::OpenUrl($script:hwnd,"http://localhost:$pagePort/user-page")
 Wait-For {[JisuTestWin]::Prop($script:hwnd,'JisuBrowser.StripTabs') -eq 3} 'the user''s own tab'
 Close-Session
 Wait-For {[JisuTestWin]::Prop($script:hwnd,'JisuBrowser.HiddenAgentTabs') -eq 1} 'hand-off page back in the pool'
 $s=State
 if($s.Strip -ne 2 -or $s.StripAgents -ne 0){throw "Stash wrong: strip=$($s.Strip) stripAgents=$($s.StripAgents) hidden=$($s.Hidden)"}
 'PASS a hand-off page the user is not looking at goes back to the hidden pool'
 Stop-Browser

 # ---------------------------------------------------------------- phase 3: showTabs=true keeps the old behaviour
 $legacy=$config.Replace('"yieldTo":[]','"yieldTo":[],"showTabs":true')
 Start-Browser $legacy @('--no-activate')
 $script:pages.Clear()
 $tab=Open-Session 'l1'
 $s=State
 if($s.Strip -ne 2 -or $s.StripAgents -ne 1 -or $s.Hidden -ne 0){throw "showTabs=true: expected the agent tab in the strip: strip=$($s.Strip) stripAgents=$($s.StripAgents) hidden=$($s.Hidden)"}
 'PASS showTabs=true still lists agent tabs in the strip'
 Stop-Browser

 # ---------------------------------------------------------------- phase 4: the user opens a background-started browser
 # (a second launch, a link or the shortcut forwards its URL with this message)
 Start-Browser $config @('--background')
 $script:pages.Clear()
 [JisuTestWin]::OpenUrl($script:hwnd,"http://localhost:$pagePort/user-link")
 Wait-For {[JisuTestWin]::Visible($script:hwnd)} 'the window opened by the user''s link'
 Wait-For {$script:pages -contains '/user-link'} 'the user''s link request'
 Wait-For {[JisuTestWin]::Prop($script:hwnd,'JisuBrowser.StripTabs') -eq 1} 'only the user''s page in the strip'
 'PASS opening a link in a background-started browser shows the window with just that page'
 Stop-Browser
 Start-Browser $config @('--background')
 [JisuTestWin]::OpenUrl($script:hwnd,'')
 Wait-For {[JisuTestWin]::Visible($script:hwnd)} 'the window opened by the user'
 Pump 800
 $s=State
 if($s.Strip -ne 1){throw "Opening the browser added an extra blank tab: strip=$($s.Strip)"}
 'PASS opening a background-started browser shows its single new-tab page'
 Stop-Browser
}finally{
 Stop-Browser
 foreach($listener in $broker,$relay,$page){try{$listener.Stop()}catch{}}
 Start-Sleep -Milliseconds 500
 Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
}
