<#
.SYNOPSIS
    Builds the runtime (build/runtime/roan-xenon.exe) from the generated code in build/ppc.

.DESCRIPTION
    Run scripts/recompile.ps1 first. The first build compiles the whole generated code and takes a
    few minutes; later builds only recompile what changed.

.EXAMPLE
    ./scripts/build.ps1
    ./build/runtime/roan-xenon.exe
#>

. "$PSScriptRoot/common.ps1"

if (-not (Test-Path (Join-Path $RepoRoot 'build/ppc/ppc_func_mapping.cpp'))) {
    Fail 'build/ppc is empty; run scripts/recompile.ps1 first'
}

Use-VsDevEnvironment
Invoke-CMakeBuild $RepoRoot (Join-Path $RepoRoot 'build/runtime') @('roan-xenon')
Write-Host "built $(Join-Path $RepoRoot 'build/runtime/roan-xenon.exe')"
