# Builds and runs every test suite (tests\test_*.cpp -> watt_test_*.exe) against the local test
# servers. Usage: .\test.ps1 [-Dir release] [-NoBuild] [-Suite http]
# Exit code: 0 when every suite passed, 1 otherwise. See docs\BUILDING.md.
param([string]$Dir = "release", [switch]$NoBuild, [string]$Suite = "")
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$bdir = Join-Path $root "build\$Dir"
$suites = Get-ChildItem (Join-Path $root "tests") -Filter "test_*.cpp" | ForEach-Object { "watt_" + $_.BaseName }
if ($Suite) { $suites = $suites | Where-Object { $_ -eq "watt_test_$Suite" } }
if (-not $suites) { throw "no test suite matches '$Suite'" }

if (-not $NoBuild) {
  foreach ($t in @("watt_testserver") + $suites) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root "build.ps1") -Dir $Dir -Target $t
    if ($LASTEXITCODE) { throw "build of $t failed" }
  }
}

# The gRPC / gRPC-Web test server (ports 50051 / 50052) for the suites that need a live peer.
$server = Start-Process (Join-Path $bdir "watt_testserver.exe") -PassThru -WindowStyle Hidden
Start-Sleep -Seconds 2
$failed = @()
try {
  foreach ($t in $suites) {
    $exe = Join-Path $bdir "$t.exe"
    Write-Host "== $t"
    & $exe
    if ($LASTEXITCODE) { $failed += $t }
  }
} finally {
  Stop-Process $server -ErrorAction SilentlyContinue
}
if ($failed) {
  Write-Host "FAILED: $($failed -join ', ')" -ForegroundColor Red
  exit 1
}
Write-Host "All $($suites.Count) suites passed." -ForegroundColor Green
exit 0
