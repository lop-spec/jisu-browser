param([Parameter(Mandatory)][string]$Exe,[Parameter(Mandatory)][string]$Expected)
$ErrorActionPreference='Stop'
$v=(Get-Item -LiteralPath $Exe).VersionInfo
if($v.FileVersion -ne $Expected -or $v.ProductVersion -ne $Expected){throw "Resource version mismatch: file=$($v.FileVersion) product=$($v.ProductVersion) expected=$Expected"}
"PASS Windows file/product resource version = $Expected"
