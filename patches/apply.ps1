<#
.SYNOPSIS
    Applies (or reverts) the patches in patches/<submodule> to the submodules in tools/.

.DESCRIPTION
    Each folder inside patches/ is named after a submodule in tools/ and holds .patch
    files generated with "git diff". They are applied in alphabetical order.
    The script is idempotent: patches that are already applied are skipped.

.EXAMPLE
    ./patches/apply.ps1           # apply everything
    ./patches/apply.ps1 -Revert   # revert everything (for example, before updating a submodule)
#>
param(
    [switch]$Revert
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$failed = $false

foreach ($dir in Get-ChildItem -Path $PSScriptRoot -Directory | Sort-Object Name) {
    $submodule = Join-Path $repoRoot (Join-Path 'tools' $dir.Name)
    if (-not (Test-Path (Join-Path $submodule '.git'))) {
        Write-Warning "$($dir.Name): submodule not initialized at $submodule (run: git submodule update --init --recursive)"
        $failed = $true
        continue
    }

    $patches = @(Get-ChildItem -Path $dir.FullName -Filter '*.patch' | Sort-Object Name)
    if ($Revert) { [array]::Reverse($patches) }

    foreach ($patch in $patches) {
        $name = "$($dir.Name)/$($patch.Name)"

        git -C $submodule apply --check --reverse $patch.FullName 2>$null
        $applied = ($LASTEXITCODE -eq 0)

        if ($Revert) {
            if (-not $applied) { Write-Host "not applied, skipping: $name"; continue }
            git -C $submodule apply --reverse $patch.FullName
            if ($LASTEXITCODE -ne 0) { Write-Warning "failed to revert: $name"; $failed = $true; break }
            Write-Host "reverted: $name"
        }
        else {
            if ($applied) { Write-Host "already applied: $name"; continue }
            git -C $submodule apply --check $patch.FullName
            if ($LASTEXITCODE -ne 0) { Write-Warning "does not apply (did the submodule change?): $name"; $failed = $true; break }
            git -C $submodule apply $patch.FullName
            Write-Host "applied: $name"
        }
    }
}

if ($failed) { exit 1 }
