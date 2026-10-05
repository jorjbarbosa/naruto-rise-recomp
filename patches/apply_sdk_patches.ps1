#Requires -Version 5.1
$ErrorActionPreference = "Stop"

$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$sdkDir = Join-Path $root "sdk"

if (-not (Test-Path (Join-Path $sdkDir ".git"))) {
    Write-Host "SDK not found at $sdkDir. Run setup.ps1 first." -ForegroundColor Red
    exit 1
}

$patchDir = Join-Path $root "patches\sdk"
$patches = Get-ChildItem -Path $patchDir -Filter "*.patch" | Sort-Object Name

if (-not $patches) {
    Write-Host "No SDK patches to apply." -ForegroundColor Green
    exit 0
}

foreach ($patch in $patches) {
    Write-Host "Applying SDK patch: $($patch.Name) ..." -ForegroundColor Cyan

    # Check if the patch is already applied by trying to reverse it.
    git -C $sdkDir apply --reverse --check $patch.FullName 2>$null
    if ($LASTEXITCODE -eq 0) {
        Write-Host "  already applied." -ForegroundColor Yellow
        continue
    }

    # Check if the patch can be applied cleanly.
    git -C $sdkDir apply --check $patch.FullName 2>$null
    if ($LASTEXITCODE -ne 0) { throw "Patch $($patch.Name) does not apply cleanly" }

    git -C $sdkDir apply $patch.FullName
    if ($LASTEXITCODE -ne 0) { throw "Failed to apply $($patch.Name)" }
    Write-Host "  applied." -ForegroundColor Green
}
