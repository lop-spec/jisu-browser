param([Parameter(Mandatory)][string]$Exe)
$ErrorActionPreference='Stop'
# CTest is launched by pwsh on CI; PS 5.1 must not inherit PS 7-only modules.
$env:PSModulePath=(Join-Path $PSHOME 'Modules')+';'+(Join-Path $env:ProgramFiles 'WindowsPowerShell/Modules')
Import-Module Microsoft.PowerShell.Utility,Microsoft.PowerShell.Management
# 1. A page cannot open windows without a user gesture and cannot navigate itself to an internal shell page.
# 2. Tabs survive a killed process (unclean exit) and come back on the next start.
# 3. A normal close is clean: the next start opens a fresh window instead of restoring.
$root=Join-Path $env:TEMP ('jisu-session-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root|Out-Null
[IO.File]::WriteAllText((Join-Path $root 'jisu-settings.json'),'{"bookmarkBar":false,"importedDefaultBrowser":true}')
$port=Get-Random -Minimum 20000 -Maximum 40000
$listener=New-Object Net.HttpListener
$listener.Prefixes.Add("http://localhost:$port/")
$listener.Start()
$script:pending=$listener.GetContextAsync()
$script:requests=New-Object System.Collections.Generic.List[string]
$page='<!doctype html><title>session-a</title><p>session</p><script>'+
  'setTimeout(()=>window.open("/spam"),300);'+
  'setTimeout(()=>{location.href="https://newtab.jisu.example/bookmarks.html"},600);</script>'
function Pump([int]$milliseconds){
  $until=(Get-Date).AddMilliseconds($milliseconds)
  while((Get-Date) -lt $until){
    if(-not $script:pending.Wait(50)){continue}
    $context=$script:pending.Result;$script:pending=$listener.GetContextAsync()
    $path=$context.Request.Url.AbsolutePath;$script:requests.Add($path)
    $bytes=[Text.Encoding]::UTF8.GetBytes($(if($path -eq '/'){$page}else{'<!doctype html><title>other</title>'}))
    $context.Response.ContentType='text/html';$context.Response.OutputStream.Write($bytes,0,$bytes.Length);$context.Response.Close()
  }
}
function Start-Browser([string[]]$extra){
  $arguments=@('--udf',"`"$root`"",'--no-agent','--no-activate','--no-spare','--bounds','-3000,0,900,700')+$extra
  Start-Process -FilePath $Exe -ArgumentList $arguments -PassThru
}
$session=Join-Path $root 'Session.json'
function Show-State([string]$label){
  $text=if(Test-Path -LiteralPath $session){Get-Content -Raw -LiteralPath $session}else{'<missing>'}
  $hosts=@(Get-CimInstance Win32_Process -Filter "Name='msedgewebview2.exe'" -ErrorAction SilentlyContinue|Where-Object {$_.CommandLine -like "*$(Split-Path -Leaf $root)*"}).Count
  "[{0}] t={1:N1}s session={2} webview2Processes={3} requests={4}" -f $label,((Get-Date)-$script:started).TotalSeconds,$text,$hosts,($script:requests -join ',')
}
function Wait-Title([System.Diagnostics.Process]$process,[scriptblock]$accept,[int]$seconds){
  $deadline=(Get-Date).AddSeconds($seconds)
  $report=(Get-Date).AddSeconds(5)
  while((Get-Date) -lt $deadline){
    Pump 200
    if($process.HasExited){throw ('Browser exited 0x{0:X8}' -f $process.ExitCode)}
    $process.Refresh()
    if(& $accept $process.MainWindowTitle){return $process.MainWindowTitle}
    if((Get-Date) -gt $report){Write-Host (Show-State ('waiting title="'+$process.MainWindowTitle+'"'));$report=(Get-Date).AddSeconds(5)}
  }
  Write-Host (Show-State 'timeout')
  throw ('Unexpected window title: "{0}" (requests: {1})' -f $process.MainWindowTitle,($script:requests -join ','))
}
$script:started=Get-Date
$process=$null
try{
  $process=Start-Browser @("http://localhost:$port/")
  Wait-Title $process {param($t) $t -like 'session-a*'} 30 | Out-Null
  Pump 2500
  $process.Refresh()
  if($process.MainWindowTitle -notlike 'session-a*'){throw "The page navigated itself to an internal shell page: $($process.MainWindowTitle)"}
  if($script:requests -contains '/spam'){throw 'A popup without a user gesture was opened'}
  $deadline=(Get-Date).AddSeconds(15)
  while((Get-Date) -lt $deadline -and -not ((Test-Path -LiteralPath $session) -and ((Get-Content -Raw -LiteralPath $session) -like "*localhost:$port*"))){Pump 200}
  if(-not (Test-Path -LiteralPath $session)){throw 'Session.json was not written'}
  Write-Host (Show-State 'before kill')
  Stop-Process -Id $process.Id -Force
  $process.WaitForExit(10000)|Out-Null
  Write-Host (Show-State 'after kill')
  'PASS popup without a gesture blocked; internal page navigation from a website blocked'

  $process=Start-Browser @()
  Wait-Title $process {param($t) $t -like 'session-a*'} 30 | Out-Null
  'PASS tabs restored after an unclean exit'
  $process.CloseMainWindow()|Out-Null
  $deadline=(Get-Date).AddSeconds(15)
  while((Get-Date) -lt $deadline -and -not $process.HasExited){Pump 200}
  if(-not $process.HasExited){throw 'Browser did not exit after a normal close'}
  if((Get-Content -Raw -LiteralPath $session) -notlike '*"clean":true*'){throw "Normal close did not mark the session clean: $(Get-Content -Raw -LiteralPath $session)"}

  $served=@($script:requests|Where-Object {$_ -eq '/'}).Count
  $process=Start-Browser @()
  Wait-Title $process {param($t) $t -ne ''} 30 | Out-Null
  Pump 4000
  $process.Refresh()
  if($process.MainWindowTitle -like 'session-a*' -or @($script:requests|Where-Object {$_ -eq '/'}).Count -ne $served){
    throw "A clean session was restored again: $($process.MainWindowTitle)"
  }
  'PASS normal close starts fresh'
  $process.CloseMainWindow()|Out-Null
  $process.WaitForExit(15000)|Out-Null
}finally{
  if($process -and -not $process.HasExited){Stop-Process -Id $process.Id -Force;$process.WaitForExit(10000)|Out-Null}
  $listener.Stop()
  Start-Sleep -Milliseconds 500
  Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
}
