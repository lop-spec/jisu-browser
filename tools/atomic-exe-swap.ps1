# Caller must physically back up the old EXE before invoking this function.
# No process is terminated. A locked destination fails without modifying it.
function Invoke-JisuAtomicExeSwap {
 param(
  [Parameter(Mandatory=$true)][string]$Source,
  [Parameter(Mandatory=$true)][string]$Destination,
  [Parameter(Mandatory=$true)][string]$ExpectedSha256,
  [Parameter(Mandatory=$true)][string]$ExpectedOldSha256
 )
 $ErrorActionPreference='Stop'
 $Source=[IO.Path]::GetFullPath($Source)
 $Destination=[IO.Path]::GetFullPath($Destination)
 if((Get-FileHash -LiteralPath $Source -Algorithm SHA256).Hash -ine $ExpectedSha256){throw 'CI source checksum mismatch'}
 if((Get-FileHash -LiteralPath $Destination -Algorithm SHA256).Hash -ine $ExpectedOldSha256){throw 'Installed binary changed; refusing to overwrite'}
 $temporary=$Destination+'.jisu-update-stage'
 try{
  Copy-Item -LiteralPath $Source -Destination $temporary -Force
  if((Get-FileHash -LiteralPath $temporary -Algorithm SHA256).Hash -ine $ExpectedSha256){throw 'Staging checksum mismatch'}
  # In Windows PowerShell 5.1, $null is bound to an empty string for this .NET
  # overload and causes ArgumentException. Pass a real CLR null string instead.
  [IO.File]::Replace($temporary,$Destination,[NullString]::Value)
  if((Get-FileHash -LiteralPath $Destination -Algorithm SHA256).Hash -ine $ExpectedSha256){throw 'Installed executable checksum mismatch'}
 }finally{
  if(Test-Path -LiteralPath $temporary){Remove-Item -LiteralPath $temporary -Force}
 }
}
