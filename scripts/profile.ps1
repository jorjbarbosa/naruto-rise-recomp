<#
.SYNOPSIS
    Samples the threads of a running roan-xenon and lists where each one spends its CPU (Windows).

.DESCRIPTION
    Builds the sampler (tools/sampler, target roan-sampler, out of the default build) when needed and
    attaches it to the running build/runtime/roan-xenon.exe for -Seconds. For each busy thread it
    prints its CPU time and the functions with the most self and inclusive time; guest functions show
    as sub_XXXXXXXX, with their curated names when tools/dev/ida/names.tsv exists. Reach the scene in
    the game first, then run this. -Fps adds ms a frame: the game's frame rate during the run (30
    when paced). The game runs a little slower while it is sampled.

    The numbers rank, they do not measure: short functions called very often come out larger than
    they are, and cache misses hide. Time a candidate directly before replacing it.

.EXAMPLE
    ./scripts/profile.ps1 -Seconds 20 -Fps 30
    ./scripts/profile.ps1 -Seconds 30 -Top 40 -Out build/profile.txt
#>
param(
    [int]$Seconds = 20,
    [int]$Rate = 200,       # sampling rounds a second
    [int]$Top = 25,         # functions listed per thread and list
    [double]$Fps = 0,
    [string]$Out = ''       # also write the report to this file
)

. "$PSScriptRoot/common.ps1"

Use-VsDevEnvironment
Invoke-CMakeBuild $RepoRoot (Join-Path $RepoRoot 'build/runtime') @('roan-sampler')
$sampler = Join-Path $RepoRoot 'build/runtime/roan-sampler.exe'

$exe = (Resolve-Path (Join-Path $RepoRoot 'build/runtime/roan-xenon.exe')).Path
$process = Get-Process roan-xenon -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $exe } | Select-Object -First 1
if (-not $process) { Fail "no running $exe" }

$arguments = @('--pid', $process.Id, '--seconds', $Seconds, '--rate', $Rate, '--top', $Top)
if ($Fps -gt 0) { $arguments += @('--fps', $Fps) }
$names = Join-Path $RepoRoot 'tools/dev/ida/names.tsv'
if (Test-Path $names) { $arguments += @('--names', $names) }

if ($Out) {
    & $sampler @arguments | Tee-Object -FilePath $Out
} else {
    & $sampler @arguments
}
if ($LASTEXITCODE -ne 0) { Fail 'the sampler failed' }
