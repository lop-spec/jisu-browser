$ErrorActionPreference='Stop'
# CTest is launched by pwsh on CI; PS 5.1 must not inherit PS 7-only modules.
$env:PSModulePath=(Join-Path $PSHOME 'Modules')+';'+(Join-Path $env:ProgramFiles 'WindowsPowerShell/Modules')
Import-Module Microsoft.PowerShell.Utility
. (Join-Path $PSScriptRoot '../tools/atomic-exe-swap.ps1')
$root=Join-Path $env:TEMP ('jisu-atomic-swap-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root|Out-Null
try{
 $source=Join-Path $root 'source.bin';$destination=Join-Path $root 'destination.bin'
 [IO.File]::WriteAllText($source,'new');[IO.File]::WriteAllText($destination,'old')
 $new=(Get-FileHash $source).Hash;$old=(Get-FileHash $destination).Hash
 $handle=[IO.File]::Open($destination,'Open','Read','Read')
 $blocked=$false
 try{Invoke-JisuAtomicExeSwap $source $destination $new $old}catch{$blocked=$true}finally{$handle.Dispose()}
 if(-not $blocked -or (Get-FileHash $destination).Hash -ne $old){throw 'Locked destination was overwritten'}
 $mismatch=$false
 try{Invoke-JisuAtomicExeSwap $source $destination ('0'*64) $old}catch{$mismatch=$true}
 if(-not $mismatch -or (Get-FileHash $destination).Hash -ne $old){throw 'Bad source checksum was accepted'}
 $mismatch=$false
 try{Invoke-JisuAtomicExeSwap $source $destination $new ('0'*64)}catch{$mismatch=$true}
 if(-not $mismatch -or (Get-FileHash $destination).Hash -ne $old){throw 'Changed destination was overwritten'}
 Invoke-JisuAtomicExeSwap $source $destination $new $old
 if((Get-FileHash $destination).Hash -ne $new){throw 'Atomic swap did not install the new binary'}
 if(Test-Path ($destination+'.jisu-update-stage')){throw 'Temporary file was left behind'}
 'PASS Windows PowerShell 5.1 atomic swap; source/destination checksums; locked EXE fail closed; temporary cleanup'
}finally{Remove-Item -LiteralPath $root -Recurse -Force}
