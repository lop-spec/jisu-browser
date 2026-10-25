# 安装极速浏览器（当前用户，无需管理员）：把 CI 运行包复制到程序目录，建快捷方式，可选写入智能体桥配置。
# 用法：
#   powershell -NoProfile -ExecutionPolicy Bypass -File install.ps1 -Package <含 jisu-browser.exe 与 ntp\ 的目录>
#     [-Dest <程序目录>] [-ShortcutDir <快捷方式目录>] [-StartMenu] [-AgentTokenFile <Pi 的 extension-auth.json>]
#     [-YieldTo thorium.exe] [-ForceAgentConfig]
# 已在运行的极速浏览器不会被结束：发现它在用目标目录里的程序就停下，提示先关掉。
param(
  [Parameter(Mandatory = $true)][string]$Package,
  [string]$Dest = (Join-Path $env:LOCALAPPDATA "Programs\JisuBrowser"),
  [string]$ShortcutDir = "",
  [switch]$StartMenu,
  [string]$AgentTokenFile = "",
  [string[]]$YieldTo = @(),
  [switch]$ForceAgentConfig
)
$ErrorActionPreference = "Stop"

$exeSource = Join-Path $Package "jisu-browser.exe"
foreach ($required in @($exeSource, (Join-Path $Package "ntp\index.html"), (Join-Path $Package "ntp\bookmarks.html"))) {
  if (-not (Test-Path -LiteralPath $required)) { throw "运行包不完整，缺少 $required" }
}

$destExe = Join-Path $Dest "jisu-browser.exe"
$running = Get-CimInstance Win32_Process -Filter "Name = 'jisu-browser.exe'" |
  Where-Object { $_.ExecutablePath -and ([IO.Path]::GetFullPath($_.ExecutablePath) -ieq [IO.Path]::GetFullPath($destExe)) }
if ($running) { throw "极速浏览器正在运行（PID $($running.ProcessId -join ', ')），请先关掉再安装" }

# 覆盖前留一份旧程序，便于回退
if (Test-Path -LiteralPath $destExe) {
  $history = Join-Path $Dest "_历史版本"
  New-Item -ItemType Directory -Force -Path $history | Out-Null
  $stamp = Get-Date -Format "yyyyMMdd-HHmmss"
  Copy-Item -LiteralPath $destExe -Destination (Join-Path $history "jisu-browser.exe.bak-$stamp")
}
New-Item -ItemType Directory -Force -Path $Dest | Out-Null
Copy-Item -LiteralPath $exeSource -Destination $destExe -Force
$ntpDest = Join-Path $Dest "ntp"
if (Test-Path -LiteralPath $ntpDest) { Remove-Item -LiteralPath $ntpDest -Recurse -Force }
Copy-Item -LiteralPath (Join-Path $Package "ntp") -Destination $ntpDest -Recurse

$hashSource = (Get-FileHash -Algorithm SHA256 -LiteralPath $exeSource).Hash
$hashDest = (Get-FileHash -Algorithm SHA256 -LiteralPath $destExe).Hash
if ($hashSource -ne $hashDest) { throw "复制后校验不一致" }

# 智能体桥配置：只在给了令牌文件时写；已有配置不覆盖，除非 -ForceAgentConfig
$configRoot = Join-Path $env:LOCALAPPDATA "JisuBrowser"
$agentConfig = Join-Path $configRoot "agent-bridge.json"
$agentWritten = $false
if ($AgentTokenFile) {
  if (-not (Test-Path -LiteralPath $AgentTokenFile)) { throw "找不到令牌文件 $AgentTokenFile" }
  if ($ForceAgentConfig -or -not (Test-Path -LiteralPath $agentConfig)) {
    New-Item -ItemType Directory -Force -Path $configRoot | Out-Null
    if (Test-Path -LiteralPath $agentConfig) {
      Copy-Item -LiteralPath $agentConfig -Destination "$agentConfig.bak-$(Get-Date -Format 'yyyyMMdd-HHmmss')"
    }
    # 只写连接所需的项；省电相关取程序里的默认值（见 README），以后升级跟着新默认走
    $config = [ordered]@{
      broker    = "http://127.0.0.1:18773"
      tokenFile = $AgentTokenFile
      yieldTo   = @($YieldTo)
    }
    # ConvertTo-Json 在 PS 5.1 里会把单元素数组压成字符串：单独处理
    $json = $config | ConvertTo-Json -Depth 3
    if ($YieldTo.Count -eq 1) { $json = $json -replace '"yieldTo":\s*"([^"]*)"', '"yieldTo": [ "$1" ]' }
    if ($YieldTo.Count -eq 0) { $json = $json -replace '"yieldTo":\s*(null|\{\s*\}|"")', '"yieldTo": [ ]' }
    [IO.File]::WriteAllText($agentConfig, $json, (New-Object Text.UTF8Encoding($false)))
    $agentWritten = $true
  }
}

function New-Shortcut([string]$path) {
  $shell = New-Object -ComObject WScript.Shell
  $link = $shell.CreateShortcut($path)
  $link.TargetPath = $destExe
  $link.WorkingDirectory = $Dest
  $link.IconLocation = "$destExe,0"
  $link.Description = "极速浏览器"
  $link.Save()
}
$shortcuts = @()
if ($ShortcutDir) {
  New-Item -ItemType Directory -Force -Path $ShortcutDir | Out-Null
  $path = Join-Path $ShortcutDir "极速浏览器.lnk"
  New-Shortcut $path
  $shortcuts += $path
}
if ($StartMenu) {
  $path = Join-Path ([Environment]::GetFolderPath("Programs")) "极速浏览器.lnk"
  New-Shortcut $path
  $shortcuts += $path
}

$version = (Get-Item -LiteralPath $destExe).VersionInfo.FileVersion
[ordered]@{
  installed   = $destExe
  version     = $version
  sha256      = $hashDest.ToLowerInvariant()
  bytes       = (Get-Item -LiteralPath $destExe).Length
  agentConfig = $(if ($agentWritten) { $agentConfig } elseif (Test-Path -LiteralPath $agentConfig) { "$agentConfig（保留原有）" } else { "未配置" })
  shortcuts   = $shortcuts
} | ConvertTo-Json
