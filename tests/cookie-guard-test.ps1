param([Parameter(Mandatory)][string]$Exe)
$ErrorActionPreference='Stop'
# CTest is launched by pwsh on CI; PS 5.1 must not inherit PS 7-only modules.
$env:PSModulePath=(Join-Path $PSHOME 'Modules')+';'+(Join-Path $env:ProgramFiles 'WindowsPowerShell/Modules')
Import-Module Microsoft.PowerShell.Utility,Microsoft.PowerShell.Management
# Real WebView2 cookie jar, real guard: a page sets 5 fixed-name cookies and 30 conversation units
# (conv_key_<uuid> + history_off_<uuid>, a later unit expires later = newer) and reports document.cookie back.
#  1. dryRun: the guard plans 46 removals but deletes nothing.
#  2. dryRun off (config re-read, no restart): the 23 oldest units go, the 7 newest units and all 5 fixed cookies stay.
#  3. enabled=false: the guard reports "disabled".
$root=Join-Path $env:TEMP ('jisu-cookie-guard-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root|Out-Null
[IO.File]::WriteAllText((Join-Path $root 'jisu-settings.json'),'{"bookmarkBar":false,"importedDefaultBrowser":true}')
$configFile=Join-Path $root 'cookie-guard.json'
$statusFile=Join-Path $root 'cookie-guard-status.json'
function Set-Config([string]$json){[IO.File]::WriteAllText($configFile,$json)}
Set-Config '{"dryRun":true,"maxPerSite":20,"keepUnits":3,"intervalSeconds":10}'

function Uuid([int]$i){'{0:x8}-aaf8-83ec-bcb0-801bbaf826e0' -f (0x6aeb0000+$i)}
$port=Get-Random -Minimum 20000 -Maximum 40000
$listener=New-Object Net.HttpListener
$listener.Prefixes.Add("http://localhost:$port/")
$listener.Start()
$script:pending=$listener.GetContextAsync()
$script:reported=$null
$script:reports=0
$fixed=@('base_a','base_b','base_c','base_d','oai-did')
$setters=New-Object System.Collections.Generic.List[string]
foreach($name in $fixed){$setters.Add("document.cookie='$name=v; path=/; max-age=31536000';")}
for($i=0;$i -lt 30;$i++){
  $u=Uuid $i
  $setters.Add("document.cookie='conv_key_$u=k; path=/; max-age=$(2592000+$i*60)';")
  $setters.Add("document.cookie='history_off_$u=1; path=/; max-age=$(604800+$i*60)';")
}
$page='<!doctype html><title>cookie-guard</title><p>cookies</p><script>'+($setters -join '')+
  'setInterval(function(){fetch("/report",{method:"POST",body:document.cookie});},1000);</script>'

function Pump([int]$milliseconds){
  $until=(Get-Date).AddMilliseconds($milliseconds)
  while((Get-Date) -lt $until){
    if(-not $script:pending.Wait(50)){continue}
    $context=$script:pending.Result;$script:pending=$listener.GetContextAsync()
    $path=$context.Request.Url.AbsolutePath
    if($path -eq '/report'){
      $reader=New-Object IO.StreamReader($context.Request.InputStream,[Text.Encoding]::UTF8)
      $script:reported=$reader.ReadToEnd();$script:reports++
      $context.Response.StatusCode=204;$context.Response.Close();continue
    }
    $bytes=[Text.Encoding]::UTF8.GetBytes($(if($path -eq '/'){$page}else{'<!doctype html><title>other</title>'}))
    $context.Response.ContentType='text/html';$context.Response.OutputStream.Write($bytes,0,$bytes.Length);$context.Response.Close()
  }
}
function Reported-Names{
  if($null -eq $script:reported -or $script:reported -eq ''){return @()}
  @($script:reported -split '; ' | ForEach-Object {($_ -split '=')[0]})
}
function Read-Status{
  if(-not (Test-Path -LiteralPath $statusFile)){return $null}
  try{Get-Content -Raw -LiteralPath $statusFile | ConvertFrom-Json}catch{$null}
}
function Wait-Until([scriptblock]$condition,[string]$what,[int]$seconds){
  $deadline=(Get-Date).AddSeconds($seconds)
  while((Get-Date) -lt $deadline){
    Pump 250
    if($process.HasExited){throw ('Browser exited 0x{0:X8} while waiting for {1}' -f $process.ExitCode,$what)}
    if(& $condition){return}
  }
  $s=Read-Status
  throw ("Timeout waiting for {0}: reports={1} reportedCookies={2} status={3}" -f $what,$script:reports,(Reported-Names).Count,$(if($s){$s|ConvertTo-Json -Compress}else{'<none>'}))
}

$process=$null
try{
  $arguments=@('--udf',"`"$root`"",'--no-agent','--no-activate','--no-spare','--bounds','-3000,0,900,700',"http://localhost:$port/")
  $process=Start-Process -FilePath $Exe -ArgumentList $arguments -PassThru

  Wait-Until { (Reported-Names).Count -eq 65 } 'the page to hold 65 cookies' 60
  'PASS page holds 5 fixed + 30 units x 2 = 65 cookies'

  Wait-Until { $s=Read-Status; $s -and $s.dryRun -eq $true -and $s.planned -eq 46 } 'a dry-run status that plans 46 removals' 90
  $s=Read-Status
  if($s.evicted -ne 0 -or $s.evictedTotal -ne 0){throw "dryRun must not delete: $($s|ConvertTo-Json -Compress)"}
  if($s.note -ne 'dry-run'){throw "Unexpected note: $($s.note)"}
  $site=@($s.sites)|Where-Object {$_.site -eq 'localhost'}
  if(-not $site -or $site.count -lt 65 -or $site.evictUnits -ne 23){throw "Unexpected site report: $($s|ConvertTo-Json -Compress)"}
  Pump 2500
  if((Reported-Names).Count -ne 65){throw "dryRun deleted cookies: $((Reported-Names).Count) left"}
  'PASS dryRun plans 23 units (46 cookies) and deletes nothing'

  Set-Config '{"dryRun":false,"maxPerSite":20,"keepUnits":3,"intervalSeconds":10}'
  Wait-Until { (Reported-Names).Count -eq 19 } 'the guard to remove 46 cookies' 90
  $names=Reported-Names
  foreach($name in $fixed){if($names -notcontains $name){throw "Fixed cookie was removed: $name"}}
  for($i=0;$i -lt 30;$i++){
    $u=Uuid $i
    $expected=$i -ge 23
    foreach($prefix in 'conv_key_','history_off_'){
      $present=$names -contains ($prefix+$u)
      if($present -ne $expected){throw "Unit $i ($prefix) present=$present expected=$expected"}
    }
  }
  $s=Read-Status
  if($s.evictedTotal -ne 46){throw "evictedTotal should be 46: $($s|ConvertTo-Json -Compress)"}
  'PASS 23 oldest units removed, 7 newest units and all fixed cookies kept'

  Wait-Until { $s=Read-Status; $s -and $s.cookies -eq 19 -and $s.planned -eq 0 -and $s.evicted -eq 0 } 'a quiet round at 19 cookies' 60
  'PASS nothing more to remove: the site settles at 19 cookies'

  Set-Config '{"enabled":false}'
  Wait-Until { $s=Read-Status; $s -and $s.enabled -eq $false -and $s.note -eq 'disabled' } 'the disabled status' 60
  'PASS enabled=false is honoured without a restart'

  $process.CloseMainWindow()|Out-Null
  $process.WaitForExit(15000)|Out-Null
}finally{
  if($process -and -not $process.HasExited){Stop-Process -Id $process.Id -Force;$process.WaitForExit(10000)|Out-Null}
  $listener.Stop()
  Start-Sleep -Milliseconds 500
  Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
}
