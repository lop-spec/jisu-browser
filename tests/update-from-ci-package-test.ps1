$ErrorActionPreference='Stop'
# CTest is launched by pwsh on CI; PS 5.1 must not inherit PS 7-only modules.
$env:PSModulePath=(Join-Path $PSHOME 'Modules')+';'+(Join-Path $env:ProgramFiles 'WindowsPowerShell/Modules')
Import-Module Microsoft.PowerShell.Utility,Microsoft.PowerShell.Management,Microsoft.PowerShell.Security
# tools\update-from-ci-package.ps1 against two synthetic CI packages in a temporary install directory:
# only changed files are replaced, each one is backed up byte-for-byte, a second run changes nothing,
# and a tampered package is rejected before the installation is touched.
$script=Join-Path $PSScriptRoot '../tools/update-from-ci-package.ps1'
$root=Join-Path $env:TEMP ('jisu-update-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root|Out-Null
function New-Package([string]$name,[string]$exe,[string]$page){
 $dir=Join-Path $root $name
 New-Item -ItemType Directory -Path "$dir/jisu-browser/ntp","$dir/jisu-browser/media" -Force|Out-Null
 [IO.File]::WriteAllText("$dir/jisu-browser/jisu-browser.exe",$exe)
 [IO.File]::WriteAllText("$dir/jisu-browser/ntp/index.html",$page)
 [IO.File]::WriteAllText("$dir/jisu-browser/ntp/stable.html",'same in both packages')
 [IO.File]::WriteAllText("$dir/jisu-browser/media/engine.bin",'same engine')
 [IO.File]::WriteAllText("$dir/ci-smoke.json",'{"version":"9.9.9","commit":"0123456789abcdef0123456789abcdef01234567"}')
 # Explicit relative names: the CI runner's TEMP is an 8.3 short path, so prefix-length arithmetic on FullName is wrong there.
 $lines='jisu-browser/jisu-browser.exe','jisu-browser/media/engine.bin','jisu-browser/ntp/index.html','jisu-browser/ntp/stable.html'|ForEach-Object{
  $file=Join-Path $dir $_
  '{0}  {1,9}  {2}' -f (Get-FileHash -Algorithm SHA256 $file).Hash.ToLowerInvariant(),(Get-Item $file).Length,$_}
 $lines|Set-Content -Path "$dir/SHA256SUMS.txt" -Encoding utf8
 $dir
}
function Invoke-Update([string]$package){
 # The child reports problems on stderr: do not let Windows PowerShell 5.1 turn that into a terminating error here.
 $previous=$ErrorActionPreference;$ErrorActionPreference='Continue'
 try{
  $output=& powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $script -Package $package -Dest $dest -ReceiptDir $receipts -Label test 2>&1
  $code=$LASTEXITCODE
 }finally{$ErrorActionPreference=$previous}
 [pscustomobject]@{Code=$code;Text=($output|Out-String)}
}
try{
 $old=New-Package 'old' 'old executable' '<title>old</title>'
 $new=New-Package 'new' 'new executable, a little longer' '<title>new</title>'
 $dest=Join-Path $root 'installed';$receipts=Join-Path $root 'receipts'
 New-Item -ItemType Directory -Path $dest|Out-Null
 Copy-Item -Recurse "$old/jisu-browser/*" $dest
 $oldExeHash=(Get-FileHash "$dest/jisu-browser.exe").Hash
 $stableBefore=(Get-Item "$dest/ntp/stable.html").LastWriteTimeUtc

 # A package whose file no longer matches SHA256SUMS.txt is refused and the installation stays as it was.
 $broken=New-Package 'broken' 'broken executable' '<title>broken</title>'
 [IO.File]::WriteAllText("$broken/jisu-browser/ntp/index.html",'<title>tampered</title>')
 $result=Invoke-Update $broken
 if($result.Code -eq 0 -or $result.Text -notmatch 'verification failed'){throw "Tampered package was accepted: $($result.Text)"}
 if((Get-FileHash "$dest/jisu-browser.exe").Hash -ne $oldExeHash -or (Test-Path "$dest/_*")){throw 'Installation changed although the package was rejected'}

 # The real update.
 $result=Invoke-Update $new
 if($result.Code -ne 0){throw "Update failed: $($result.Text)"}
 foreach($file in 'jisu-browser.exe','ntp/index.html'){
  if((Get-FileHash "$dest/$file").Hash -ne (Get-FileHash "$new/jisu-browser/$file").Hash){throw "$file was not updated"}
 }
 if((Get-Item "$dest/ntp/stable.html").LastWriteTimeUtc -ne $stableBefore){throw 'An unchanged file was rewritten'}
 $history=@(Get-ChildItem -Directory $dest -Filter '_*')
 if($history.Count -ne 1){throw 'History folder was not created'}
 $backups=@(Get-ChildItem -File $history[0].FullName|Sort-Object Name)
 if($backups.Count -ne 2 -or ($backups|Where-Object {$_.Name -notmatch '\.bak-\d{8}-\d{6}-test'}).Count -ne 0){throw "Unexpected backups: $($backups.Name -join ', ')"}
 $exeBackup=$backups|Where-Object {$_.Name -like 'jisu-browser.exe.bak-*'}
 if(-not $exeBackup -or (Get-FileHash $exeBackup.FullName).Hash -ne $oldExeHash){throw 'Executable backup differs from the replaced file'}
 if((Get-ChildItem -Recurse -File $dest -Filter '*.jisu-update-stage').Count -ne 0){throw 'Staging file was left behind'}
 $receiptFile=Join-Path $receipts 'update-9.9.9-0123456.json'
 if(-not (Test-Path $receiptFile) -or (Get-Content -Raw $receiptFile) -notmatch 'jisu-browser.exe'){throw 'Receipt missing'}

 # Running it again finds nothing to do: no new backups.
 $result=Invoke-Update $new
 if($result.Code -ne 0 -or @(Get-ChildItem -File $history[0].FullName).Count -ne 2){throw "Second run was not a no-op: $($result.Text)"}
 'PASS CI package update: verifies first, replaces and backs up only changed files, idempotent, rejects tampering'
}finally{Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue}
