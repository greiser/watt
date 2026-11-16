<#
.SYNOPSIS
  Screenshot an app's main window at a fixed size (default 1600x1000 via SetWindowPos).
.EXAMPLE
  # launch, wait 15 s, resize, capture, leave running
  bench\screenshot_app.ps1 -Exe C:\path\app.exe -Out bench\refs\screens\app.png -Launch -WaitSec 15
  # send keys to the running app, then capture again and kill it
  bench\screenshot_app.ps1 -Exe C:\path\app.exe -Out bench\refs\screens\app-request.png -SendKeys '^n' -Kill
#>
param(
    [Parameter(Mandatory)] [string]$Exe,
    [Parameter(Mandatory)] [string]$Out,
    [switch]$Launch,
    [switch]$Kill,
    [int]$WaitSec = 15,
    [int]$Width = 1600,
    [int]$Height = 1000,
    [int]$X = 40,
    [int]$Y = 20,
    [string]$SendKeys,
    [string]$Clicks,   # "x,y|x,y" relative to the window frame top-left, performed before SendKeys
    [int]$AfterKeysMs = 2500,
    [string[]]$ExtraDir = @()
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'BenchNative.ps1')
Add-Type -AssemblyName System.Drawing
$Exe = (Resolve-Path $Exe).Path
if (-not [IO.Path]::IsPathRooted($Out)) { $Out = Join-Path (Get-Location).Path $Out }
$prefixes = Get-BenchDirPrefixes $Exe $ExtraDir
$cache = New-Object 'System.Collections.Generic.Dictionary[int,string]'
$roots = [int[]]@()
if ($Launch) {
    Stop-BenchApp $Exe $prefixes; Start-Sleep 2
    $p = Start-Process $Exe -WorkingDirectory (Split-Path -Parent $Exe) -PassThru
    $roots = [int[]]@($p.Id)
    Start-Sleep -Seconds $WaitSec
}
$set = [WattBench.Native]::Tree($roots, [string[]]$prefixes, $cache)
$win = Get-BenchMainWindow $set 300 200
if (-not $win) { throw "No main window found for $Exe" }
Write-Host "window: $win"
[void][WattBench.Native]::ShowWindow($win.Hwnd, 9)   # SW_RESTORE (un-maximize so SetWindowPos sticks)
[void][WattBench.Native]::SetWindowPos($win.Hwnd, [IntPtr]::Zero, $X, $Y, $Width, $Height, 0x0040)  # SWP_SHOWWINDOW
# Alt tap lets SetForegroundWindow succeed from a background process
[WattBench.Native]::Key(0x12, $false); [WattBench.Native]::Key(0x12, $true)
[void][WattBench.Native]::SetForegroundWindow($win.Hwnd); [void][WattBench.Native]::BringWindowToTop($win.Hwnd)
Start-Sleep -Milliseconds 1500
if ($Clicks) {
    $fr = [WattBench.Native]::FrameRect($win.Hwnd)
    foreach ($pt in ($Clicks -split '\|')) { $xy = $pt -split ','; [WattBench.Native]::Click($fr.Left + [int]$xy[0], $fr.Top + [int]$xy[1]); Start-Sleep -Milliseconds 1200 }
    Start-Sleep -Milliseconds $AfterKeysMs
}
if ($SendKeys) {
    $sh = New-Object -ComObject WScript.Shell
    foreach ($k in ($SendKeys -split '\|')) { $sh.SendKeys($k); Start-Sleep -Milliseconds 700 }
    Start-Sleep -Milliseconds $AfterKeysMs
    [void][WattBench.Native]::SetForegroundWindow($win.Hwnd)
    Start-Sleep -Milliseconds 300
}
$rect = [WattBench.Native]::FrameRect($win.Hwnd)
$w = $rect.Right - $rect.Left; $h = $rect.Bottom - $rect.Top
$bmp = New-Object System.Drawing.Bitmap $w, $h
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($rect.Left, $rect.Top, 0, 0, (New-Object System.Drawing.Size $w, $h))
$g.Dispose()
New-Item -ItemType Directory -Force (Split-Path -Parent $Out) | Out-Null
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
Write-Host "saved $Out (${w}x${h} frame; outer window ${Width}x${Height})"
if ($Kill) { Stop-BenchApp $Exe $prefixes }

