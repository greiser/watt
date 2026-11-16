<#
.SYNOPSIS
  Generic cold-start / idle-RAM measurement for a Windows GUI app (used for reference apps and watt.exe).

.DESCRIPTION
  Per run:
    1. Kill every process of the app (descendants + anything whose image lives under the exe's directory
       or any -ExtraDir), wait -CooldownSec.
    2. Start the exe (t0 = immediately before Process.Start).
    3. window_visible_ms: first time a visible, uncloaked, non-minimized, unowned top-level window
       >= MinWindowW x MinWindowH belongs to the process tree (polled every 15 ms).
       NOTE: a splash screen that meets the size counts; the window title is logged so you can tell.
    4. idle_ms: after the window is visible, CPU of the whole tree is sampled every SampleMs. Idle is the
       START of the first run of consecutive samples spanning >= IdleWindowMs in which every sample is below
       IdleCpuPct percent of total machine capacity (Task Manager convention: 100% = all logical cores).
    5. Wait SettleSec more, then record idle RAM = sum of private working set over the tree (MB, 1 MB = 2^20 B),
       also private commit and full working set, and the process count.
    6. Kill the tree.
  Reports every run plus the median (of all runs; none are discarded).

.EXAMPLE
  powershell -File bench\measure_app.ps1 -Exe C:\path\watt.exe -Name watt -Runs 5 -OutJson bench\out\watt.json
#>
param(
    [Parameter(Mandatory)] [string]$Exe,
    [Parameter(Mandatory)] [string]$Name,
    [int]$Runs = 5,
    [string[]]$ArgList = @(),
    [string[]]$ExtraDir = @(),
    [double]$IdleCpuPct = 2.0,
    [int]$IdleWindowMs = 1000,
    [int]$SampleMs = 250,
    [int]$SettleSec = 10,
    [int]$TimeoutSec = 120,
    [int]$CooldownSec = 3,
    [int]$MinWindowW = 300,
    [int]$MinWindowH = 200,
    [string]$OutJson
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'BenchNative.ps1')

$Exe = (Resolve-Path $Exe).Path
$prefixes = Get-BenchDirPrefixes $Exe $ExtraDir
$cores = [Environment]::ProcessorCount

function Median([double[]]$v) {
    $v = @($v | Where-Object { $_ -ne $null -and -not [double]::IsNaN($_) } | Sort-Object)
    if ($v.Count -eq 0) { return $null }
    if ($v.Count % 2) { return $v[[int][math]::Floor($v.Count / 2)] }
    return ($v[$v.Count / 2 - 1] + $v[$v.Count / 2]) / 2
}

$results = @()
for ($run = 1; $run -le $Runs; $run++) {
    Stop-BenchApp $Exe $prefixes
    Start-Sleep -Seconds $CooldownSec

    $cache = New-Object 'System.Collections.Generic.Dictionary[int,string]'
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $psi = New-Object Diagnostics.ProcessStartInfo $Exe
    if ($ArgList.Count) { $psi.Arguments = ($ArgList -join ' ') }
    $psi.UseShellExecute = $false
    $psi.WorkingDirectory = Split-Path -Parent $Exe
    $proc = [Diagnostics.Process]::Start($psi)
    $roots = [int[]]@($proc.Id)

    # Phase 1: window visible
    $winMs = $null; $win = $null
    while ($sw.Elapsed.TotalSeconds -lt $TimeoutSec) {
        $set = [WattBench.Native]::Tree($roots, [string[]]$prefixes, $cache)
        $win = Get-BenchMainWindow $set $MinWindowW $MinWindowH
        if ($win) { $winMs = $sw.Elapsed.TotalMilliseconds; break }
        Start-Sleep -Milliseconds 15
    }

    # Phase 2: idle
    $idleMs = $null; $peakCpu = 0.0
    if ($winMs -ne $null) {
        $prev = @{}
        $set = [WattBench.Native]::Tree($roots, [string[]]$prefixes, $cache)
        foreach ($p in $set) { $prev[$p] = [WattBench.Native]::CpuTime($p) }
        $tPrev = $sw.Elapsed.TotalMilliseconds
        $quietStart = $null
        while ($sw.Elapsed.TotalSeconds -lt $TimeoutSec) {
            Start-Sleep -Milliseconds $SampleMs
            $set = [WattBench.Native]::Tree($roots, [string[]]$prefixes, $cache)
            $tNow = $sw.Elapsed.TotalMilliseconds
            $delta = 0L; $cur = @{}
            foreach ($p in $set) {
                $c = [WattBench.Native]::CpuTime($p)
                if ($c -lt 0) { continue }
                $cur[$p] = $c
                $old = if ($prev.ContainsKey($p) -and $prev[$p] -ge 0) { $prev[$p] } else { 0L }
                $delta += ($c - $old)
            }
            $pct = 100.0 * ($delta / 10000.0) / (($tNow - $tPrev) * $cores)
            if ($pct -gt $peakCpu) { $peakCpu = $pct }
            if ($pct -lt $IdleCpuPct) {
                if ($quietStart -eq $null) { $quietStart = $tPrev }
                if (($tNow - $quietStart) -ge $IdleWindowMs) { $idleMs = $quietStart; break }
            } else { $quietStart = $null }
            $prev = $cur; $tPrev = $tNow
        }
    }

    # Phase 3: settle, then RAM
    Start-Sleep -Seconds $SettleSec
    $set = [WattBench.Native]::Tree($roots, [string[]]$prefixes, $cache)
    $pws = 0L; $commit = 0L; $ws = 0L; $n = 0
    foreach ($p in $set) {
        $m = [WattBench.Native]::Mem($p)
        if ($m) { $pws += $m[0]; $commit += $m[1]; $ws += $m[2]; $n++ }
    }
    $finalWin = Get-BenchMainWindow $set $MinWindowW $MinWindowH

    $r = [pscustomobject]@{
        run                  = $run
        window_visible_ms    = if ($winMs -ne $null) { [math]::Round($winMs, 0) } else { $null }
        idle_ms              = if ($idleMs -ne $null) { [math]::Round($idleMs, 0) } else { $null }
        idle_ram_mb          = [math]::Round($pws / 1MB, 1)
        private_commit_mb    = [math]::Round($commit / 1MB, 1)
        working_set_mb       = [math]::Round($ws / 1MB, 1)
        process_count        = $set.Count
        peak_cpu_pct         = [math]::Round($peakCpu, 1)
        first_window         = if ($win) { "$($win.W)x$($win.H) '$($win.Title)' [$($win.Class)]" } else { $null }
        final_window         = if ($finalWin) { "$($finalWin.W)x$($finalWin.H) '$($finalWin.Title)' [$($finalWin.Class)]" } else { $null }
    }
    $results += $r
    Write-Host ("[{0}] run {1}: window {2} ms, idle {3} ms, RAM {4} MB (commit {5}, WS {6}), {7} procs | first: {8}" -f `
        $Name, $run, $r.window_visible_ms, $r.idle_ms, $r.idle_ram_mb, $r.private_commit_mb, $r.working_set_mb, $r.process_count, $r.first_window)

    Stop-BenchApp $Exe $prefixes
}

$summary = [pscustomobject]@{
    name                     = $Name
    exe                      = $Exe
    runs                     = $Runs
    method                   = "window=first visible top-level >= ${MinWindowW}x${MinWindowH}; idle=start of first >=${IdleWindowMs}ms span with every ${SampleMs}ms sample < ${IdleCpuPct}% of $cores logical cores (tree); RAM=sum private working set ${SettleSec}s after idle"
    median_window_visible_ms = Median ($results | ForEach-Object { $_.window_visible_ms })
    median_idle_ms           = Median ($results | ForEach-Object { $_.idle_ms })
    median_idle_ram_mb       = Median ($results | ForEach-Object { $_.idle_ram_mb })
    median_process_count     = Median ($results | ForEach-Object { $_.process_count })
    idle_timeouts            = @($results | Where-Object { $_.idle_ms -eq $null }).Count
    measured_at              = (Get-Date).ToString('s')
    per_run                  = $results
}
Write-Host ("[{0}] MEDIAN window {1} ms, idle {2} ms, RAM {3} MB, {4} procs (idle timeouts: {5})" -f `
    $Name, $summary.median_window_visible_ms, $summary.median_idle_ms, $summary.median_idle_ram_mb, $summary.median_process_count, $summary.idle_timeouts)
if ($OutJson) {
    $dir = Split-Path -Parent $OutJson; if ($dir) { New-Item -ItemType Directory -Force $dir | Out-Null }
    $summary | ConvertTo-Json -Depth 5 | Out-File -Encoding utf8 $OutJson
}
$summary
