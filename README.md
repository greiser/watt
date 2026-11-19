<p align="center"><img src="resources/icon/watt-256.png" width="128" alt=""></p>

<h1 align="center">Web API Testing Toolkit (watt)</h1>

<p align="center">A fast, native Windows tool for calling and testing REST, gRPC, WebSocket and GraphQL APIs.<br>
Version 1.0.0</p>

watt is a desktop API client for engineers, in the space of Postman, Insomnia, Bruno and BloomRPC.
It is a single native C++ / wxWidgets process: the window is up in about 0.2 s, it idles in about
13 MB of memory, and every response shows how long it took, broken down by phase.

## Features

**Requests**
- HTTP/1.1 and HTTP/2 REST requests: params, headers, bodies (JSON, form, multipart, files, raw),
  cookies, redirects, proxies, client certificates and custom CA certificates.
- gRPC: unary, server, client and bidirectional streaming. Schemas come from server reflection or
  from `.proto` files, and the standard `google/protobuf` types are built in. Requests start from a
  generated template, and rich error details are decoded. gRPC-Web is supported too.
- WebSocket and Server-Sent Events, with messages shown as they arrive.
- GraphQL queries with schema-aware editing.

**Timing**
- Every response shows its total time and a waterfall: DNS, connect, TLS, request sent, waiting
  for the server and download. It also shows whether the connection was reused, the TLS version
  and the certificate chain.

**Organising and automating**
- Workspaces are plain folders of JSON files, so they work with git. Requests inherit headers,
  auth, variables, assertions and scripts from their folder and collection.
- Environments, with secret values encrypted for your Windows account.
- Auth: Basic, Bearer, Digest, NTLM, Negotiate (Kerberos), AWS Signature v4 and OAuth 2.0.
- Assertions and captures without code. Pre-request and post-response scripts use the Postman
  (`pm.*`) and Bruno (`bru.*`) APIs.
- Collection runner with data files, iterations, parallel load profiles and p50 to p99 latency.
  It also runs from the command line for CI (`watt --run`), with JUnit, JSON and HTML reports.
- Mock server that answers with a folder's saved example responses.
- History of every request sent, with its full response.

**Moving in and out**
- Import from Postman collections, OpenAPI specs and curl commands.
- Copy any request as code in 13 forms, including curl, PowerShell, Python, JavaScript, Go and C#.

**Safety**
- A workspace you clone cannot quietly read your files or environment variables, send your
  credentials elsewhere, or run scripts. Each of these needs your approval once, stored with your
  Windows account rather than in the workspace.

## Install

Download `watt-1.0.0-windows-x64.zip`, unzip it anywhere and run `watt.exe`. Nothing else needs
installing. Requirements: Windows 10 or 11, 64-bit. HTML previews use the Microsoft Edge WebView2
runtime, which is included in Windows 11 and current Windows 10; without it watt shows a simpler
HTML view.

## Command line

```
watt.exe [<workspace folder>] [--new-instance]
watt.exe --run <folder or .req.json> [--workspace <dir>] [--env <name>] [--iterations <n>]
         [--data <file.csv|file.json>] [--parallel <n>] [--duration <s>] [--bail]
         [--reporter-junit <file>] [--reporter-json <file>] [--reporter-html <file>]
```

`--run` exits with 0 when every request and assertion passed, 1 when something failed and 2 for
a usage error. Run `watt.exe --run` with no folder to see every option.

## Building from source

Windows only. See [docs/BUILDING.md](docs/BUILDING.md). In short, with Visual Studio 2026 and
vcpkg set up:

```powershell
.\build.ps1                    # build\release\watt.exe
.\test.ps1                     # all test suites
.\packaging\package.ps1        # dist\watt-1.0.0-windows-x64.zip
```

## Repository layout

| Path | Contents |
|---|---|
| `src/core` | Engine, no GUI: HTTP (libcurl), gRPC, WebSocket, workspace model, scripts, assertions |
| `src/app` | The wxWidgets application |
| `tests` | Catch2 test suites, one executable per `test_<piece>.cpp` |
| `tools/testserver` | Local gRPC, gRPC-Web and HTTPS server used by the tests |
| `tools/httpserver` | Local HTTP echo server for manual testing |
| `bench` | Headless benchmarks and startup measurement scripts |
| `resources/icon` | Application icon and the script that draws it |
| `packaging` | Windows distribution script, the end-user README and extra licence texts |
| `vcpkg` | vcpkg overlay port and triplet for the pinned libcurl build |
| `docs` | Build guide |

## Licence

Third-party components and their licences are listed in `THIRD-PARTY-NOTICES.txt` inside the
distribution zip.
