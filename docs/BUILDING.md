# Building the Web API Testing Toolkit (watt) on Windows

watt is a Windows-only application (x64). It is built with MSVC, CMake and Ninja, and its
libraries come from vcpkg. A helper script, `build.ps1`, sets up the Visual Studio environment,
configures CMake and builds.

The whole procedure, once the tools are installed:

```powershell
git clone https://github.com/microsoft/vcpkg C:\vcpkg
C:\vcpkg\bootstrap-vcpkg.bat -disableMetrics
C:\vcpkg\vcpkg install --classic --triplet x64-windows "grpc[codegen]" protobuf rapidjson re2 quickjs-ng "wxwidgets[webview]" catch2 "curl[http2,ssl,sspi,non-http]" openssl zlib
cd <your clone of watt>
.\build.ps1                      # -> build\release\watt.exe
.\test.ps1                       # all test suites
.\packaging\package.ps1          # -> dist\watt-1.0.0-windows-x64.zip
```

The rest of this guide explains each step, the options, and what to do when something fails.

## 1. Requirements

| Tool | Version | Notes |
|---|---|---|
| Windows | 10 or 11, 64-bit | Built and tested on Windows 11. |
| Visual Studio | 2026 (tested with 18.9, MSVC 14.51) | Workload **Desktop development with C++**. It includes the MSVC x64 compiler, a Windows SDK, and **C++ CMake tools for Windows**, which bring CMake and Ninja. The free Community edition is enough. Visual Studio 2022 has not been tested. |
| CMake | 3.25 or later | The copy that comes with Visual Studio is used; no separate install is needed. |
| Ninja | any recent | Comes with Visual Studio's CMake tools. |
| Git for Windows | any recent | vcpkg needs git. Its `openssl` is used only to regenerate the test certificates. |
| vcpkg | tested at commit `44819aa2a6c10e56065e2b0330e7d6c89d1d2574` (2026-06-13) | Classic mode, triplet `x64-windows`. |
| PowerShell | 5.1 (built into Windows) or 7 | Runs the build, test and packaging scripts. |
| Python 3 with Pillow | optional | Only to redraw the application icon. |

Building the vcpkg libraries the first time takes a long time (gRPC, protobuf, Abseil and
wxWidgets are compiled from source: expect an hour or more) and needs several GB of disk space.
Later builds of watt itself take a few minutes from scratch and seconds when little has changed.

## 2. Install Visual Studio

Install Visual Studio 2026 with the **Desktop development with C++** workload. Keep the default
components of that workload; they include **C++ CMake tools for Windows** and a Windows SDK.

`build.ps1` finds Visual Studio with `vswhere.exe` (installed by the Visual Studio Installer), so
no Developer Command Prompt is needed. To use a specific installation, pass its folder, for
example `-VsPath "D:\VisualStudio\Community"`.

## 3. Set up vcpkg and the libraries

Use a short path for vcpkg, such as `C:\vcpkg`; long paths make some library builds fail.

```powershell
git clone https://github.com/microsoft/vcpkg C:\vcpkg
cd C:\vcpkg
git checkout 44819aa2a6c10e56065e2b0330e7d6c89d1d2574   # optional: the tested version
.\bootstrap-vcpkg.bat -disableMetrics
.\vcpkg install --classic --triplet x64-windows "grpc[codegen]" protobuf rapidjson re2 quickjs-ng "wxwidgets[webview]" catch2 "curl[http2,ssl,sspi,non-http]" openssl zlib
```

What each library is for:

| Package | Used by |
|---|---|
| `grpc[codegen]`, `protobuf` | gRPC calls and `.proto` parsing; `codegen` builds the test server's generated code |
| `wxwidgets[webview]` | The user interface; `webview` adds the Edge WebView2 HTML preview |
| `quickjs-ng` | Pre-request and post-response scripts |
| `re2`, `rapidjson` | Assertions and JSON handling |
| `curl[http2,ssl,sspi,non-http]` | HTTP in Debug builds (Release builds use the pinned libcurl, below) |
| `openssl` | TLS for the gRPC-Web test server |
| `zlib` | Compression, shared by gRPC and libcurl |
| `catch2` | The test suites |

If vcpkg is not at `C:\vcpkg`, set the `VCPKG_ROOT` environment variable to its folder (all the
scripts use it), or pass `-VcpkgRoot <folder>` to `build.ps1` and `packaging\package.ps1`. Note that the Visual Studio developer shell sets
`VCPKG_ROOT` to Visual Studio's own bundled vcpkg; `build.ps1` ignores that one because it has
none of these libraries.

### The pinned libcurl

Release builds do not use the vcpkg-wide libcurl. They use libcurl 8.22 built statically with
Schannel (Windows TLS), HTTP/2 and NTLM / Negotiate, from the overlay port in `vcpkg\ports\curl`
and the triplet `vcpkg\triplets\x64-windows-static-md-rel.cmake`. This version avoids a check that
costs time on every reused connection. `build.ps1` installs it automatically the first time a
Release tree is configured, into `build\_curl822r1`, and shares it between build trees. It takes
a few minutes and needs nothing from you.

## 4. Build

From the root of the source tree:

```powershell
.\build.ps1
```

This configures `build\release` (first run only) and builds every target. The application is
`build\release\watt.exe`. The vcpkg toolchain copies the DLLs it needs next to it, so it runs
straight from there.

`build.ps1` parameters:

| Parameter | Default | Meaning |
|---|---|---|
| `-Config Release\|Debug` | `Release` | Build type. Debug uses vcpkg's libcurl. |
| `-Target <name>` | `all` | One CMake target, e.g. `watt` for only the application. |
| `-Dir <name>` | the config name | Build folder `build\<name>`; separate folders let builds run side by side. |
| `-VsPath <folder>` | found by vswhere | Visual Studio installation to use. |
| `-VcpkgRoot <folder>` | `VCPKG_ROOT`, else `C:\vcpkg` | vcpkg installation to use. |

Targets:

| Target | Output | What it is |
|---|---|---|
| `watt` | `watt.exe` | The application (also the `watt --run` command-line runner) |
| `watt_core` | libraries | The engine without the GUI: `watt_http`, `watt_grpc`, `watt_model` |
| `watt_test_<piece>` | `watt_test_<piece>.exe` | One Catch2 suite per `tests\test_<piece>.cpp` |
| `watt_testserver` | `watt_testserver.exe` | Local gRPC (port 50051), gRPC-Web (50052) and TLS server used by the tests |
| `watt_httpserver` | `watt_httpserver.exe` | Local HTTP echo server for trying requests by hand (port 18080) |
| `watt_bench` | `watt_bench.exe` | Headless benchmarks |

`build.ps1` configures a build folder only once. After changing the toolchain, the vcpkg location
or `CMakeLists.txt` options, delete the build folder (or its `build.ninja`) so the next run
configures it again.

### Building without build.ps1

From an **x64 Native Tools Command Prompt for VS** (it puts CMake, Ninja and MSVC on the path):

```bat
cmake -S . -B build\release -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows ^
  -DWATT_CURL_ROOT=<source>/build/_curl822r1/installed/x64-windows-static-md-rel
cmake --build build\release
```

Leave out `-DWATT_CURL_ROOT` to build with vcpkg's libcurl instead of the pinned one; everything
works, but reused connections are slightly slower. `WATT_CURL_ROOT` must point at an install made
by `build.ps1` (or by the same `vcpkg install` command it runs).

To work in the Visual Studio IDE, use **File > Open > Folder** on the source tree, and set the
same CMake variables (toolchain file, triplet, and optionally `WATT_CURL_ROOT`) in the CMake
settings.

## 5. Run the tests

```powershell
.\test.ps1                 # build and run all suites
.\test.ps1 -NoBuild        # run what is already built
.\test.ps1 -Suite http     # one suite: watt_test_http
```

`test.ps1` builds the suites and `watt_testserver`, starts the server in the background, runs
every `watt_test_*.exe` and stops the server. It prints `All 17 suites passed.` and exits with 0
on success, or names the failing suites and exits with 1. The suites talk to local servers on
the loopback interface (the test server listens on 50051 and 50052). A few HTTP tests, tagged
`[net]`, also make a request to `https://www.google.com/generate_204` to check HTTP/2 and TLS
against a real server; without internet access they are skipped, not failed. To leave them out,
run that suite with `"~[net]"`.

A single suite can also be run directly, with Catch2's options, for example
`build\release\watt_test_model.exe "[workspace]"`.

## 6. Make the distribution zip

```powershell
.\packaging\package.ps1
```

It builds `watt.exe` in Release, then creates:

- `dist\watt-<version>-windows-x64\` with `watt.exe`, the DLLs it needs, `README.txt`,
  `CHANGELOG.txt` and `THIRD-PARTY-NOTICES.txt`;
- `dist\watt-<version>-windows-x64.zip`, the same folder zipped;
- `dist\watt-<version>-windows-x64.zip.sha256`, its SHA-256 checksum.

The DLL list is worked out from `watt.exe` itself (`dumpbin /dependents`, followed recursively):
the libraries vcpkg copied into the build folder, plus the Visual C++ runtime DLLs from Visual
Studio's redistributable folder. The zip therefore runs on a clean Windows 10 or 11 machine
without installing the Visual C++ Redistributable. The licence texts come from vcpkg
(`installed\x64-windows\share\<port>\copyright`) and from `packaging\licenses`.

`-NoBuild` packages what `build\release` already contains.

## 7. Releasing a new version

1. Change `project(watt VERSION x.y.z ...)` in `CMakeLists.txt`. The version reaches the
   User-Agent header, the executable's file properties and the zip's name from there.
2. Add the release to `CHANGELOG.md`, and update the version in `README.md` and
   `packaging\README.txt`.
3. Delete `build\release` so it is configured with the new version, then run `.\test.ps1` and
   `.\packaging\package.ps1`.

## 8. Regenerating checked-in assets

- **Application icon**: `python resources\icon\make_icon.py` redraws `watt.ico` (16 to 256 px)
  and the PNGs in `resources\icon`. It needs Pillow (`pip install pillow`). The icon is compiled
  into `watt.exe` through `src\app\watt.rc`.
- **Test certificates**: `sh tools/testserver/certs/make_certs.sh` (from Git Bash) creates a new
  test CA, server and client certificate. `tests\test_http.cpp` checks the server certificate's
  dates, serial number and fingerprint, so update those expectations after regenerating.

## 9. Troubleshooting

| Problem | Fix |
|---|---|
| `Could NOT find ZLIB` (or gRPC, protobuf, wxWidgets) while configuring | CMake is using a vcpkg without the libraries, usually Visual Studio's bundled one. Check `-VcpkgRoot` / `VCPKG_ROOT`, delete the build folder and run `build.ps1` again. |
| `vswhere.exe not found` or `no Visual Studio with the C++ x64 tools found` | Install the **Desktop development with C++** workload, or pass `-VsPath`. |
| vcpkg build fails with path or file-name errors | Move vcpkg to a short path such as `C:\vcpkg`. |
| A change to the vcpkg location or CMake options has no effect | The build folder keeps its first configuration: delete it, or its `build.ninja`. |
| `watt.exe` reports a missing DLL when started from a folder of your own | Copy every DLL from `build\release` with it, or use `packaging\package.ps1`, which collects exactly the ones needed. |
| Tests fail to connect to `127.0.0.1:50051` | Another program holds port 50051 or 50052, or `watt_testserver.exe` was not built. Run the suites through `test.ps1`. |
| Antivirus slows or blocks the build | Exclude the source tree's `build` folder and the vcpkg folder from real-time scanning. |

## Repository layout

| Path | Contents |
|---|---|
| `CMakeLists.txt` | The build: targets, version, compiler options |
| `build.ps1`, `test.ps1` | Build and test helpers described above |
| `src\core` | The engine, without the GUI: HTTP (libcurl), gRPC, WebSocket, workspace model, scripts, assertions |
| `src\app` | The wxWidgets application, plus `watt.rc` (icon and version resource) |
| `tests` | Catch2 suites, one executable per `test_<piece>.cpp` |
| `tools\testserver` | The test server and its test certificates |
| `tools\httpserver` | The HTTP echo server |
| `bench` | Benchmarks and startup measurement scripts |
| `resources\icon` | Application icon and the script that draws it |
| `packaging` | `package.ps1`, the README for the zip, extra licence texts |
| `vcpkg` | Overlay port and triplet for the pinned libcurl |
| `docs` | This guide; `docs\dev` holds development notes |
