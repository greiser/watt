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
if (-not (Test-Path "$bdir\build.ninja")) {
  cmake -S $root -B $bdir -G Ninja "-DCMAKE_BUILD_TYPE=$Config" `
    -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows
  if ($LASTEXITCODE) { exit $LASTEXITCODE }
}
cmake --build $bdir --target $Target
exit $LASTEXITCODE

