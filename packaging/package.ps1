# Builds the Windows distribution of the Web API Testing Toolkit (watt):
#   dist\watt-<version>-windows-x64\       the unpacked application folder
#   dist\watt-<version>-windows-x64.zip    the same folder, zipped
#   dist\watt-<version>-windows-x64.zip.sha256
#
#   .\packaging\package.ps1              Release build of watt.exe, then package it
#   .\packaging\package.ps1 -NoBuild     package what build\release already holds
#
# The DLLs are found by walking watt.exe's imports (dumpbin /dependents): every DLL it reaches
# that the build folder holds (vcpkg copies them there), plus the Visual C++ runtime DLLs it
# reaches, taken from Visual Studio's redistributable folder (app-local, so no VC++ redistributable
# install is needed). License texts of the bundled libraries come from vcpkg (share\<port>\copyright)
# and from packaging\licenses. See docs\BUILDING.md.
param([string]$Dir = "release", [switch]$NoBuild, [string]$VsPath = "", [string]$VcpkgRoot = "")
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$bdir = Join-Path $root "build\$Dir"

# ---- version from CMakeLists.txt: project(watt VERSION x.y.z
$cm = Get-Content (Join-Path $root "CMakeLists.txt") -Raw
if ($cm -notmatch 'project\(watt\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)') { throw "no project VERSION in CMakeLists.txt" }
$version = $Matches[1]
$name = "watt-$version-windows-x64"

if (-not $NoBuild) {
  $bargs = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", (Join-Path $root "build.ps1"), "-Dir", $Dir, "-Target", "watt")
  if ($VsPath) { $bargs += @("-VsPath", $VsPath) }
  if ($VcpkgRoot) { $bargs += @("-VcpkgRoot", $VcpkgRoot) }
  & powershell @bargs
  if ($LASTEXITCODE) { throw "build failed" }
}
$exe = Join-Path $bdir "watt.exe"
if (-not (Test-Path $exe)) { throw "$exe not found: build first (.\build.ps1 -Target watt)" }

# ---- tools: dumpbin and the VC++ runtime folder from Visual Studio
if (-not $VsPath) {
  $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
  $VsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
}
$msvc = Get-ChildItem (Join-Path $VsPath "VC\Tools\MSVC") -Directory | Sort-Object { [version]$_.Name } | Select-Object -Last 1
$dumpbin = Join-Path $msvc.FullName "bin\Hostx64\x64\dumpbin.exe"
$crt = Get-ChildItem (Join-Path $VsPath "VC\Redist\MSVC") -Directory |
  Where-Object { $_.Name -match '^[0-9.]+$' } | Sort-Object { [version]$_.Name } | Select-Object -Last 1 |
  ForEach-Object { Get-ChildItem (Join-Path $_.FullName "x64") -Directory -Filter "Microsoft.VC*.CRT" } | Select-Object -First 1
if (-not (Test-Path $dumpbin)) { throw "dumpbin.exe not found under $VsPath" }
if (-not $crt) { throw "Visual C++ runtime folder (VC\Redist\MSVC\<ver>\x64\Microsoft.VC*.CRT) not found under $VsPath" }

# ---- dependency walk
$found = @{}   # lower-case name -> full path
$queue = New-Object System.Collections.Queue
$queue.Enqueue($exe)
while ($queue.Count) {
  $f = $queue.Dequeue()
  $deps = & $dumpbin /nologo /dependents $f | Where-Object { $_ -match '^\s+(\S+\.dll)\s*$' } | ForEach-Object { $Matches[1] }
  foreach ($d in $deps) {
    $k = $d.ToLower()
    if ($found.ContainsKey($k)) { continue }
    $local = Join-Path $bdir $d
    $runtime = Join-Path $crt.FullName $d
    if (Test-Path $local) { $found[$k] = (Resolve-Path $local).Path; $queue.Enqueue($found[$k]) }
    elseif (Test-Path $runtime) { $found[$k] = (Resolve-Path $runtime).Path; $queue.Enqueue($found[$k]) }
    else { $found[$k] = $null }  # part of Windows
  }
}
$ship = $found.Values | Where-Object { $_ } | Sort-Object

# ---- stage
$dist = Join-Path $root "dist"
$stage = Join-Path $dist $name
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force $stage | Out-Null
Copy-Item $exe $stage
foreach ($f in $ship) { Copy-Item $f $stage }
Copy-Item (Join-Path $PSScriptRoot "README.txt") $stage
Copy-Item (Join-Path $root "CHANGELOG.md") (Join-Path $stage "CHANGELOG.txt")
Copy-Item (Join-Path $root "LICENSE") (Join-Path $stage "LICENSE.txt")

# ---- third-party notices: every library linked into watt.exe or a shipped DLL
if (-not $VcpkgRoot) { $VcpkgRoot = if ($env:VCPKG_ROOT -and $env:VCPKG_ROOT -notlike "*\VC\vcpkg") { $env:VCPKG_ROOT } else { "C:\vcpkg" } }
$share = Join-Path $VcpkgRoot "installed\x64-windows\share"
$curlShare = Get-ChildItem (Join-Path $root "build") -Directory -Filter "_curl822*" | Sort-Object Name |
  Select-Object -Last 1 | ForEach-Object { Join-Path $_.FullName "installed\x64-windows-static-md-rel\share" }
$notices = [ordered]@{
  "wxWidgets" = "$share\wxwidgets\copyright"
  "gRPC" = "$share\grpc\copyright"
  "Protocol Buffers" = "$share\protobuf\copyright"
  "utf8_range" = "$share\utf8-range\copyright"
  "Abseil" = "$share\abseil\copyright"
  "c-ares" = "$share\c-ares\copyright"
  "RE2" = "$share\re2\copyright"
  "OpenSSL" = "$share\openssl\copyright"
  "libcurl" = "$curlShare\curl\copyright"
  "nghttp2" = "$curlShare\nghttp2\copyright"
  "zlib" = "$share\zlib\copyright"
  "RapidJSON" = "$share\rapidjson\copyright"
  "QuickJS-ng" = "$share\quickjs-ng\copyright"
  "libpng" = "$share\libpng\copyright"
  "libjpeg-turbo" = "$share\libjpeg-turbo\copyright"
  "LibTIFF" = "$share\tiff\copyright"
  "liblzma (XZ Utils)" = "$share\liblzma\copyright"
  "libwebp" = "$share\libwebp\copyright"
  "PCRE2" = "$PSScriptRoot\licenses\pcre2.txt"
  "NanoSVG" = "$share\nanosvg\copyright"
  "Microsoft Edge WebView2 SDK (WebView2Loader.dll)" = "$share\webview2\copyright"
  "Windows Implementation Libraries (WIL)" = "$share\wil\copyright"
}
$sb = New-Object System.Text.StringBuilder
[void]$sb.AppendLine("Web API Testing Toolkit (watt) $version - third-party notices")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("watt.exe and the DLLs next to it include the following open source software.")
[void]$sb.AppendLine("The Visual C++ runtime DLLs (msvcp140*.dll, vcruntime140*.dll) are Microsoft redistributable files.")
foreach ($k in $notices.Keys) {
  $p = $notices[$k]
  if (-not (Test-Path $p)) { throw "license text for $k not found: $p" }
  [void]$sb.AppendLine("")
  [void]$sb.AppendLine(("=" * 78))
  [void]$sb.AppendLine($k)
  [void]$sb.AppendLine(("=" * 78))
  [void]$sb.AppendLine((Get-Content $p -Raw).TrimEnd())
}
[System.IO.File]::WriteAllText((Join-Path $stage "THIRD-PARTY-NOTICES.txt"), $sb.ToString().Replace("`r`n", "`n").Replace("`n", "`r`n"))

# ---- zip + checksum
$zip = Join-Path $dist "$name.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path $stage -DestinationPath $zip -CompressionLevel Optimal
$hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
# One LF-terminated line, as sha256sum writes it, so `sha256sum -c` reads it on any system.
[System.IO.File]::WriteAllText((Join-Path $dist "$name.zip.sha256"), "$hash  $name.zip`n", [System.Text.Encoding]::ASCII)

$size = "{0:N1} MB" -f ((Get-Item $zip).Length / 1MB)
Write-Host "Packaged $($ship.Count) DLLs with watt.exe"
Write-Host "$zip ($size)"
Write-Host "SHA-256 $hash"
