param([Parameter(Mandatory)][string]$Exe)
$ErrorActionPreference='Stop'
# CTest is launched by pwsh on CI; PS 5.1 must not inherit PS 7-only modules.
$env:PSModulePath=(Join-Path $PSHOME 'Modules')+';'+(Join-Path $env:ProgramFiles 'WindowsPowerShell/Modules')
Import-Module Microsoft.PowerShell.Utility,Microsoft.PowerShell.Management
# Regression: the "Auto Close Old Tabs" native limit closed a window.open opener while the popup's controller
# was still being created; put_NewWindow on the dead opener's args then crashed EmbeddedBrowserWebView.dll (0xc0000005).
$root=Join-Path $env:TEMP ('jisu-popup-opener-'+[guid]::NewGuid().ToString('N'))
$ext=Join-Path $root 'Extensions\auto-close-old-tabs\1.0.0'
New-Item -ItemType Directory -Path $ext|Out-Null
[IO.File]::WriteAllText((Join-Path $ext 'manifest.json'),'{"manifest_version":3,"name":"Auto Close Old Tabs","version":"1.0.0"}')
$entry=@{sourceId='auto-close-old-tabs';name='Auto Close Old Tabs';path=$ext;popup='';options='';enabled=$true;pinned=$true}
[IO.File]::WriteAllText((Join-Path $root 'extensions.json'),(@{removeUnlisted=$false;extensions=@($entry)}|ConvertTo-Json -Depth 4))
[IO.File]::WriteAllText((Join-Path $root 'jisu-settings.json'),'{"bookmarkBar":false,"importedDefaultBrowser":true,"maxRegularTabs":1}')
$port=Get-Random -Minimum 20000 -Maximum 40000
$listener=New-Object Net.HttpListener
$listener.Prefixes.Add("http://localhost:$port/")
$listener.Start()
$process=$null
$requests=@()
try{
 $process=Start-Process -FilePath $Exe -ArgumentList @('--udf',"`"$root`"",'--no-agent','--no-activate','--no-spare','--bounds','-3000,0,900,700',"http://localhost:$port/") -PassThru
 $deadline=(Get-Date).AddSeconds(20)
 $pending=$listener.GetContextAsync()
 while((Get-Date) -lt $deadline -and -not $process.HasExited){
  if(-not $pending.Wait(100)){continue}
  $context=$pending.Result;$pending=$listener.GetContextAsync()
  $path=$context.Request.Url.AbsolutePath;$requests+=$path
  $html=if($path -eq '/'){'<!doctype html><title>opener</title><script>setTimeout(()=>window.open("/child"),500)</script>'}else{'<!doctype html><title>child</title>'}
  $bytes=[Text.Encoding]::UTF8.GetBytes($html)
  $context.Response.ContentType='text/html';$context.Response.OutputStream.Write($bytes,0,$bytes.Length);$context.Response.Close()
  if($path -eq '/child'){Start-Sleep -Seconds 4;break}
 }
 if($process.HasExited){throw ('Browser exited 0x{0:X8} after the opener was auto-closed (requests: {1})' -f $process.ExitCode,($requests -join ','))}
 if($requests -notcontains '/child'){throw ('Popup never loaded after its opener was auto-closed (requests: {0})' -f ($requests -join ','))}
 $status=Get-Content -Raw (Join-Path $root 'extensions-status.json')
 if($status -notmatch '"nativeTabLimit":true' -or $status -notmatch '"loaded":true'){throw "Tab limit adapter was not active: $status"}
 'PASS popup survives its opener being auto-closed by the tab limit while the popup controller is created'
}finally{
 if($process -and -not $process.HasExited){Stop-Process -Id $process.Id -Force;$process.WaitForExit(10000)|Out-Null}
 $listener.Stop()
 Start-Sleep -Milliseconds 500
 Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
}
