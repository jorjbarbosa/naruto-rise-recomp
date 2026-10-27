param (
    [switch]$Build,
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

# Only curated packages from source belong in a release, never local build mods.
$BundledModFiles = @('mods\pt-br\mod.toml', 'mods\pt-br\texts.json')
foreach ($name in $BundledModFiles) {
    $path = Join-Path $RootDir $name
    if (-not (Test-Path -LiteralPath $path -PathType Leaf) -or (Get-Item -LiteralPath $path).Length -eq 0) {
        throw "Missing or empty bundled translation file: $path"
    }
}

Write-Host "Packaging NarutoRisePC $Version" -ForegroundColor Cyan
# A directory named "release" is not sufficient: validate the actual CMake mode.
cmake -S (Join-Path $RootDir 'app') -B $AppBuildDir -DCMAKE_BUILD_TYPE=Release
if ($LASTEXITCODE -ne 0) { throw 'Release configuration failed. Existing releases were not changed.' }
$releaseCache = Get-Content -LiteralPath (Join-Path $AppBuildDir 'CMakeCache.txt') -Raw
if ($releaseCache -notmatch '(?m)^NARUTORISE_NATIVE_RENDERER_BOOTSTRAP:BOOL=(ON|TRUE|YES|1)\s*$') {
    throw 'The ZIP requires the native setup screen. Configure with -DNARUTORISE_NATIVE_RENDERER_BOOTSTRAP=ON before packaging. Existing releases were not changed.'
}
# Refresh the host too: adding a module changes its embedded registry. Ninja
# skips unchanged compilation; -Build remains accepted for script compatibility.
$targets = @('narutorise', 'narutorise_ai2c', 'narutorise_runtime_config', 'narutorise_shader_cache')
# Build every character DLC engine revision emitted by codegen.
$DlcEngineDlls = @()
$ModuleRegistry = Get-Content (Join-Path $RootDir 'app\generated\default\module_registry.cpp') -Raw
foreach ($revision in 1..3) {
    if ($ModuleRegistry.Contains('"ai2c@' + $revision + '.dll"')) {
        $targets += "narutorise_ai2c$revision"
        $DlcEngineDlls += "narutorise_ai2c$revision.dll"
    }
}
cmake --build $AppBuildDir --target $targets
if ($LASTEXITCODE -ne 0) { throw "Build failed ($LASTEXITCODE). Existing releases were not changed." }

$binaries = @('narutorise.exe', 'narutorise_ai2c.dll',
    'rexgpu-xenos.dll', 'rexruntime.dll', 'defaults\narutorise.toml')
foreach ($name in $binaries) {
    $path = Join-Path $AppBuildDir $name
    if (-not (Test-Path -LiteralPath $path -PathType Leaf) -or (Get-Item -LiteralPath $path).Length -eq 0) {
        throw "Missing or empty release file: $path"
    }
}
# Optional AMD FidelityFX runtime DLL (staged when REXGLUE_ENABLE_FIDELITYFX=ON).
$sentryCache = Get-Content -LiteralPath (Join-Path $AppBuildDir 'CMakeCache.txt') -Raw
$sentryEnabled = $sentryCache -match '(?m)^NARUTORISE_ENABLE_SENTRY:BOOL=(ON|TRUE|YES|1)\s*$'
if ($sentryEnabled) {
    $sentryRuntime = @('crashpad_handler.exe', 'crashpad_wer.dll', 'sentry-notices.txt')
    if ($sentryCache -match '(?m)^NARUTORISE_SENTRY_SHARED:INTERNAL=(ON|TRUE|YES|1)\s*$') {
        $sentryRuntime += 'sentry.dll'
    }
    foreach ($name in $sentryRuntime) {
        $path = Join-Path $AppBuildDir $name
        if (-not (Test-Path -LiteralPath $path -PathType Leaf) -or (Get-Item -LiteralPath $path).Length -eq 0) {
            throw "Missing Sentry runtime file: $path"
        }
        $binaries += $name
    }
}
$FidelityFxDll = 'amd_fidelityfx_dx12.dll'
if (Test-Path (Join-Path $AppBuildDir $FidelityFxDll)) {
    $binaries += $FidelityFxDll
}
# Never silently omit a DLL that this build's generated registry needs.
# Use generated revisions rather than copying stale DLLs from earlier builds.
foreach ($name in $DlcEngineDlls) {
    $path = Join-Path $AppBuildDir $name
    if (-not (Test-Path -LiteralPath $path -PathType Leaf) -or (Get-Item -LiteralPath $path).Length -eq 0) {
        throw "Missing or empty DLC engine module: $path"
    }
    $binaries += $name
}
$shaderFiles = @(Get-ChildItem (Join-Path $AppBuildDir 'shader_cache\555307E5.*') -File |
    Where-Object { $_.Extension -in @('.xsh', '.xpso', '.vkspv') })
if (-not $shaderFiles.Count) {
    throw 'Missing Naruto shader cache: place 555307E5.xsh and 555307E5.rtv.d3d12.xpso in packaging\private\shader_cache (or set NARUTORISE_SHADER_STORAGE_DIR).'
}

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
foreach ($name in $peFiles) {
    $imports = & $inspect --coff-imports (Join-Path $AppBuildDir $name)
    if ($LASTEXITCODE -ne 0) { throw "Cannot inspect release binary: $name" }
    if ($imports -match 'Name:\s+(?:(?:MSVCP|VCRUNTIME|CONCRT)\w*D\.dll|ucrtbased\.dll)') {
        throw "Debug runtime dependency found in $name. Rebuild the game and dependencies in Release."
    }
}

# Notices of the third-party components this build ships (third-party-licenses.psd1).
$ReleaseParts = @{
    FidelityFX = $binaries -contains $FidelityFxDll
    NativeRenderer = $cache -match '(?m)^NARUTORISE_NATIVE_RENDERER_BOOTSTRAP:BOOL=(ON|TRUE|YES|1)\s*$'
    NativeVulkan = ($cache -match '(?m)^NARUTORISE_NATIVE_RENDERER_BOOTSTRAP:BOOL=(ON|TRUE|YES|1)\s*$') -and
                   ($cache -match '(?m)^NARUTORISE_NATIVE_VULKAN:BOOL=(ON|TRUE|YES|1)\s*$')
}
$Licenses = @((Import-PowerShellDataFile (Join-Path $PSScriptRoot 'third-party-licenses.psd1')).Components |
    Where-Object { -not $_.Condition -or $ReleaseParts[$_.Condition] })
if (-not (Test-Path -LiteralPath (Join-Path $RootDir 'LICENSE') -PathType Leaf)) { throw 'Missing project LICENSE.' }
foreach ($component in $Licenses) {
    if (-not (Test-Path -LiteralPath (Join-Path $RootDir $component.File) -PathType Leaf)) {
        throw "Missing license text for $($component.Name): $($component.File)"
    }
}
# The LGPL libraries are statically linked into rexruntime.dll: name the exact
# source revisions it is built from, so it can be rebuilt with modified copies.
function Get-SourceRevision([string]$Path) {
    $revision = git -C $Path rev-parse HEAD
    if ($LASTEXITCODE -ne 0 -or -not $revision) { throw "Cannot read the source revision of $Path." }
    return $revision.Trim()
}
$SdkDir = Join-Path $RootDir 'sdk'
$SourceRevisions = @(
    @{ Name = 'Naruto: Rise of a Ninja PC port (SDK patches: patches\sdk)'; Url = 'https://github.com/jorjbarbosa/naruto-rise-recomp'; Revision = Get-SourceRevision $RootDir }
    @{ Name = 'ReXGlue SDK (rexruntime.dll)'; Url = 'https://github.com/rexglue/rexglue-sdk'; Revision = Get-SourceRevision $SdkDir }
    @{ Name = 'FFmpeg'; Url = 'https://github.com/wmarti/FFmpeg'; Revision = Get-SourceRevision (Join-Path $SdkDir 'thirdparty\FFmpeg') }
    @{ Name = 'libmspack'; Url = 'https://github.com/kyz/libmspack'; Revision = Get-SourceRevision (Join-Path $SdkDir 'thirdparty\libmspack') }
)

# Build into a fresh staging directory. A failed compile leaves the previous
# release intact; only complete, verified artifacts are published.
$Stage = Join-Path $OutputRootDir ('.packaging-' + [guid]::NewGuid().ToString('N'))
$Portable = Join-Path $Stage 'naruto-rise-recomp-win-amd64'
New-Item -ItemType Directory -Path $Portable -Force | Out-Null
foreach ($dir in @('shader_cache', 'licenses')) {
    New-Item -ItemType Directory -Path (Join-Path $Portable $dir) | Out-Null
}
try {
    foreach ($name in $binaries) {
        $destination = Join-Path $Portable $name
        New-Item -ItemType Directory -Path (Split-Path $destination -Parent) -Force | Out-Null
        Copy-Item -LiteralPath (Join-Path $AppBuildDir $name) -Destination $destination
    }
    foreach ($name in $BundledModFiles) {
        $destination = Join-Path $Portable (Join-Path 'defaults' $name)
        New-Item -ItemType Directory -Path (Split-Path $destination -Parent) -Force | Out-Null
        Copy-Item -LiteralPath (Join-Path $RootDir $name) -Destination $destination
    }
    Set-Content (Join-Path $Portable 'narutorise.version') -Encoding ASCII -Value $Version
    $shaderFiles | Copy-Item -Destination (Join-Path $Portable 'shader_cache')
    $LicenseDir = Join-Path $Portable 'licenses'
    Copy-Item -LiteralPath (Join-Path $RootDir 'LICENSE') -Destination (Join-Path $LicenseDir 'naruto-rise-recomp.txt')
    foreach ($component in $Licenses) {
        Copy-Item -LiteralPath (Join-Path $RootDir $component.File) -Destination (Join-Path $LicenseDir $component.Target)
    }
    $componentText = ($Licenses | ForEach-Object {
        "$($_.Name)`r`n  License: $($_.License) ($($_.Target))`r`n  Source:  $($_.Url)`r`n  Used in: $($_.Usage)"
    }) -join "`r`n`r`n"
    $sourceText = ($SourceRevisions | ForEach-Object { "  $($_.Name)`r`n    $($_.Url)/tree/$($_.Revision)" }) -join "`r`n"
    Set-Content (Join-Path $LicenseDir 'README.txt') -Encoding UTF8 -Value @"
THIRD-PARTY NOTICES - NARUTO: RISE OF A NINJA PC PORT ($Version)

This port's own code is licensed under GPL-3.0-only: naruto-rise-recomp.txt.
It includes the components below, each under its own license; the license texts
are in this folder. Naruto: Rise of a Ninja, its content, artwork and trademarks
belong to their respective owners and are not covered by these licenses. Game
data is not distributed with this port.

$componentText

LGPL SOURCE CODE:
FFmpeg (LGPL-2.1-or-later) and libmspack (LGPL-2.1-only) are statically linked
into rexruntime.dll, which is built from the ReXGlue SDK (BSD-3-Clause). The
complete source code of these libraries and of rexruntime.dll, at the revisions
this release was built from:

$sourceText

You may modify these libraries, rebuild rexruntime.dll from these sources and
replace the rexruntime.dll of this package with it.
"@
    Set-Content (Join-Path $Portable 'README.txt') -Encoding UTF8 -Value @"
NARUTO: RISE OF A NINJA - PC PORT ($Version)

FIRST LAUNCH:
Extract the ZIP to a writable folder and run narutorise.exe.
Choose your original Xbox 360 ISO or the default.xex of a complete extracted
copy. The ISO is read directly; no extraction or installer is needed.
Later launches follow the Show menu at startup preference. Hold Shift while opening, or use
narutorise.exe --setup, to return to the setup screen.

IN GAME:
Press F1 or Back+Start to open Settings, DLC and Exit game.
These shortcuts can be remapped under Controls. The game keeps running while
the menu is open, with game input blocked. Show FPS is a menu option.
Settings include Game, Display, Quality, Controls and Advanced, in seven
interface languages. Graphics options that require a restart are labelled.
Create a desktop shortcut from Settings > Advanced.

UPDATES:
The game notifies you about new releases and opens the release page.
Close the game and extract the new ZIP over the existing package files.
Keep data/ and data_location.json, game/, game_root/ and community mods.
The ZIP contains only defaults/narutorise.toml; your active settings, logs,
saves, mods and DLC packages live in data/. Settings > Advanced shows the active
folder and can select another location after restart. Explicit user_data_root
continues to select its exact save/content folder, without a saves/ suffix.
The first launch copies eligible legacy data and keeps the originals. Back up
the active data folder and any explicit save/content folder before updating.
If import reports a conflict, preserve both copies and resolve the differing
files before retrying. Do not delete .narutorise-migration metadata.
The old narutorise_launcher.exe from a portable copy can be deleted manually.
Registered 0.3.0 installations can use the separately distributed
NarutoRiseInstaller.exe bridge. It replaces shortcuts and removes the launcher
while preserving game data and configuration. Later updates use the ZIP.

TRANSLATIONS:
Brazilian Portuguese is bundled as an immutable seed in defaults\mods\pt-br.
First launch copies it to the active data folder's mods\pt-br if absent,
after importing eligible legacy mods. Select it under Settings > Game > Game
language. Updates and rebuilds preserve active mods, including customized or
partial PT-BR packages. To update a translation, back it up and merge the new
defaults deliberately. The selected language stays in the active TOML.

Microsoft Visual C++ 2015-2022 Redistributable (x64) is required.
Alt+F4 closes the game.

Game files are not included. License and third-party notices: licenses\README.txt.
"@

    $Artifacts = @('naruto-rise-recomp-win-amd64')
    Compress-Archive -Path "$Portable\*" -DestinationPath (Join-Path $Stage 'naruto-rise-recomp-win-amd64.zip')
    $Artifacts += 'naruto-rise-recomp-win-amd64.zip'

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
    if (Test-Path -LiteralPath $Stage) {
        $resolvedStage = (Resolve-Path -LiteralPath $Stage).Path
        $resolvedOutput = (Resolve-Path -LiteralPath $OutputRootDir).Path
        if ((Split-Path $resolvedStage -Parent) -ne $resolvedOutput -or
            (Split-Path $resolvedStage -Leaf) -notlike '.packaging-*') {
            throw "Refusing to remove a staging directory outside the release output: $resolvedStage"
        }
        Remove-Item -LiteralPath $resolvedStage -Recurse -Force -ErrorAction SilentlyContinue
    }
}
