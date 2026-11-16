# Shared native helpers for bench\measure_app.ps1 and bench\screenshot_app.ps1.
# Dot-source this file. Windows PowerShell 5.1 compatible (C# 5 syntax only).

if (-not ('WattBench.Native' -as [type])) {
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;

namespace WattBench {

public class WinInfo {
    public IntPtr Hwnd; public int Pid; public string Title; public string Class;
    public int X, Y, W, H;
    public override string ToString() { return string.Format("hwnd=0x{0:X} pid={1} {2}x{3} '{4}' [{5}]", Hwnd.ToInt64(), Pid, W, H, Title, Class); }
}

public class ProcSnap { public int Pid; public int ParentPid; public string Name; }

public static class Native {
    const uint TH32CS_SNAPPROCESS = 0x2;
    const uint PROCESS_QUERY_LIMITED_INFORMATION = 0x1000;
    const uint PROCESS_VM_READ = 0x10;
    const uint PROCESS_TERMINATE = 0x1;

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct PROCESSENTRY32W {
        public uint dwSize; public uint cntUsage; public uint th32ProcessID; public IntPtr th32DefaultHeapID;
        public uint th32ModuleID; public uint cntThreads; public uint th32ParentProcessID; public int pcPriClassBase;
        public uint dwFlags; [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 260)] public string szExeFile;
    }
    [StructLayout(LayoutKind.Sequential)]
    struct PROCESS_MEMORY_COUNTERS_EX2 {
        public uint cb; public uint PageFaultCount; public UIntPtr PeakWorkingSetSize; public UIntPtr WorkingSetSize;
        public UIntPtr QuotaPeakPagedPoolUsage; public UIntPtr QuotaPagedPoolUsage; public UIntPtr QuotaPeakNonPagedPoolUsage;
        public UIntPtr QuotaNonPagedPoolUsage; public UIntPtr PagefileUsage; public UIntPtr PeakPagefileUsage;
        public UIntPtr PrivateUsage; public UIntPtr PrivateWorkingSetSize; public ulong SharedCommitUsage;
    }
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }

    [DllImport("kernel32.dll", SetLastError = true)] static extern IntPtr CreateToolhelp32Snapshot(uint f, uint pid);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] static extern bool Process32FirstW(IntPtr h, ref PROCESSENTRY32W e);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] static extern bool Process32NextW(IntPtr h, ref PROCESSENTRY32W e);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
    [DllImport("kernel32.dll")] static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
    [DllImport("kernel32.dll")] static extern bool GetProcessTimes(IntPtr h, out long c, out long e, out long k, out long u);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] static extern bool QueryFullProcessImageNameW(IntPtr h, int flags, StringBuilder sb, ref int size);
    [DllImport("kernel32.dll")] static extern bool TerminateProcess(IntPtr h, uint code);
    [DllImport("psapi.dll")] static extern bool GetProcessMemoryInfo(IntPtr h, ref PROCESS_MEMORY_COUNTERS_EX2 c, uint cb);

    delegate bool EnumWindowsProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumWindowsProc cb, IntPtr l);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] static extern bool IsIconic(IntPtr h);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out int pid);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetWindowTextW(IntPtr h, StringBuilder sb, int n);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassNameW(IntPtr h, StringBuilder sb, int n);
    [DllImport("user32.dll")] static extern IntPtr GetWindow(IntPtr h, uint cmd);
    [DllImport("dwmapi.dll")] static extern int DwmGetWindowAttribute(IntPtr h, int attr, out int v, int cb);
    [DllImport("dwmapi.dll")] static extern int DwmGetWindowAttribute(IntPtr h, int attr, out RECT v, int cb);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);

    public static List<ProcSnap> Snapshot() {
        var list = new List<ProcSnap>();
        IntPtr h = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (h == IntPtr.Zero || h == new IntPtr(-1)) return list;
        try {
            var e = new PROCESSENTRY32W(); e.dwSize = (uint)Marshal.SizeOf(typeof(PROCESSENTRY32W));
            if (Process32FirstW(h, ref e)) {
                do { list.Add(new ProcSnap { Pid = (int)e.th32ProcessID, ParentPid = (int)e.th32ParentProcessID, Name = e.szExeFile }); }
                while (Process32NextW(h, ref e));
            }
        } finally { CloseHandle(h); }
        return list;
    }

    public static string ImagePath(int pid) {
        IntPtr h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, false, pid);
        if (h == IntPtr.Zero) return null;
        try { var sb = new StringBuilder(1024); int n = sb.Capacity; return QueryFullProcessImageNameW(h, 0, sb, ref n) ? sb.ToString() : null; }
        finally { CloseHandle(h); }
    }

    // Process tree = descendants of any root pid, plus any process whose image lives under one of dirPrefixes.
    // Path-based membership catches apps whose launcher stub exits and re-parents (Squirrel stubs, single-instance handoff).
    public static HashSet<int> Tree(IEnumerable<int> roots, string[] dirPrefixes, Dictionary<int,string> pathCache) {
        var snap = Snapshot();
        var set = new HashSet<int>();
        foreach (int r in roots) if (r > 0) set.Add(r);
        var alive = new HashSet<int>(); foreach (var p in snap) alive.Add(p.Pid);
        set.IntersectWith(alive);
        foreach (var p in snap) {
            if (set.Contains(p.Pid) || p.Pid <= 4) continue;
            string path;
            if (!pathCache.TryGetValue(p.Pid, out path)) { path = ImagePath(p.Pid); pathCache[p.Pid] = path; }
            if (path == null) continue;
            foreach (var d in dirPrefixes) if (path.StartsWith(d, StringComparison.OrdinalIgnoreCase)) { set.Add(p.Pid); break; }
        }
        bool grew = true;
        while (grew) { grew = false; foreach (var p in snap) if (!set.Contains(p.Pid) && set.Contains(p.ParentPid) && p.Pid != p.ParentPid) { set.Add(p.Pid); grew = true; } }
        return set;
    }

    // Total CPU time (100ns units) of a pid, -1 if inaccessible/exited.
    public static long CpuTime(int pid) {
        IntPtr h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, false, pid);
        if (h == IntPtr.Zero) return -1;
        try { long c, e, k, u; return GetProcessTimes(h, out c, out e, out k, out u) ? k + u : -1; }
        finally { CloseHandle(h); }
    }

    // [privateWorkingSet, privateCommit, workingSet] in bytes.
    public static long[] Mem(int pid) {
        IntPtr h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, false, pid);
        if (h == IntPtr.Zero) h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, false, pid);
        if (h == IntPtr.Zero) return null;
        try {
            var c = new PROCESS_MEMORY_COUNTERS_EX2(); c.cb = (uint)Marshal.SizeOf(typeof(PROCESS_MEMORY_COUNTERS_EX2));
            if (!GetProcessMemoryInfo(h, ref c, c.cb)) return null;
            return new long[] { (long)c.PrivateWorkingSetSize.ToUInt64(), (long)c.PrivateUsage.ToUInt64(), (long)c.WorkingSetSize.ToUInt64() };
        } finally { CloseHandle(h); }
    }

    public static void Kill(IEnumerable<int> pids) {
        foreach (int pid in pids) {
            IntPtr h = OpenProcess(PROCESS_TERMINATE, false, pid);
            if (h != IntPtr.Zero) { TerminateProcess(h, 1); CloseHandle(h); }
        }
    }

    // Visible, uncloaked, non-minimized, unowned top-level windows of the given pids with at least minW x minH.
    public static List<WinInfo> Windows(HashSet<int> pids, int minW, int minH) {
        var res = new List<WinInfo>();
        EnumWindows(delegate(IntPtr h, IntPtr l) {
            int pid; GetWindowThreadProcessId(h, out pid);
            if (!pids.Contains(pid) || !IsWindowVisible(h) || IsIconic(h)) return true;
            if (GetWindow(h, 4 /*GW_OWNER*/) != IntPtr.Zero) return true;
            int cloaked = 0; DwmGetWindowAttribute(h, 14 /*DWMWA_CLOAKED*/, out cloaked, 4);
            if (cloaked != 0) return true;
            RECT r; if (!GetWindowRect(h, out r)) return true;
            int w = r.Right - r.Left, hh = r.Bottom - r.Top;
            if (w < minW || hh < minH) return true;
            var t = new StringBuilder(512); GetWindowTextW(h, t, 512);
            var c = new StringBuilder(256); GetClassNameW(h, c, 256);
            res.Add(new WinInfo { Hwnd = h, Pid = pid, Title = t.ToString(), Class = c.ToString(), X = r.Left, Y = r.Top, W = w, H = hh });
            return true;
        }, IntPtr.Zero);
        return res;
    }

    // Visible frame bounds (excludes the invisible resize border on Win10/11).
    public static RECT FrameRect(IntPtr h) {
        RECT r;
        if (DwmGetWindowAttribute(h, 9 /*DWMWA_EXTENDED_FRAME_BOUNDS*/, out r, Marshal.SizeOf(typeof(RECT))) != 0) GetWindowRect(h, out r);
        return r;
    }

    public static void Key(byte vk, bool up) { keybd_event(vk, 0, up ? 2u : 0u, UIntPtr.Zero); }
    [DllImport("user32.dll")] static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] static extern void mouse_event(uint f, int dx, int dy, uint data, UIntPtr extra);
    public static void Click(int x, int y) { SetCursorPos(x, y); System.Threading.Thread.Sleep(80); mouse_event(2, 0, 0, 0, UIntPtr.Zero); mouse_event(4, 0, 0, 0, UIntPtr.Zero); }
}
}
'@
}
[void][WattBench.Native]::SetProcessDPIAware()

function Get-BenchDirPrefixes([string]$Exe, [string[]]$Extra) {
    # Every process whose image lives under one of these dirs is treated as part of the app (and gets killed),
    # so refuse shared/system directories outright.
    $d = (Split-Path -Parent (Resolve-Path $Exe).Path).TrimEnd('\') + '\'
    $all = @($d) + @($Extra | Where-Object { $_ } | ForEach-Object { $_.TrimEnd('\') + '\' })
    $forbidden = @($env:windir, $env:ProgramFiles, ${env:ProgramFiles(x86)}, $env:LOCALAPPDATA, $env:APPDATA,
                   "$env:LOCALAPPDATA\Programs", $env:USERPROFILE, $env:SystemDrive) |
                 Where-Object { $_ } | ForEach-Object { $_.TrimEnd('\') + '\' }
    foreach ($p in $all) {
        if ($p.StartsWith(($env:windir.TrimEnd('\') + '\'), 'OrdinalIgnoreCase') -or ($forbidden -contains $p) -or $p.Length -le 4) {
            throw "Refusing to treat shared directory '$p' as an app directory (would match unrelated processes). Put the app in its own directory."
        }
    }
    $all
}

function Stop-BenchApp([string]$Exe, [string[]]$DirPrefixes) {
    $cache = New-Object 'System.Collections.Generic.Dictionary[int,string]'
    for ($i = 0; $i -lt 20; $i++) {
        $set = [WattBench.Native]::Tree([int[]]@(), [string[]]$DirPrefixes, $cache)
        if ($set.Count -eq 0) { return }
        [WattBench.Native]::Kill($set)
        Start-Sleep -Milliseconds 300
        $cache.Clear()
    }
    Write-Warning "Could not kill all processes under $($DirPrefixes -join ', ')"
}

function Get-BenchMainWindow([System.Collections.Generic.HashSet[int]]$Pids, [int]$MinW = 300, [int]$MinH = 200) {
    $w = [WattBench.Native]::Windows($Pids, $MinW, $MinH)
    $w | Sort-Object { $_.W * $_.H } -Descending | Select-Object -First 1
}

