# Configure + build watt with MSVC/Ninja from the VS install. Usage: .\build.ps1 [-Config Release|Debug] [-Target watt]
# -Dir lets parallel agents use separate build trees (build\<Dir>) so concurrent ninja runs never collide.
param([string]$Config = "Release", [string]$Target = "all", [string]$Dir = "")
$ErrorActionPreference = "Stop"
$vs = "C:\dev\tools\VisualStudio\community"
Import-Module "$vs\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments "-arch=x64 -host_arch=x64" | Out-Null
$root = $PSScriptRoot
$name = if ($Dir) { $Dir } else { $Config.ToLower() }
$bdir = Join-Path $root "build\$name"
# watt pins its own libcurl (ports\curl overlay, static release build; see CMakeLists.txt). It
# is installed once into build\_curl822, shared by every build dir, and re-installed when the
# overlay port's version changes (about a minute, or a binary-cache hit).
$curlTriplet = "x64-windows-static-md-rel"
$curlBase = Join-Path $root "build\_curl822"
$curlPrefix = Join-Path $curlBase "installed\$curlTriplet"
function Install-PinnedCurl {
  $want = (Get-Content (Join-Path $root "ports\curl\vcpkg.json") -Raw | ConvertFrom-Json).version
  $ver = Join-Path $curlPrefix "include\curl\curlver.h"
  if ((Test-Path "$curlPrefix\lib\libcurl.lib") -and (Test-Path $ver) -and
      (Select-String -Path $ver -SimpleMatch "#define LIBCURL_VERSION `"$want`"" -Quiet)) { return }
  if (Test-Path "$curlBase\installed") { Remove-Item -Recurse -Force "$curlBase\installed" }
  & C:\vcpkg\vcpkg.exe install "curl[core,http2,ssl,sspi,non-http]" --classic --triplet $curlTriplet `
    "--overlay-ports=$root\ports" "--overlay-triplets=$root\triplets" "--x-install-root=$curlBase\installed" `
    "--x-buildtrees-root=$curlBase\bt" "--x-packages-root=$curlBase\pk"
  if ($LASTEXITCODE) { exit $LASTEXITCODE }
}
if (-not (Test-Path "$bdir\build.ninja")) {
  # The pinned build is release-only (/MD); a Debug tree keeps vcpkg's own libcurl.
  $curlArg = "-DWATT_CURL_ROOT="
  if ($Config -eq "Release") {
    Install-PinnedCurl
    $curlArg = "-DWATT_CURL_ROOT=$($curlPrefix.Replace('\', '/'))"
  }
  cmake -S $root -B $bdir -G Ninja "-DCMAKE_BUILD_TYPE=$Config" `
    -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows `
    $curlArg
  if ($LASTEXITCODE) { exit $LASTEXITCODE }
}
cmake --build $bdir --target $Target
exit $LASTEXITCODE

