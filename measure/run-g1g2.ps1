<#
G1/G2 实测（在本机跑 CI 产物）。
- 外部计时：可行性原型（不在本仓库）里 jisu-probe.exe 的 launch 模式，CreateProcess 前打点，用 WinEventHook 捕获外壳第一个顶层窗口显示。
  默认找与本仓库同级的 spike\bin\jisu-probe.exe，可用 -Probe 指定。
- 内部计时：外壳 --metrics 写的启动时间线，零点 = 进程创建时间（GetProcessTimes）。
- 非预热：每轮之间等同一用户数据目录的 msedgewebview2.exe 全部退出，每轮都是冷内核进程、热文件缓存；先跑 1 轮暖场不计入。
- 预热（-Preheat）：另起一个外壳以 --preheat 持有同一用户数据目录的内核进程，对应开机预热 / Edge 启动加速。
- 窗口以 --no-activate 显示，不抢焦点；但测量期间会反复弹出约 1 秒的测试窗口。
- 有人在用这台机器时：-WaitUserIdle <秒> 让每一轮都等键鼠空闲满这么久再跑（中途有人碰键鼠就暂停）；
  -AbortOnInput 则在开跑后一旦有键鼠输入就整批放弃并以退出码 3 结束（采 ETW 时用）。
用法：
  powershell -NoProfile -ExecutionPolicy Bypass -File run-g1g2.ps1 -Exe <jisu-browser.exe> -OutDir <结果目录> [-Label default] [-Runs 20] [-ShellArgs "--rt hw"] [-Preheat] [-WaitUserIdle 300] [-AbortOnInput]
#>
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [Parameter(Mandatory = $true)][string]$OutDir,
    [string]$Label = "default",
    [int]$Runs = 20,
    [string]$ShellArgs = "",
    [switch]$Preheat,
    [string]$Probe = "",
    [int]$WaitUserIdle = 0,
    [switch]$AbortOnInput
)
$ErrorActionPreference = "Stop"

Add-Type -Namespace JisuMeasure -Name Input -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool GetLastInputInfo(ref LASTINPUTINFO info);
[DllImport("kernel32.dll")] public static extern uint GetTickCount();
[System.Runtime.InteropServices.StructLayout(System.Runtime.InteropServices.LayoutKind.Sequential)]
public struct LASTINPUTINFO { public uint cbSize; public uint dwTime; }
'@

function Get-TickSpan([uint32]$from, [uint32]$to) {
    $d = [int64]$to - [int64]$from
    if ($d -lt 0) { $d += 4294967296 }  # GetTickCount 约 49.7 天回绕一次
    return $d
}

function Get-UserIdleMs {
    $info = New-Object JisuMeasure.Input+LASTINPUTINFO
    $info.cbSize = 8
    [void][JisuMeasure.Input]::GetLastInputInfo([ref]$info)
    return Get-TickSpan $info.dwTime ([JisuMeasure.Input]::GetTickCount())
}

function Wait-UserAway {
    if ($WaitUserIdle -le 0) { return }
    $announced = $false
    while ((Get-UserIdleMs) -lt $WaitUserIdle * 1000) {
        if (-not $announced) { Write-Host ("等待键鼠空闲满 {0} 秒……" -f $WaitUserIdle); $announced = $true }
        Start-Sleep -Seconds 15
    }
}

$batchStart = [JisuMeasure.Input]::GetTickCount()
function Test-InputSinceStart {
    $now = [JisuMeasure.Input]::GetTickCount()
    return (Get-UserIdleMs) -lt (Get-TickSpan $batchStart $now)
}

function Assert-NoInput {
    if ($AbortOnInput -and (Test-InputSinceStart)) {
        Write-Host "开跑后检测到键鼠输入，整批放弃"
        exit 3
    }
}
if (-not $Probe) { $Probe = Join-Path $PSScriptRoot "..\..\spike\bin\jisu-probe.exe" }
$Probe = (Resolve-Path -LiteralPath $Probe).Path
$Exe = (Resolve-Path -LiteralPath $Exe).Path
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$OutDir = (Resolve-Path -LiteralPath $OutDir).Path
$udf = Join-Path $OutDir ("udf-" + $Label)

function Get-WebViewProcesses([string]$userData) {
    @(Get-CimInstance Win32_Process -Filter "Name='msedgewebview2.exe'" |
        Where-Object { $_.CommandLine -and $_.CommandLine.Contains($userData) })
}

function Wait-Idle([string]$userData) {
    for ($i = 0; $i -lt 150; $i++) {
        if ((Get-WebViewProcesses $userData).Count -eq 0) { return }
        Start-Sleep -Milliseconds 200
    }
    Write-Warning "30 秒内 msedgewebview2 仍未退出：$userData"
}

function Invoke-Run([string]$name) {
    Wait-UserAway
    Assert-NoInput
    $inner = Join-Path $OutDir ($name + ".json")
    $outer = Join-Path $OutDir ($name + ".launch.json")
    Remove-Item -LiteralPath $inner, $outer -ErrorAction SilentlyContinue
    $probeArgs = @("launch", $outer, "x", $Exe, "--metrics", $inner, "--udf", $udf, "--exit-after-metrics",
        "--no-activate", "--bounds", "40,40,1280,860")
    if ($ShellArgs) { $probeArgs += $ShellArgs.Split(" ", [StringSplitOptions]::RemoveEmptyEntries) }
    $line = ($probeArgs | ForEach-Object { '"' + $_ + '"' }) -join " "
    $p = Start-Process -FilePath $Probe -ArgumentList $line -PassThru -Wait
    $state = "ok"
    if (-not (Test-Path -LiteralPath $inner)) { $state = "缺外壳时间线" }
    elseif (-not (Test-Path -LiteralPath $outer)) { $state = "缺外部计时" }
    Write-Host ("{0,-22} probe exit={1} {2}" -f $name, $p.ExitCode, $state)
}

$holder = $null
Wait-UserAway
$batchStart = [JisuMeasure.Input]::GetTickCount()
if ($Preheat) {
    # 持有进程带上同样的 -ShellArgs：同一用户数据目录的环境选项必须一致，否则被测进程建环境会报 ERROR_INVALID_STATE
    $holderArgs = '--preheat 1800 --udf "' + $udf + '"'
    if ($ShellArgs) { $holderArgs += " " + $ShellArgs }
    $holder = Start-Process -FilePath $Exe -ArgumentList $holderArgs -PassThru
    Start-Sleep -Seconds 8
}
try {
    Invoke-Run ($Label + "-warmup")
    for ($i = 0; $i -le $Runs; $i++) {
        if ($i -gt 0) { Invoke-Run ("{0}-{1:d2}" -f $Label, $i) }
        if ($Preheat) { Start-Sleep -Milliseconds 800 } else { Wait-Idle $udf }
        Start-Sleep -Milliseconds 300
    }
}
finally {
    if ($holder) {
        Stop-Process -Id $holder.Id -ErrorAction SilentlyContinue
        Wait-Idle $udf
    }
}

& (Join-Path $PSScriptRoot "summarize-g1g2.ps1") -OutDir $OutDir -Label $Label -Preheat:$Preheat
