# MineKey 配置器诊断
# 用法：右键「使用 PowerShell 运行」，或：
#   powershell -ExecutionPolicy Bypass -File diagnose.ps1 -AppDir "C:\path\to\MineKey-v1.3"
# 结果会打印在窗口里，同时写到 diagnose.txt（和本脚本同目录）。

param(
    [string]$AppDir = ""
)

$ErrorActionPreference = "SilentlyContinue"
$lines = New-Object System.Collections.Generic.List[string]

function Add($s) { $lines.Add($s); Write-Host $s }

Add "=== MineKey 诊断 ==="
Add ("时间: " + (Get-Date -Format "yyyy-MM-dd HH:mm:ss"))

# ---------- 系统 ----------
$os = Get-CimInstance Win32_OperatingSystem
Add ""
Add "=== 系统 ==="
Add ("OS      : " + $os.Caption + " (" + $os.Version + ")")
Add ("架构    : " + $env:PROCESSOR_ARCHITECTURE)
Add ("用户    : " + $env:USERNAME)

# ---------- WebView2 ----------
Add ""
Add "=== WebView2 运行时 ==="
$guid = "{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}"
$paths = @(
    "HKLM:\SOFTWARE\WOW6432Node\Microsoft\EdgeUpdate\Clients\$guid",
    "HKLM:\SOFTWARE\Microsoft\EdgeUpdate\Clients\$guid",
    "HKCU:\SOFTWARE\Microsoft\EdgeUpdate\Clients\$guid"
)
$wv2 = $null
foreach ($p in $paths) {
    if (Test-Path $p) {
        $v = (Get-ItemProperty $p).pv
        if ($v) { $wv2 = $v; break }
    }
}
if ($wv2) {
    Add ("已安装  : " + $wv2)
} else {
    Add "!! 没有检测到 WebView2 运行时 —— 配置器会白屏或根本打不开界面"
    Add "   解决：装一次 https://developer.microsoft.com/microsoft-edge/webview2/"
}

# ---------- 程序目录 ----------
Add ""
Add "=== 程序文件 ==="
if ($AppDir -eq "") {
    $scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
    if (Test-Path (Join-Path $scriptDir "MineKey.exe")) { $AppDir = $scriptDir }
}
if ($AppDir -ne "") {
    Add ("目录    : " + $AppDir)
    $exe = Join-Path $AppDir "MineKey.exe"
    $idx = Join-Path $AppDir "www\index.html"
    $js  = Join-Path $AppDir "www\app.js"
    $okExe = if (Test-Path $exe) { "存在" } else { "!! 缺失" }
    $okIdx = if (Test-Path $idx) { "存在" } else { "!! 缺失 - 界面会白屏" }
    $okJs  = if (Test-Path $js)  { "存在" } else { "!! 缺失" }
    Add ("MineKey.exe    : " + $okExe)
    Add ("www\index.html : " + $okIdx)
    Add ("www\app.js     : " + $okJs)
} else {
    Add "!! 未指定程序目录，跳过（把本脚本放到 MineKey.exe 旁边再跑）"
}

# ---------- HID 设备 ----------
Add ""
Add "=== HID 设备（关键：找 1209:0001） ==="
$hids = Get-PnpDevice -Class HIDClass -PresentOnly
if (-not $hids) {
    Add "!! 读不到 HID 设备列表"
} else {
    Add ("HID 设备总数: " + @($hids).Count)
    $mine = $hids | Where-Object { $_.InstanceId -like "*VID_1209*" -or $_.InstanceId -like "*PID_0001*" }
    if ($mine) {
        Add "找到 MineKey 设备："
        foreach ($d in $mine) {
            Add ("  " + $d.Status + "  " + $d.FriendlyName)
            Add ("    " + $d.InstanceId)
        }
    } else {
        Add "!! 没有找到 VID_1209 / PID_0001 的设备"
        Add "   说明系统层面就没认到键盘，不是配置器的问题"
    }
}

# ---------- 相关异常设备 ----------
Add ""
Add "=== 异常设备（未知设备 / 感叹号） ==="
$bad = Get-PnpDevice -PresentOnly | Where-Object { $_.Status -ne "OK" -and $_.Status -ne "Unknown" }
if ($bad) {
    foreach ($d in $bad) { Add ("  " + $d.Status + "  " + $d.Class + "  " + $d.FriendlyName) }
} else {
    Add "  无"
}

# ---------- BootLoader ----------
Add ""
Add "=== 是否停在 BootLoader（VID_4348） ==="
$boot = Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like "*VID_4348*" }
if ($boot) {
    foreach ($d in $boot) { Add ("  " + $d.Status + "  " + $d.FriendlyName + "  " + $d.InstanceId) }
    Add "  注意：BootLoader 状态下键盘不出键、配置器也连不上，拔插一次即可"
} else {
    Add "  否"
}

# ---------- 输出 ----------
Add ""
Add "=== 结束 ==="
$outFile = Join-Path (Get-Location) "diagnose.txt"
$lines | Set-Content -Path $outFile -Encoding UTF8
Write-Host ""
Write-Host ("报告已保存: " + $outFile) -ForegroundColor Green
