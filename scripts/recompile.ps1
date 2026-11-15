<#
.SYNOPSIS
    Builds the patched recompilers and translates the game: game/default.xex into build/ppc and the
    shaders of game/xeshader.sdb into build/shaders.

.DESCRIPTION
    1. Applies patches/ to the submodules (patches/apply.ps1).
    2. Builds XenonAnalyse and XenonRecomp with clang-cl and Ninja into build/tools/XenonRecomp.
    3. Recreates build/ppc and runs XenonRecomp with config/default.toml. The log goes to
       build/recompile.log. Any warning from XenonRecomp makes the script fail, so a config or
       recompiler regression does not go unnoticed.
    4. Builds XenosRecomp into build/tools/XenosRecomp and translates every shader of
       game/xeshader.sdb to DXIL and SPIR-V, written as build/shaders/shader_cache.cpp. A shader
       that does not compile makes the script fail (its HLSL is left in build/shaders/*.failed).

    Requires Visual Studio with "C++ Clang tools for Windows" (see scripts/common.ps1).

.PARAMETER SwitchTables
    Also runs XenonAnalyse and writes its jump table output to build/default_switch_tables.toml,
    to compare with config/default_switch_tables.toml (which adds the tables XenonAnalyse misses).

.EXAMPLE
    ./scripts/recompile.ps1
#>
param(
    [switch]$SwitchTables
)

. "$PSScriptRoot/common.ps1"

$xex = Join-Path $RepoRoot 'game/default.xex'
$config = Join-Path $RepoRoot 'config/default.toml'
$submodule = Join-Path $RepoRoot 'tools/XenonRecomp'
$toolsBuild = Join-Path $RepoRoot 'build/tools/XenonRecomp'
$output = Join-Path $RepoRoot 'build/ppc'
$log = Join-Path $RepoRoot 'build/recompile.log'

if (-not (Test-Path $xex)) { Fail "game/default.xex not found (copy the game files to $(Join-Path $RepoRoot 'game'))" }
if (-not (Test-Path (Join-Path $submodule '.git'))) { Fail 'tools/XenonRecomp is not initialized (run: git submodule update --init --recursive)' }

# 1. Patches
& (Join-Path $RepoRoot 'patches/apply.ps1')
if ($LASTEXITCODE -ne 0) { Fail 'could not apply the patches' }

# 2. Tools
Use-VsDevEnvironment
Invoke-CMakeBuild $submodule $toolsBuild @('XenonAnalyse', 'XenonRecomp')

$xenonAnalyse = Join-Path $toolsBuild 'XenonAnalyse/XenonAnalyse.exe'
$xenonRecomp = Join-Path $toolsBuild 'XenonRecomp/XenonRecomp.exe'

if ($SwitchTables) {
    $tables = Join-Path $RepoRoot 'build/default_switch_tables.toml'
    & $xenonAnalyse $xex $tables
    if ($LASTEXITCODE -ne 0) { Fail 'XenonAnalyse failed' }
    Write-Host "XenonAnalyse output: $tables"
}

# 3. Recompile
if (Test-Path $output) { Remove-Item -Recurse -Force $output }
New-Item -ItemType Directory -Force $output | Out-Null

$lines = & $xenonRecomp $config (Join-Path $submodule 'XenonUtils/ppc_context.h')
$exitCode = $LASTEXITCODE
$lines | Set-Content -Encoding UTF8 $log
if ($exitCode -ne 0) { Fail "XenonRecomp exited with code $exitCode (see $log)" }

$warnings = @($lines | Where-Object { $_ -and $_ -notmatch '^Recompiling functions' })
$files = @(Get-ChildItem $output -File)
$size = ($files | Measure-Object Length -Sum).Sum / 1MB
Write-Host ("generated {0} files ({1:N0} MB) in {2}" -f $files.Count, $size, $output)

if ($warnings.Count -gt 0) {
    $warnings | Select-Object -First 20 | ForEach-Object { Write-Host "  $_" }
    Fail "XenonRecomp reported $($warnings.Count) warnings (see $log)"
}
Write-Host 'no warnings'

# 4. Shaders
$shaderDb = Join-Path $RepoRoot 'game/xeshader.sdb'
$xenosSubmodule = Join-Path $RepoRoot 'tools/XenosRecomp'
$xenosBuild = Join-Path $RepoRoot 'build/tools/XenosRecomp'
$shaderOutput = Join-Path $RepoRoot 'build/shaders'

if (-not (Test-Path $shaderDb)) { Fail "game/xeshader.sdb not found" }
if (-not (Test-Path (Join-Path $xenosSubmodule '.git'))) { Fail 'tools/XenosRecomp is not initialized (run: git submodule update --init --recursive)' }

Invoke-CMakeBuild $xenosSubmodule $xenosBuild @('XenosRecomp')

if (Test-Path $shaderOutput) { Remove-Item -Recurse -Force $shaderOutput }
New-Item -ItemType Directory -Force $shaderOutput | Out-Null

$shaderCache = Join-Path $shaderOutput 'shader_cache.cpp'
& (Join-Path $xenosBuild 'XenosRecomp/XenosRecomp.exe') $shaderDb $shaderCache (Join-Path $xenosSubmodule 'XenosRecomp/shader_common.h') |
    Where-Object { $_ -notmatch '^Recompiling shaders' }
if ($LASTEXITCODE -ne 0) { Fail "XenosRecomp exited with code $LASTEXITCODE" }
Write-Host ("generated {0} ({1:N1} MB)" -f $shaderCache, ((Get-Item $shaderCache).Length / 1MB))
