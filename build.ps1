# Configures and builds the Web API Testing Toolkit (watt) with MSVC + Ninja + vcpkg.
#
#   .\build.ps1                          Release build of everything into build\release
#   .\build.ps1 -Target watt             only the application
#   .\build.ps1 -Config Debug            Debug build into build\debug
#   .\build.ps1 -Dir mine                a separate build tree (build\mine), e.g. for parallel builds
#
# Visual Studio is found with vswhere (override with -VsPath); vcpkg is $env:VCPKG_ROOT, else
# C:\vcpkg (override with -VcpkgRoot). See docs\BUILDING.md.
param(
  [ValidateSet("Release", "Debug")][string]$Config = "Release",
  [string]$Target = "all",
  [string]$Dir = "",
  [string]$VsPath = "",
  [string]$VcpkgRoot = ""
)
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot

# ---- vcpkg. Resolved before the VS developer shell starts: that shell points VCPKG_ROOT at
# Visual Studio's own bundled vcpkg, which has none of watt's packages and is never used.
if (-not $VcpkgRoot) { $VcpkgRoot = if ($env:VCPKG_ROOT) { $env:VCPKG_ROOT } else { "C:\vcpkg" } }
if ($VcpkgRoot -like "*\Microsoft Visual Studio\*" -or $VcpkgRoot -like "*\VC\vcpkg") { $VcpkgRoot = "C:\vcpkg" }
$vcpkg = Join-Path $VcpkgRoot "vcpkg.exe"
if (-not (Test-Path $vcpkg)) { throw "vcpkg not found at ${VcpkgRoot}: set VCPKG_ROOT or pass -VcpkgRoot" }
$toolchain = (Join-Path $VcpkgRoot "scripts\buildsystems\vcpkg.cmake").Replace('\', '/')

# ---- Visual Studio developer environment (x64)
if (-not $VsPath) {
  $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
  if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found: install Visual Studio 2022 or later, or pass -VsPath" }
  $VsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
  if (-not $VsPath) { throw "no Visual Studio with the C++ x64 tools found (workload: Desktop development with C++)" }
}
# VsDevCmd looks for vswhere on the PATH too (and prints an error when it is not there).
$installer = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer"
if (Test-Path $installer) { $env:PATH = "$installer;$env:PATH" }
Import-Module (Join-Path $VsPath "Common7\Tools\Microsoft.VisualStudio.DevShell.dll")
Enter-VsDevShell -VsInstallPath $VsPath -SkipAutomaticLocation -DevCmdArguments "-arch=x64 -host_arch=x64" | Out-Null
$env:VCPKG_ROOT = $VcpkgRoot

$name = if ($Dir) { $Dir } else { $Config.ToLower() }
$bdir = Join-Path $root "build\$name"

# watt pins its own libcurl (vcpkg\ports\curl overlay: 8.22, static, release-only, Schannel +
# nghttp2; see CMakeLists.txt). It is installed once into build\_curl822r<port-version>, shared by
# every build tree, and installed again when the overlay's version changes. A new port-version
# installs into its own folder, so trees configured against the previous one keep working;
# reconfigure a tree (delete its build.ninja) to pick it up.
$overlay = Join-Path $root "vcpkg"
$curlTriplet = "x64-windows-static-md-rel"
$curlPort = Get-Content (Join-Path $overlay "ports\curl\vcpkg.json") -Raw | ConvertFrom-Json
$curlBase = Join-Path $root ("build\_curl822" + $(if ($curlPort."port-version") { "r$($curlPort.'port-version')" } else { "" }))
$curlPrefix = Join-Path $curlBase "installed\$curlTriplet"
function Install-PinnedCurl {
  $ver = Join-Path $curlPrefix "include\curl\curlver.h"
  if ((Test-Path "$curlPrefix\lib\libcurl.lib") -and (Test-Path $ver) -and
      (Select-String -Path $ver -SimpleMatch "#define LIBCURL_VERSION `"$($curlPort.version)`"" -Quiet)) { return }
  if (Test-Path "$curlBase\installed") { Remove-Item -Recurse -Force "$curlBase\installed" }
  & $vcpkg install "curl[core,http2,ssl,sspi,non-http]" --classic --triplet $curlTriplet `
    "--overlay-ports=$overlay\ports" "--overlay-triplets=$overlay\triplets" "--x-install-root=$curlBase\installed" `
    "--x-buildtrees-root=$curlBase\bt" "--x-packages-root=$curlBase\pk"
  if ($LASTEXITCODE) { exit $LASTEXITCODE }
}

if (-not (Test-Path "$bdir\build.ninja")) {
  # The pinned libcurl is release-only (/MD); a Debug tree uses vcpkg's own libcurl.
  $curlArg = "-DWATT_CURL_ROOT="
  if ($Config -eq "Release") {
    Install-PinnedCurl
    $curlArg = "-DWATT_CURL_ROOT=$($curlPrefix.Replace('\', '/'))"
  }
  cmake -S $root -B $bdir -G Ninja "-DCMAKE_BUILD_TYPE=$Config" `
    "-DCMAKE_TOOLCHAIN_FILE=$toolchain" -DVCPKG_TARGET_TRIPLET=x64-windows $curlArg
  if ($LASTEXITCODE) { exit $LASTEXITCODE }
}
cmake --build $bdir --target $Target
exit $LASTEXITCODE
