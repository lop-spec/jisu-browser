<#
汇总 run-g1g2.ps1 的结果：G1/G2 中位数与判定、外部计时对照、启动时间线各阶段中位数。
输出同时写到 <OutDir>\<Label>-summary.txt 与 <Label>-summary.json。
  G1：进程创建 → 窗口首帧上屏（外壳内部 firstFramePresented，DwmFlush 之后）≤ 70ms
  G2：进程创建 → 新标签页内容首帧（ntpFirstFrameHost，口径同可行性原型的 webFirstFrameMs）≤ 500ms；预热时 ≤ 300ms
#>
param(
    [Parameter(Mandatory = $true)][string]$OutDir,
    [string]$Label = "default",
    [switch]$Preheat
)
$ErrorActionPreference = "Stop"

function Get-Sorted([double[]]$values) { return , @($values | Sort-Object) }

function Get-Median([double[]]$values) {
    $v = Get-Sorted $values
    if ($v.Count -eq 0) { return [double]::NaN }
    if ($v.Count % 2 -eq 1) { return $v[[int](($v.Count - 1) / 2)] }
    return ($v[[int]($v.Count / 2 - 1)] + $v[[int]($v.Count / 2)]) / 2
}

function Get-Quantile([double[]]$values, [double]$q) {
    $v = Get-Sorted $values
    if ($v.Count -eq 0) { return [double]::NaN }
    $index = [int][math]::Ceiling($q * $v.Count) - 1
    if ($index -lt 0) { $index = 0 }
    return $v[$index]
}

$pattern = "^" + [regex]::Escape($Label) + "-\d\d\.json$"
$rows = @()
foreach ($file in (Get-ChildItem -LiteralPath $OutDir -Filter "*.json" | Where-Object { $_.Name -match $pattern } | Sort-Object Name)) {
    $inner = Get-Content -LiteralPath $file.FullName -Raw -Encoding UTF8 | ConvertFrom-Json
    $outerPath = Join-Path $OutDir ($file.BaseName + ".launch.json")
    $outer = $null
    if (Test-Path -LiteralPath $outerPath) { $outer = Get-Content -LiteralPath $outerPath -Raw -Encoding UTF8 | ConvertFrom-Json }
    $rows += [pscustomobject]@{ Name = $file.BaseName; Inner = $inner; Outer = $outer }
}
if ($rows.Count -eq 0) { throw "没有找到 $Label-NN.json：$OutDir" }

function Get-Mark([string]$key) {
    $values = @()
    foreach ($row in $rows) {
        $v = $row.Inner.t.$key
        if ($null -ne $v) { $values += [double]$v }
    }
    return , $values
}

function Get-Delta([string]$from, [string]$to) {
    $values = @()
    foreach ($row in $rows) {
        $a = $row.Inner.t.$from
        $b = $row.Inner.t.$to
        if ($null -ne $a -and $null -ne $b) { $values += ([double]$b - [double]$a) }
    }
    return , $values
}

function Format-Row([string]$name, [double[]]$values) {
    if ($values.Count -eq 0) { return ("{0,-34} （无数据）" -f $name) }
    "{0,-34} 中位 {1,8:N1}  最小 {2,8:N1}  P90 {3,8:N1}  最大 {4,8:N1}  n={5}" -f $name, (Get-Median $values),
        (Get-Quantile $values 0.0), (Get-Quantile $values 0.9), (Get-Quantile $values 1.0), $values.Count
}

$g1 = Get-Mark "firstFramePresented"
$g2 = Get-Mark "ntpFirstFrameHost"
$external = @()
$clockGap = @()
foreach ($row in $rows) {
    if ($row.Outer -and $null -ne $row.Outer.windowShownMs) {
        $external += [double]$row.Outer.windowShownMs
        if ($null -ne $row.Inner.t.windowShown) {
            $clockGap += [double]$row.Outer.windowShownMs - [double]$row.Inner.t.windowShown
        }
    }
}

$g1Median = Get-Median $g1
$g2Median = Get-Median $g2
$g2Limit = 500
if ($Preheat) { $g2Limit = 300 }
$g1Pass = ($g1.Count -gt 0) -and ($g1Median -le 70)
$g2Pass = ($g2.Count -gt 0) -and ($g2Median -le $g2Limit)
$first = $rows[0].Inner
$missing = @($rows | Where-Object { $null -eq $_.Inner.t.firstFramePresented -or $null -eq $_.Inner.t.ntpFirstFrameHost } | ForEach-Object { $_.Name })
$forced = @($rows | Where-Object { $_.Inner.forced -eq 1 } | ForEach-Object { $_.Name })

$lines = @()
$lines += "== $Label  （$($rows.Count) 轮，预热=$([bool]$Preheat)）"
$lines += "运行时 $($first.runtimeVersion)  SDK $($first.sdkVersion)  提交 $($first.commit)  DPI $($first.dpi)  刷新率 $($first.refreshHz)Hz  硬件渲染目标 $($first.rtHardware)  主题 $($first.theme)"
$lines += ("G1 窗口首帧上屏（≤70ms）            中位 {0:N1} ms → {1}" -f $g1Median, $(if ($g1Pass) { "通过" } else { "不通过" }))
$lines += ("G2 新标签页内容首帧（≤{0}ms）       中位 {1:N1} ms → {2}" -f $g2Limit, $g2Median, $(if ($g2Pass) { "通过" } else { "不通过" }))
$lines += ""
$lines += Format-Row "G1 firstFramePresented" $g1
$lines += Format-Row "G2 ntpFirstFrameHost" $g2
$lines += Format-Row "外部：CreateProcess→窗口显示" $external
$lines += Format-Row "对时：外部显示−内部 windowShown" $clockGap
$lines += Format-Row "ntpFcp（页面 FCP 呈现时刻）" (Get-Mark "ntpFcp")
$lines += Format-Row "ntpFirstFramePage（页面 rAF 后）" (Get-Mark "ntpFirstFramePage")
$lines += ""
$lines += "-- 阶段耗时"
$lines += Format-Row "环境发起同步部分 envBegin→Returned" (Get-Delta "envBegin" "envCallReturned")
$lines += Format-Row "环境就绪 envBegin→envReady" (Get-Delta "envBegin" "envReady")
$lines += Format-Row "首个控件 controllerBegin→Ready" (Get-Delta "controllerBegin" "controllerReady")
$lines += Format-Row "导航→页面 timeOrigin" (Get-Delta "ntpNavigate" "ntpTimeOrigin")
$lines += Format-Row "导航→新标签页首帧" (Get-Delta "ntpNavigate" "ntpFirstFrameHost")
$lines += Format-Row "首帧 EndDraw→上屏" (Get-Delta "firstPaint" "firstFramePresented")
$lines += Format-Row "备用控件 spareBegin→Ready" (Get-Delta "spareBegin" "spareReady")
$lines += ""
$lines += "-- 时间线（距进程创建，ms）"
$keys = @("mainEnter", "warmupBegin", "d2dFactoryReady", "dwriteReady", "d3dWarmReady", "comReady", "addressBarCreated",
    "windowCreated", "envBegin", "envCallReturned", "warmupJoined", "renderTargetCreated", "windowShown", "firstPaint",
    "firstFramePresented", "envReady", "controllerBegin", "controllerReady", "ntpNavigate", "ntpTimeOrigin",
    "ntpNavCompleted", "ntpFcp", "ntpFirstFramePage", "ntpFirstFrameHost", "spareBegin", "spareReady")
foreach ($key in $keys) { $lines += Format-Row $key (Get-Mark $key) }
if ($missing.Count -gt 0) { $lines += "缺 G1/G2 数据的轮次：$($missing -join ', ')" }
if ($forced.Count -gt 0) { $lines += "超时强制写出的轮次：$($forced -join ', ')" }

$text = $lines -join "`r`n"
Write-Host $text
$text | Set-Content -LiteralPath (Join-Path $OutDir ($Label + "-summary.txt")) -Encoding UTF8

$summary = [ordered]@{
    label = $Label; preheat = [bool]$Preheat; runs = $rows.Count
    runtimeVersion = $first.runtimeVersion; sdkVersion = $first.sdkVersion; commit = $first.commit
    g1MedianMs = $g1Median; g1Pass = $g1Pass; g1Values = $g1
    g2MedianMs = $g2Median; g2LimitMs = $g2Limit; g2Pass = $g2Pass; g2Values = $g2
    externalWindowShownMedianMs = (Get-Median $external); externalValues = $external
    clockGapMedianMs = (Get-Median $clockGap)
}
$summary | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $OutDir ($Label + "-summary.json")) -Encoding UTF8
