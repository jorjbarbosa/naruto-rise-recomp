#Requires -Version 7.2
$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $MyInvocation.MyCommand.Path

Write-Host "== Naruto: Rise of a Ninja - ReXGlue project setup ==" -ForegroundColor Cyan

# -----------------------------------------------------------------------------
# 1. Verify toolchain prerequisites
# -----------------------------------------------------------------------------
foreach ($tool in @("git", "cmake", "ninja", "clang")) {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        Write-Host "MISSING: $tool not found on PATH." -ForegroundColor Red
        Write-Host "  ReXGlue requires: Clang 18+, CMake 3.25+, Ninja, Visual Studio 2022 (Windows SDK)."
        exit 1
    }
}
Write-Host "Prerequisites OK." -ForegroundColor Green

# -----------------------------------------------------------------------------
# 2. Initialize SDK submodule
# -----------------------------------------------------------------------------
# The SDK is tracked as a Git submodule at sdk/ (currently a fork with local
# patches). This ensures the correct commit is checked out.
Write-Host "Initializing SDK submodule ..." -ForegroundColor Cyan
git -C $root submodule update --init --recursive
if ($LASTEXITCODE -ne 0) { throw "SDK submodule init failed" }

# -----------------------------------------------------------------------------
# 3. Download extract-xiso (used by the setup helper to import Xbox 360 ISOs)
# -----------------------------------------------------------------------------
$extractXisoDir = Join-Path $root "tools\extract-xiso"
$extractXisoExe = Join-Path $extractXisoDir "extract-xiso.exe"

if (Test-Path $extractXisoExe) {
    Write-Host "extract-xiso already present." -ForegroundColor Green
} else {
    Write-Host "Downloading extract-xiso (Win64 Release) ..." -ForegroundColor Cyan
    New-Item -ItemType Directory -Path $extractXisoDir -Force | Out-Null

    $releaseApi = "https://api.github.com/repos/XboxDev/extract-xiso/releases/latest"
    $release = Invoke-RestMethod -Uri $releaseApi -Headers @{ Accept = "application/vnd.github+json" }
    $asset = $release.assets | Where-Object { $_.name -eq "extract-xiso-Win64_Release.zip" }
    if (-not $asset) { throw "Could not find extract-xiso-Win64_Release.zip in latest release" }

    $zipPath = Join-Path $env:TEMP "extract-xiso-Win64_Release.zip"
    Invoke-WebRequest -Uri $asset.browser_download_url -OutFile $zipPath -UseBasicParsing

    Expand-Archive -Path $zipPath -DestinationPath $extractXisoDir -Force
    Remove-Item $zipPath -ErrorAction SilentlyContinue

    # The release zip places the binary under an artifacts/ subfolder.
    $nestedExe = Join-Path $extractXisoDir "artifacts\extract-xiso.exe"
    if (Test-Path $nestedExe) {
        Move-Item -Path $nestedExe -Destination $extractXisoExe -Force
        Remove-Item (Join-Path $extractXisoDir "artifacts") -Recurse -Force -ErrorAction SilentlyContinue
    }

    if (-not (Test-Path $extractXisoExe)) { throw "extract-xiso.exe was not extracted" }
    Write-Host "extract-xiso downloaded to $extractXisoExe" -ForegroundColor Green
}

# -----------------------------------------------------------------------------
# 4. Apply local SDK patches (prepared for Fase 3)
# -----------------------------------------------------------------------------
$applySdkPatches = Join-Path $root "patches\apply_sdk_patches.ps1"
if (Test-Path $applySdkPatches) {
    Write-Host "Applying local SDK patches ..." -ForegroundColor Cyan
    & $applySdkPatches
    if ($LASTEXITCODE -ne 0) { throw "SDK patch application failed" }
}

Write-Host ""
Write-Host "Setup complete." -ForegroundColor Green
Write-Host "Next steps:"
Write-Host "  1. Extract your Xbox 360 ISO into .\game_root\ (entrypoint at game_root\default.xex)"
Write-Host "  2. Build the SDK CLI:  cmake --preset win-amd64 -S sdk -B sdk\out\build\win-amd64 ; cmake --build sdk\out\build\win-amd64 --config Release --target install"
Write-Host "  3. Regenerate SDK-managed files:  sdk\out\install\win-amd64\bin\rexglue.exe init --force --project-name narutorise --project-root . --xex-path game_root\default.xex --game-root game_root"
Write-Host "  4. Configure & build the port:  cmake --preset win-amd64-release -S app -B app\out\build\win-amd64-release ; cmake --build app\out\build\win-amd64-release"
