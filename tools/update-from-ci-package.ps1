# 用云端 CI 产出的运行包更新已经装好的极速浏览器，浏览器正在运行也可以更新，不结束任何进程。
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\update-from-ci-package.ps1 -Package <解压后的产物目录> [-SetMediumLabel]
# -Package：解压后的 jisu-browser-win-x64-<提交> 目录（里面有 SHA256SUMS.txt 和 jisu-browser\）。
# 做法：逐个文件按 SHA256SUMS.txt 核对大小与哈希；只处理内容有变化的文件，先备份到 <Dest>\_历史版本\（名字
# <原名>.bak-<时间>-<标签>，读回哈希确认），再用 File.Replace 换上新文件；正在运行的程序继续用旧映像，重启后才是新版本。
# 任何一步失败都停下，已有文件保持原样。播放器 jisu-media.exe 正在运行时整体放弃（不结束播放）。
# -SetMediumLabel：给 jisu-browser.exe 加 Medium 完整性标签（与 Pi 同级，应用窗口和日常窗口才共用一个内核进程）；
# 本机其余进程都是 High 时（例如关了 UAC 的机器）不要加。
param(
  [Parameter(Mandatory = $true)][string]$Package,
  [string]$Dest = (Join-Path $env:LOCALAPPDATA 'Programs\JisuBrowser'),
  [string]$Label = 'update',
  [string]$ReceiptDir = (Join-Path $env:LOCALAPPDATA 'JisuBrowser'),
  [switch]$SetMediumLabel
)
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.Encoding]::UTF8
$Package = [IO.Path]::GetFullPath($Package)
$Dest = [IO.Path]::GetFullPath($Dest)
$manifest = Join-Path $Package 'SHA256SUMS.txt'
if (-not (Test-Path -LiteralPath $manifest)) { throw "Package has no SHA256SUMS.txt: $Package" }

# 1. 整个包先核对完，再动安装目录
$items = @()
foreach ($line in [IO.File]::ReadAllLines($manifest)) {
  $line = $line.TrimStart([char]0xFEFF)
  if ($line.Trim().Length -eq 0) { continue }
  if ($line -notmatch '^([a-f0-9]{64})\s+(\d+)\s+(.+)$') { throw "Invalid checksum line: $line" }
  $hash = $Matches[1]; $size = [long]$Matches[2]; $relative = $Matches[3]
  $source = Join-Path $Package ($relative -replace '/', '\')
  if (-not (Test-Path -LiteralPath $source) -or (Get-Item -LiteralPath $source).Length -ne $size -or
      (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash -ine $hash) {
    throw "Package verification failed: $relative"
  }
  if ($relative.StartsWith('jisu-browser/')) {
    $inner = $relative.Substring('jisu-browser/'.Length)
    $items += @{ Source = $source; Target = (Join-Path $Dest ($inner -replace '/', '\')); Hash = $hash; Relative = $inner }
  }
}
if ($items.Count -eq 0) { throw 'Package has no jisu-browser/ files' }
$activeMedia = @(Get-CimInstance Win32_Process -Filter "Name='jisu-media.exe'" | Where-Object { $_.ExecutablePath -like ($Dest + '*') })
if ($activeMedia.Count -gt 0) { throw 'Installed media player is running; nothing was changed and no playback was stopped' }

# 2. 只换有变化的文件：先备份，再原子替换
$history = Join-Path $Dest '_历史版本'
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$pidsBefore = @(Get-CimInstance Win32_Process -Filter "Name='jisu-browser.exe'" | Select-Object -ExpandProperty ProcessId)
$changed = @()
foreach ($item in $items) {
  $target = $item.Target; $old = $null
  if (Test-Path -LiteralPath $target) {
    $old = (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash
    if ($old -ieq $item.Hash) { continue }
    New-Item -ItemType Directory -Force -Path $history | Out-Null
    # 子目录里的文件把相对路径写进标签，避免不同目录的同名文件互相覆盖备份
    $slug = if ($item.Relative.Contains('\')) { $Label + '-' + ($item.Relative -replace '[^A-Za-z0-9.]+', '-') } else { $Label }
    $backup = Join-Path $history ((Split-Path -Leaf $target) + '.bak-' + $stamp + '-' + $slug)
    Copy-Item -LiteralPath $target -Destination $backup
    if ((Get-FileHash -LiteralPath $backup -Algorithm SHA256).Hash -ine $old) { throw "Backup verification failed: $target" }
  }
  New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
  $temporary = $target + '.jisu-update-stage'
  try {
    Copy-Item -LiteralPath $item.Source -Destination $temporary -Force
    if ((Get-FileHash -LiteralPath $temporary -Algorithm SHA256).Hash -ine $item.Hash) { throw "Staging hash mismatch: $target" }
    if ($old) {
      if ((Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -ine $old) { throw "Installed file changed concurrently: $target" }
      # File.Replace 把正在运行的 exe 改名挪开，不需要结束它；PS 5.1 要传真正的 null 字符串
      [IO.File]::Replace($temporary, $target, [NullString]::Value)
    } else {
      [IO.File]::Move($temporary, $target)
    }
  } finally {
    if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary -Force }
  }
  $changed += $item.Relative
}

# 3. 完整性标签、整包读回、回执
$exe = Join-Path $Dest 'jisu-browser.exe'
if ($SetMediumLabel) {
  & icacls.exe $exe /setintegritylevel medium | Out-Null
  if ($LASTEXITCODE -ne 0) { throw 'Setting the Medium integrity label failed' }
  if (((& icacls.exe $exe) -join "`n") -notmatch 'Medium Mandatory Level') { throw 'Medium integrity label missing after update' }
}
$bytes = 0L
foreach ($item in $items) {
  if ((Get-FileHash -LiteralPath $item.Target -Algorithm SHA256).Hash -ine $item.Hash) { throw "Installed package verification failed: $($item.Target)" }
  $bytes += (Get-Item -LiteralPath $item.Target).Length
}
$smoke = Join-Path $Package 'ci-smoke.json'
$info = if (Test-Path -LiteralPath $smoke) { Get-Content -LiteralPath $smoke -Raw -Encoding UTF8 | ConvertFrom-Json } else { $null }
$receipt = [ordered]@{
  host = $env:COMPUTERNAME
  version = if ($info) { $info.version } else { $null }
  commit = if ($info) { $info.commit } else { $null }
  files = $items.Count
  changed = $changed
  bytes = $bytes
  mainSha256 = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToLowerInvariant()
  pidsBefore = $pidsBefore
  pidsAfter = @(Get-CimInstance Win32_Process -Filter "Name='jisu-browser.exe'" | Select-Object -ExpandProperty ProcessId)
  restarted = $false
  pending = if ($changed.Count -gt 0) { 'Exit and reopen the daily browser (and open app windows) to load the new binary' } else { 'Nothing to update' }
}
if ($changed.Count -gt 0) {
  New-Item -ItemType Directory -Force -Path $ReceiptDir | Out-Null
  $name = 'update-' + $(if ($info) { [string]$info.version + '-' + ([string]$info.commit).Substring(0, 7) } else { $stamp }) + '.json'
  [IO.File]::WriteAllText((Join-Path $ReceiptDir $name), ($receipt | ConvertTo-Json -Depth 4), (New-Object Text.UTF8Encoding($false)))
}
$receipt | ConvertTo-Json -Depth 4
