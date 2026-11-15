<#
.SYNOPSIS
    Builds the patched recompilers and translates the game: game/default.xex into build/ppc,
    game/AI2C.dll into build/ppc_ai2c and the shaders of game/xeshader.sdb into build/shaders.

.DESCRIPTION
    1. Applies patches/ to the submodules (patches/apply.ps1).
    2. Builds XenonAnalyse and XenonRecomp with clang-cl and Ninja into build/tools/XenonRecomp.
    3. For each module (config/default.toml, config/ai2c.toml), recreates its output directory and
       runs XenonRecomp. The logs go to build/recompile-<module>.log. Any warning from XenonRecomp
       makes the script fail, so a config or recompiler regression does not go unnoticed.
    4. Builds XenosRecomp into build/tools/XenosRecomp and translates every shader of
       game/xeshader.sdb to DXIL and SPIR-V, written as build/shaders/shader_cache.cpp. A shader
       that does not compile makes the script fail (its HLSL is left in build/shaders/*.failed).

    Requires Visual Studio with "C++ Clang tools for Windows" (see scripts/common.ps1).

.PARAMETER SwitchTables
    Also runs XenonAnalyse on each module and writes its jump table output to
    build/<module>_switch_tables.toml, to compare with config/<module>_switch_tables.toml (the
    default.xex one adds the tables XenonAnalyse misses).

.EXAMPLE
    ./scripts/recompile.ps1
#>
param(
    [switch]$SwitchTables
)

. "$PSScriptRoot/common.ps1"

# Recompiled modules: the image, its XenonRecomp config and the output directory (both relative to the
# repository root, matching the paths inside the config).
$modules = @(
    @{ Name = 'default'; Image = 'game/default.xex'; Config = 'config/default.toml'; Output = 'build/ppc' },
    @{ Name = 'ai2c'; Image = 'game/AI2C.dll'; Config = 'config/ai2c.toml'; Output = 'build/ppc_ai2c' }
)
$submodule = Join-Path $RepoRoot 'tools/XenonRecomp'
$toolsBuild = Join-Path $RepoRoot 'build/tools/XenonRecomp'

foreach ($module in $modules) {
    if (-not (Test-Path (Join-Path $RepoRoot $module.Image))) {
        Fail "$($module.Image) not found (copy the game files to $(Join-Path $RepoRoot 'game'))"
    }
}
if (-not (Test-Path (Join-Path $submodule '.git'))) { Fail 'tools/XenonRecomp is not initialized (run: git submodule update --init --recursive)' }

# 1. Patches
& (Join-Path $RepoRoot 'patches/apply.ps1')
if ($LASTEXITCODE -ne 0) { Fail 'could not apply the patches' }

# 2. Tools
Use-VsDevEnvironment
Invoke-CMakeBuild $submodule $toolsBuild @('XenonAnalyse', 'XenonRecomp')

$xenonAnalyse = Join-Path $toolsBuild 'XenonAnalyse/XenonAnalyse.exe'
$xenonRecomp = Join-Path $toolsBuild 'XenonRecomp/XenonRecomp.exe'

foreach ($module in $modules) {
    $image = Join-Path $RepoRoot $module.Image
    $output = Join-Path $RepoRoot $module.Output
    $log = Join-Path $RepoRoot "build/recompile-$($module.Name).log"

    if ($SwitchTables) {
        $tables = Join-Path $RepoRoot "build/$($module.Name)_switch_tables.toml"
        & $xenonAnalyse $image $tables
        if ($LASTEXITCODE -ne 0) { Fail "XenonAnalyse failed on $($module.Image)" }
        Write-Host "XenonAnalyse output: $tables"
    }

    # 3. Recompile
    if (Test-Path $output) { Remove-Item -Recurse -Force $output }
    New-Item -ItemType Directory -Force $output | Out-Null

    $lines = & $xenonRecomp (Join-Path $RepoRoot $module.Config) (Join-Path $submodule 'XenonUtils/ppc_context.h')
    $exitCode = $LASTEXITCODE
    $lines | Set-Content -Encoding UTF8 $log
    if ($exitCode -ne 0) { Fail "XenonRecomp exited with code $exitCode on $($module.Image) (see $log)" }

    $warnings = @($lines | Where-Object { $_ -and $_ -notmatch '^Recompiling functions' })
    $files = @(Get-ChildItem $output -File)
    $size = ($files | Measure-Object Length -Sum).Sum / 1MB
    Write-Host ("{0}: generated {1} files ({2:N0} MB) in {3}" -f $module.Image, $files.Count, $size, $output)

    if ($warnings.Count -gt 0) {
        $warnings | Select-Object -First 20 | ForEach-Object { Write-Host "  $_" }
        Fail "XenonRecomp reported $($warnings.Count) warnings on $($module.Image) (see $log)"
    }
    Write-Host 'no warnings'
}

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
