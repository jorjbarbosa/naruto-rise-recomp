<#
.SYNOPSIS
    Compares the D3D12 and Vulkan renderers on a fixed set of recorded frames.

.DESCRIPTION
    Without -Record: replays every recording in build/compare/recordings/<scene>/ on D3D12 and on
    Vulkan (ROAN_REPLAY, src/gpu/replay.h), diffs the front buffer and every color resolve of each
    frame, and writes build/compare/report.md. Exits with 1 when an image differs past the
    tolerance, an image is missing on one side or a replay fails. Rerun it after each renderer change.

    With -Record: plays the game to each scene with keys scripted by frame (ROAN_SCRIPT) and records
    its frames (ROAN_RECORD_DIR, ROAN_RECORD_FRAMES, src/gpu/guest/compare.h) on -RecordApi. A
    recording holds the uploads live at the frame's start and the frame's operations, so it replays
    the same on any later build of the same layout; record again when the renderer's operations
    change (the replay refuses a recording of another layout). The village scene loads the first save
    of Load Game: put the main checkout's Player Save 2 in build/runtime/data/user first.

    A pixel differs when a channel differs by more than -Threshold; an image fails when more than
    -MaxShare of its pixels differ. The default threshold keeps texture filtering precision
    (1-2 levels on surfaces seen at grazing angles) out of the failures.

.EXAMPLE
    ./scripts/compare-gpu.ps1 -Record              # once, or after the operations change
    ./scripts/compare-gpu.ps1                      # replay and report
    ./scripts/compare-gpu.ps1 -Scenes fight -Validation
#>
param(
    [switch]$Record,
    [string[]]$Scenes = @('title', 'fight', 'village'),
    [ValidateSet('d3d12', 'vulkan')]
    [string]$RecordApi = 'd3d12',
    [switch]$Validation,      # replays with ROAN_GPU_DEBUG=1
    [int]$Threshold = 8,
    [double]$MaxShare = 0.001,
    [int]$TimeoutSeconds = 240
)

. "$PSScriptRoot/common.ps1"

$exe = Join-Path $RepoRoot 'build/runtime/roan-xenon.exe'
$compareDir = Join-Path $RepoRoot 'build/compare'
$recordingsDir = Join-Path $compareDir 'recordings'
if (-not (Test-Path $exe)) { Fail 'build/runtime/roan-xenon.exe is missing; run scripts/build.ps1 first' }

# The scenes: keys by frame from the start, and the frames recorded once there.
$sceneSet = @{
    title   = @{ Script = ''; Frames = '1200,1500' }
    fight   = @{ Script = 'Return@750,S@840,Space@900,Space@960,Space@1020,Space@1500,Space@1560'; Frames = '1800,2100' }
    village = @{ Script = 'Return@750,Space@840,S@930,Space@1020,Space@1140,Space@1260,Space@1380'; Frames = '2600,2900' }
}

# Runs the executable with the given environment and waits for it; returns its exit code (-1 on timeout).
function Invoke-Runtime([hashtable]$environment, [string]$logFile) {
    $saved = @{}
    foreach ($name in $environment.Keys) {
        $saved[$name] = [Environment]::GetEnvironmentVariable($name)
        [Environment]::SetEnvironmentVariable($name, $environment[$name])
    }
    try {
        $process = Start-Process -FilePath $exe -WorkingDirectory $RepoRoot -RedirectStandardError $logFile `
            -RedirectStandardOutput "$logFile.out" -PassThru
        if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
            Stop-Process -Id $process.Id -Force
            return -1
        }
        return $process.ExitCode
    }
    finally {
        foreach ($name in $saved.Keys) { [Environment]::SetEnvironmentVariable($name, $saved[$name]) }
    }
}

if ($Record) {
    foreach ($scene in $Scenes) {
        if (-not $sceneSet.ContainsKey($scene)) { Fail "unknown scene $scene" }
        $dir = Join-Path $recordingsDir $scene
        Remove-Item -Recurse -Force $dir -ErrorAction SilentlyContinue
        New-Item -ItemType Directory -Force $dir | Out-Null
        $environment = @{
            ROAN_GPU_API = $RecordApi; ROAN_GPU_DEBUG = $null
            ROAN_SCRIPT = $sceneSet[$scene].Script; ROAN_RECORD_DIR = $dir; ROAN_RECORD_FRAMES = $sceneSet[$scene].Frames
        }
        if (-not $environment.ROAN_SCRIPT) { $environment.ROAN_SCRIPT = $null }
        $code = Invoke-Runtime $environment (Join-Path $dir 'record.log')
        $files = @(Get-ChildItem $dir -Filter '*.rxf')
        Write-Host ("{0}: {1} frames recorded (exit {2})" -f $scene, $files.Count, $code)
        if ($code -ne 0 -or $files.Count -eq 0) { Fail "recording $scene failed; see $(Join-Path $dir 'record.log')" }
    }
    exit 0
}

Add-Type -TypeDefinition @"
using System;
using System.IO;
public static class Ppm {
    // Width, height and the RGB bytes of a binary PPM (P6, maxval 255).
    public static byte[] Read(string path, out int width, out int height) {
        byte[] file = File.ReadAllBytes(path);
        int at = 0;
        string[] fields = new string[4];
        for (int i = 0; i < 4; i++) {
            while (char.IsWhiteSpace((char)file[at])) at++;
            int start = at;
            while (!char.IsWhiteSpace((char)file[at])) at++;
            fields[i] = System.Text.Encoding.ASCII.GetString(file, start, at - start);
        }
        at++;
        if (fields[0] != "P6" || fields[3] != "255") throw new InvalidDataException(path + " is not a binary PPM");
        width = int.Parse(fields[1]);
        height = int.Parse(fields[2]);
        byte[] rgb = new byte[width * height * 3];
        Array.Copy(file, at, rgb, 0, rgb.Length);
        return rgb;
    }
    // Mean and largest channel difference, and the pixels whose largest difference passes threshold.
    public static double[] Diff(string a, string b, int threshold) {
        int wa, ha, wb, hb;
        byte[] x = Read(a, out wa, out ha);
        byte[] y = Read(b, out wb, out hb);
        if (wa != wb || ha != hb) return null;
        long sum = 0; int max = 0; long over = 0;
        for (int i = 0; i < x.Length; i += 3) {
            int pixel = 0;
            for (int c = 0; c < 3; c++) {
                int d = Math.Abs(x[i + c] - y[i + c]);
                sum += d;
                if (d > pixel) pixel = d;
            }
            if (pixel > max) max = pixel;
            if (pixel > threshold) over++;
        }
        return new double[] { (double)sum / x.Length, max, (double)over / (x.Length / 3) };
    }
}
"@

$outDir = Join-Path $compareDir 'out'
$lines = @('# D3D12 vs Vulkan', '', [string]::Format([Globalization.CultureInfo]::InvariantCulture, "Threshold {0}, fail above {1}% of the pixels.", $Threshold, $MaxShare * 100), '',
    '| Frame | Image | Mean | Max | Pixels over | Result |', '| --- | --- | --- | --- | --- | --- |')
$failed = $false
foreach ($scene in $Scenes) {
    $dir = Join-Path $recordingsDir $scene
    $recordings = @(Get-ChildItem $dir -Filter '*.rxf' -ErrorAction SilentlyContinue | Sort-Object Name)
    if ($recordings.Count -eq 0) {
        Write-Host "$($scene): no recordings; run with -Record" -ForegroundColor Yellow
        $lines += "| $scene | (no recordings) | | | | missing |"
        $failed = $true
        continue
    }
    foreach ($recording in $recordings) {
        $frame = "$scene/$($recording.BaseName)"
        $images = @{}
        foreach ($api in 'd3d12', 'vulkan') {
            $out = Join-Path $outDir "$scene-$($recording.BaseName)-$api"
            Remove-Item -Recurse -Force $out -ErrorAction SilentlyContinue
            $environment = @{
                ROAN_GPU_API = $api; ROAN_GPU_DEBUG = $(if ($Validation) { '1' } else { $null })
                ROAN_REPLAY = $recording.FullName; ROAN_REPLAY_OUT = $out
            }
            New-Item -ItemType Directory -Force $outDir | Out-Null
            $code = Invoke-Runtime $environment (Join-Path $outDir "$scene-$($recording.BaseName)-$api.log")
            if ($code -ne 0) {
                $lines += "| $frame | ($api replay failed, exit $code) | | | | fail |"
                $failed = $true
                continue
            }
            $images[$api] = $out
        }
        if ($images.Count -ne 2) { continue }
        $names = @(Get-ChildItem $images.d3d12 -Filter '*.ppm' | ForEach-Object Name) + @(Get-ChildItem $images.vulkan -Filter '*.ppm' | ForEach-Object Name) |
            Sort-Object -Unique
        foreach ($name in $names) {
            $a = Join-Path $images.d3d12 $name
            $b = Join-Path $images.vulkan $name
            if (-not (Test-Path $a) -or -not (Test-Path $b)) {
                $lines += "| $frame | $name | | | | missing on $(if (Test-Path $a) { 'Vulkan' } else { 'D3D12' }) |"
                $failed = $true
                continue
            }
            $diff = [Ppm]::Diff($a, $b, $Threshold)
            if ($null -eq $diff) {
                $lines += "| $frame | $name | | | | sizes differ |"
                $failed = $true
                continue
            }
            $fail = $diff[2] -gt $MaxShare
            $failed = $failed -or $fail
            $lines += [string]::Format([Globalization.CultureInfo]::InvariantCulture, "| {0} | {1} | {2:F3} | {3} | {4:F3}% | {5} |",
                $frame, $name, $diff[0], $diff[1], ($diff[2] * 100), $(if ($fail) { 'fail' } else { 'ok' }))
        }
    }
}
$report = Join-Path $compareDir 'report.md'
$lines | Set-Content -Path $report -Encoding utf8
$lines | ForEach-Object { Write-Host $_ }
Write-Host "report: $report"
if ($failed) { exit 1 }
exit 0
