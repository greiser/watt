Web API Testing Toolkit (watt) 1.0.0 for Windows
===============================================

A fast, native tool for calling and testing REST, gRPC, WebSocket and GraphQL APIs.
Every response shows its elapsed time, broken down by phase.


Running
-------

Run watt.exe. No installation is needed: the folder can live anywhere, for example
C:\Tools\watt or a USB stick. Keep every file in this folder together; watt.exe needs the DLLs
next to it.

The first start opens a workspace in Documents\watt; open or create others from the sidebar's
workspace menu, or pass a folder on the command line. A workspace is an ordinary folder of JSON
files, so you can keep it in git.

Requirements: Windows 10 or Windows 11, 64-bit. HTML response previews use the Microsoft Edge
WebView2 runtime (part of Windows 11 and current Windows 10); without it, watt shows a simpler
HTML view.


Command line
------------

  watt.exe [<workspace folder>] [--new-instance]

  watt.exe --run <folder or .req.json> [--workspace <dir>] [--env <name>]
           [--iterations <n>] [--data <file.csv|file.json>] [--parallel <n>]
           [--duration <s>] [--bail] [--trust-scripts]
           [--reporter-junit <file>] [--reporter-json <file>] [--reporter-html <file>]

--run runs a folder of saved requests without the window, for CI. The exit code is 0 when
everything passed, 1 when a request or assertion failed and 2 for a usage error. Run
"watt.exe --run" with no folder to list every option.


Where watt keeps things
-----------------------

  <workspace>\watt.json             workspace settings (shared, safe to commit)
  <workspace>\environments\         environments (shared; secret values are not stored here)
  <workspace>\.watt\                your history, secret values, cookies and session
                                    (personal: watt adds it to .gitignore)
  %APPDATA%\watt\                   approvals you gave per workspace (file access, environment
                                    variables, scripts, network settings)

Secret values are encrypted for your Windows account and cannot be read by another user or on
another machine.


Uninstalling
------------

Delete this folder. To remove your settings too, delete %APPDATA%\watt and the registry key
HKEY_CURRENT_USER\Software\watt.


Licenses
--------

watt is released under the MIT License (LICENSE.txt). THIRD-PARTY-NOTICES.txt lists the open
source libraries included in watt and their licenses.
