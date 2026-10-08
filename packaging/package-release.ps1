param (
    [switch]$Build,
    [switch]$NoZip,
    [switch]$NoSetup,
    [string]$IsccPath = "",
    [string]$OutputRootDir = "",
    [string]$Version = ""
)

$ErrorActionPreference = "Stop"
$RootDir = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$AppBuildDir = Join-Path $RootDir 'app\out\build\win-amd64-release'
if (-not $OutputRootDir) { $OutputRootDir = Join-Path $RootDir 'dist' }
$OutputRootDir = [IO.Path]::GetFullPath($OutputRootDir)
if (-not $Version) {
    $versionSource = Get-Content (Join-Path $PSScriptRoot 'version.iss') -Raw
    if ($versionSource -notmatch '#define MyAppVersion "([^"]+)"') { throw 'Missing release version.' }
    $Version = $Matches[1]
}
if ($Version -notmatch '^\d+\.\d+\.\d+([-+][A-Za-z0-9.-]+)?$') { throw 'Invalid release version.' }

# Check tools before writing or replacing any release artifact.
if (-not $NoSetup) {
    if (-not $IsccPath) {
        $compiler = Get-Command ISCC.exe -ErrorAction SilentlyContinue
        if ($compiler) { $IsccPath = $compiler.Source }
        foreach ($base in @(${env:ProgramFiles(x86)}, $env:ProgramFiles, $env:LOCALAPPDATA)) {
            if (-not $base) { continue }
            foreach ($sub in @('Inno Setup 6\ISCC.exe', 'Programs\Inno Setup 6\ISCC.exe')) {
                $candidate = Join-Path $base $sub
                if (-not $IsccPath -and (Test-Path -LiteralPath $candidate)) { $IsccPath = $candidate }
            }
        }
    }
    if (-not $IsccPath -or -not (Test-Path -LiteralPath $IsccPath -PathType Leaf)) {
        throw 'Install Inno Setup 6.4+ or pass -IsccPath. Use -NoSetup for portable packaging only.'
    }
    $IsccPath = (Resolve-Path -LiteralPath $IsccPath).Path
}

Write-Host "Packaging NarutoRisePC $Version" -ForegroundColor Cyan
# A directory named "release" is not sufficient: validate the actual CMake mode.
cmake -S (Join-Path $RootDir 'app') -B $AppBuildDir -DCMAKE_BUILD_TYPE=Release
if ($LASTEXITCODE -ne 0) { throw 'Release configuration failed. Existing releases were not changed.' }
# Always refresh the launcher/helper; only rebuild the game when requested or missing.
$targets = @('narutorise_launcher', 'narutorise_runtime_config', 'narutorise_shader_cache')
if (-not $NoSetup) { $targets += 'narutorise_setup_helper' }
# DLC engine module: build it when codegen produced sources for it (present
# when game_root/ai2c2.dll - the DLC's AI2C@2.dll - existed at codegen time).
if (Test-Path (Join-Path $RootDir 'app\generated\ai2c2\sources.cmake')) { $targets += 'narutorise_ai2c2' }
if ($Build -or -not (Test-Path (Join-Path $AppBuildDir 'narutorise.exe'))) { $targets += 'narutorise' }
cmake --build $AppBuildDir --target $targets
if ($LASTEXITCODE -ne 0) { throw "Build failed ($LASTEXITCODE). Existing releases were not changed." }

$binaries = @('narutorise.exe', 'narutorise_launcher.exe', 'narutorise_ai2c.dll',
    'rexgpu-xenos.dll', 'rexruntime.dll', 'narutorise.toml')
foreach ($name in $binaries) {
    $path = Join-Path $AppBuildDir $name
    if (-not (Test-Path -LiteralPath $path -PathType Leaf) -or (Get-Item -LiteralPath $path).Length -eq 0) {
        throw "Missing or empty release file: $path"
    }
}
# Optional AMD FidelityFX runtime DLL (staged when REXGLUE_ENABLE_FIDELITYFX=ON).
$FidelityFxDll = 'amd_fidelityfx_dx12.dll'
if (Test-Path (Join-Path $AppBuildDir $FidelityFxDll)) {
    $binaries += $FidelityFxDll
}
# Optional DLC engine module (narutorise_ai2c2.dll): present when the build
# had the DLC's AI2C@2.dll available for codegen (see docs/dlc.md, seção 3.3).
# Without it the game runs normally, but a DLC install requires it.
$DlcEngineDll = 'narutorise_ai2c2.dll'
if (Test-Path (Join-Path $AppBuildDir $DlcEngineDll)) {
    $binaries += $DlcEngineDll
}
$HelperPath = Join-Path $AppBuildDir 'narutorise_setup_helper.exe'
if (-not $NoSetup -and -not (Test-Path -LiteralPath $HelperPath -PathType Leaf)) { throw 'Missing ISO helper.' }
foreach ($asset in @('cover.jpg', 'narutorise.ico', 'fonts\Lato-Regular.ttf', 'fonts\Lato-Bold.ttf', 'fonts\OFL.txt')) {
    if (-not (Test-Path (Join-Path $RootDir "launcher\assets\$asset"))) { throw "Missing asset: $asset" }
}
$shaderFiles = @(Get-ChildItem (Join-Path $AppBuildDir 'shader_cache\555307E5.*') -File |
    Where-Object { $_.Extension -in @('.xsh', '.xpso', '.vkspv') })
if (-not $shaderFiles.Count) { throw 'Missing Naruto shader cache.' }

# Reject non-redistributable debug CRT imports, even if a binary was left over
# from a previous configuration. llvm-readobj accompanies the LLVM compiler.
$cache = Get-Content (Join-Path $AppBuildDir 'CMakeCache.txt') -Raw
if ($cache -notmatch '(?m)^CMAKE_CXX_COMPILER:[^=]+=(.+)$') { throw 'Cannot identify the C++ compiler.' }
$compiler = $Matches[1].Trim()
# The cache may store a bare compiler name (resolved via PATH at configure
# time) instead of an absolute path; llvm-readobj.exe ships with the same
# LLVM toolchain, so fall back to PATH resolution for it.
$inspect = $null
if ([IO.Path]::IsPathRooted($compiler)) {
    $inspect = Join-Path (Split-Path $compiler -Parent) 'llvm-readobj.exe'
}
if (-not $inspect -or -not (Test-Path -LiteralPath $inspect -PathType Leaf)) {
    $found = Get-Command llvm-readobj.exe -ErrorAction SilentlyContinue
    if ($found) { $inspect = $found.Source }
}
if (-not $inspect -or -not (Test-Path -LiteralPath $inspect -PathType Leaf)) {
    throw 'llvm-readobj.exe is required to audit release dependencies.'
}
$peFiles = @($binaries | Where-Object { $_ -match '\.(exe|dll)$' })
if (-not $NoSetup) { $peFiles += 'narutorise_setup_helper.exe' }
foreach ($name in $peFiles) {
    $imports = & $inspect --coff-imports (Join-Path $AppBuildDir $name)
    if ($LASTEXITCODE -ne 0) { throw "Cannot inspect release binary: $name" }
    if ($imports -match 'Name:\s+(?:(?:MSVCP|VCRUNTIME|CONCRT)\w*D\.dll|ucrtbased\.dll)') {
        throw "Debug runtime dependency found in $name. Rebuild the game and dependencies in Release."
    }
}

# Build into a fresh staging directory. A failed compile leaves the previous
# release intact; only complete, verified artifacts are published.
$Stage = Join-Path $OutputRootDir ('.packaging-' + [guid]::NewGuid().ToString('N'))
$Portable = Join-Path $Stage 'naruto-rise-recomp-win-amd64'
New-Item -ItemType Directory -Path $Portable -Force | Out-Null
foreach ($dir in @('assets', 'shader_cache', 'game', 'licenses')) {
    New-Item -ItemType Directory -Path (Join-Path $Portable $dir) | Out-Null
}
try {
    foreach ($name in $binaries) { Copy-Item -LiteralPath (Join-Path $AppBuildDir $name) -Destination $Portable }
    Set-Content (Join-Path $Portable 'narutorise.version') -Encoding ASCII -Value $Version
    foreach ($asset in @('cover.jpg', 'narutorise.ico')) {
        Copy-Item -LiteralPath (Join-Path $RootDir "launcher\assets\$asset") -Destination (Join-Path $Portable 'assets')
    }
    Copy-Item -LiteralPath (Join-Path $RootDir 'launcher\assets\fonts') -Destination (Join-Path $Portable 'assets') -Recurse
    $shaderFiles | Copy-Item -Destination (Join-Path $Portable 'shader_cache')
    Copy-Item -LiteralPath (Join-Path $RootDir 'tools\extract-xiso\LICENSE.TXT') -Destination (Join-Path $Portable 'licenses\extract-xiso.txt')
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'install.ps1') -Destination $Portable
    Set-Content (Join-Path $Portable 'game\PLACE_GAME_FILES_HERE.txt') -Encoding UTF8 -Value @'
Place your legally obtained, extracted Naruto: Rise of a Ninja Xbox 360 files here.
The root must contain default.xex. Game data is not distributed with this port.
'@
    Set-Content (Join-Path $Portable 'README.txt') -Encoding UTF8 -Value @"
NARUTO: RISE OF A NINJA - PC PORT ($Version)

WINDOWS SETUP:
Run the separately distributed NarutoRiseInstaller.exe. The default location is
%LOCALAPPDATA%\NarutoRisePC; administrator rights are not required.
Choose your original Xbox 360 ISO, or skip it to use already extracted files.
ISO extraction is available only in Setup; run Setup again to import later.
The setup preserves game and narutorise.toml on reinstall/uninstall.

MANUAL UPDATE:
Run the new NarutoRiseInstaller.exe and choose Update an existing installation.
Select the folder containing narutorise.exe and narutorise_launcher.exe.
Close the game and launcher first. No ISO import or uninstall is needed.
Game files, configuration, saves and DLCs are preserved. Portable installations
can also be updated this way; Setup registers them as installed applications.

PORTABLE PACKAGE:
Extract this folder to a writable location and run narutorise_launcher.exe.
Select an extracted game folder in GAME FILES, or put your original extracted
files into game (default.xex must be at its root). The launcher does not import ISOs.
Optional: run install.ps1 to create shortcuts.

SETTINGS:
Interface languages: English, Brazilian Portuguese, French, German, Spanish,
Italian, and Russian. Game languages: English, French, German, Spanish, Italian.
Resolution scale, fullscreen, VSync,
FXAA anti-aliasing, AMD FidelityFX CAS/FSR post-processing, anisotropic
filtering, aspect ratios 16:9 / 21:9 / 32:9, and FPS overlay (F1).
SDL controller support includes Xbox and PlayStation controllers.
Microsoft Visual C++ 2015-2022 Redistributable (x64) is required by the game/launcher.
Alt+F4 closes the game; F11 toggles fullscreen.

Game files are not included. extract-xiso notices: licenses\extract-xiso.txt.
"@

    $Artifacts = @('naruto-rise-recomp-win-amd64')
    if (-not $NoZip) {
        Compress-Archive -Path "$Portable\*" -DestinationPath (Join-Path $Stage 'naruto-rise-recomp-win-amd64.zip')
        $Artifacts += 'naruto-rise-recomp-win-amd64.zip'
    }
    if (-not $NoSetup) {
        $portBytes = (Get-ChildItem -LiteralPath $Portable -Recurse -File | Measure-Object Length -Sum).Sum
        & $IsccPath "/DPackageDir=$Portable" "/DHelperPath=$HelperPath" "/DMyAppVersion=$Version" "/DPortBytes=$portBytes" "/O$Stage" (Join-Path $PSScriptRoot 'installer.iss')
        if ($LASTEXITCODE -ne 0) { throw "Inno Setup compilation failed ($LASTEXITCODE)." }
        $setup = Join-Path $Stage 'NarutoRiseInstaller.exe'
        if (-not (Test-Path $setup) -or (Get-Item $setup).Length -eq 0) { throw 'Setup output missing.' }
        $Artifacts += 'NarutoRiseInstaller.exe'
    }

    # Keep existing artifacts as backups instead of recursively deleting user data.
    $Backup = Join-Path $OutputRootDir ('previous-release-' + [guid]::NewGuid().ToString('N'))
    $Published = @()
    $BackedUp = @()
    try {
        foreach ($name in $Artifacts) {
            $existing = Join-Path $OutputRootDir $name
            if (Test-Path -LiteralPath $existing) {
                New-Item -ItemType Directory -Path $Backup -Force | Out-Null
                Move-Item -LiteralPath $existing -Destination (Join-Path $Backup $name)
                $BackedUp += $name
            }
            Move-Item -LiteralPath (Join-Path $Stage $name) -Destination $existing
            $Published += $name
        }
    } catch {
        # Restore old artifacts without deleting them if a destination is locked.
        foreach ($name in $Published) {
            Move-Item -LiteralPath (Join-Path $OutputRootDir $name) -Destination (Join-Path $Stage $name)
        }
        foreach ($name in $BackedUp) {
            Move-Item -LiteralPath (Join-Path $Backup $name) -Destination (Join-Path $OutputRootDir $name)
        }
        throw
    }
    Write-Host "Release created in $OutputRootDir" -ForegroundColor Green
    if (Test-Path $Backup) { Write-Host "Previous artifacts preserved in $Backup" }
} finally {
    # Stage is a unique directory created by this invocation, never an existing release.
    Remove-Item -LiteralPath $Stage -Recurse -Force -ErrorAction SilentlyContinue
}
