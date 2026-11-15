<#
.SYNOPSIS
    Applies (or reverts) the patches in patches/<submodule> to the submodules.

.DESCRIPTION
    Each folder inside patches/ is named after a submodule (the last part of its path in
    .gitmodules, such as tools/XenonRecomp or thirdparty/plume) and holds .patch files
    generated with "git diff". They are applied in alphabetical order.
    The script is idempotent: patches that are already applied are skipped.

.EXAMPLE
    ./patches/apply.ps1           # apply everything
    ./patches/apply.ps1 -Revert   # revert everything (for example, before updating a submodule)
#>
param(
    [switch]$Revert
)

# No 'Stop' here: Windows PowerShell 5.1 turns git's stderr into terminating errors.
# Failures are detected through $LASTEXITCODE instead.
$repoRoot = Split-Path -Parent $PSScriptRoot
$failed = $false

# Submodule paths by their last component, from .gitmodules.
$submodules = @{}
foreach ($line in Get-Content (Join-Path $repoRoot '.gitmodules')) {
    if ($line -match '^\s*path\s*=\s*(.+?)\s*$') {
        $submodules[(Split-Path -Leaf $Matches[1])] = Join-Path $repoRoot $Matches[1]
    }
}

foreach ($dir in Get-ChildItem -Path $PSScriptRoot -Directory | Sort-Object Name) {
    $submodule = $submodules[$dir.Name]
    if (-not $submodule) {
        Write-Warning "$($dir.Name): no submodule with that name in .gitmodules"
        $failed = $true
        continue
    }
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
