#Requires -Version 5.1
<#
.SYNOPSIS
    Install DLCs and Title Updates for Naruto: Rise of a Ninja (PC port).

.DESCRIPTION
    Organize DLC and Title Update files in the folders expected by the ReXGlue
    SDK / PC port. For STFS packages (.live/.con/.pirs), extract the files with
    an external tool (wxPirs, Xenia, etc.), then use this script to copy the
    extracted files.

.PARAMETER GameRoot
    Game data folder containing default.xex, used for Title Updates.

.PARAMETER UserDataRoot
    Exact guest root used by the game: the user folder inside the active data
    folder shown in Settings > Advanced, or your custom user_data_root. DLC operations require
    an explicit value or REX_USER_DATA_ROOT; Documents is never assumed.

.PARAMETER PrepareDlcFolders
    Create the DLC folder hierarchy in user_data_root.

.PARAMETER Scan
    Scan a folder for STFS packages (.live/.con/.pirs) and list the packages
    that need to be extracted with an external tool.

.PARAMETER InstallLoose
    Copy extracted files from a folder into the DLC tree.

.PARAMETER TitleUpdate
    Copy a folder's contents into title_update/ beside GameRoot.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools/install-content.ps1 `
        -UserDataRoot "C:\Games\Naruto - Rise of a Ninja\data\user" -PrepareDlcFolders

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools/install-content.ps1 `
        -UserDataRoot "C:\Games\Naruto - Rise of a Ninja\data\user" `
        -InstallLoose "C:\Users\You\Downloads\NarutoRiseShikamaru"

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools/install-content.ps1 `
        -GameRoot "C:\Games\Naruto - Rise of a Ninja\game" `
        -TitleUpdate "C:\Users\You\Downloads\NarutoRise_TU2"
#>
[CmdletBinding()]
param(
    [Parameter()]
    [string]$GameRoot = "",

    [Parameter()]
    [string]$UserDataRoot = "",

    [Parameter()]
    [switch]$PrepareDlcFolders,

    [Parameter()]
    [string]$Scan = "",

    [Parameter()]
    [string]$InstallLoose = "",

    [Parameter()]
    [string]$TitleUpdate = ""
)

$ErrorActionPreference = "Stop"

$TitleId = "555307E5"
$MarketplaceContentType = "00000002"
$CommonXuid = "0000000000000000"

function Resolve-GameRoot {
    param([string]$Path)
    if ([string]::IsNullOrWhiteSpace($Path)) {
        # Check the repository and current working directory for game folders.
        $candidates = @(
            (Join-Path $PSScriptRoot "..\game_root"),
            (Join-Path $PSScriptRoot "..\game"),
            (Join-Path (Get-Location) "game_root"),
            (Join-Path (Get-Location) "game")
        )
        foreach ($c in $candidates) {
            $full = Resolve-Path $c -ErrorAction SilentlyContinue
            if ($full -and (Test-Path (Join-Path $full "default.xex"))) {
                return $full.Path
            }
        }
        throw "Could not find the game folder. Specify -GameRoot."
    }
    $full = Resolve-Path $Path -ErrorAction SilentlyContinue
    if (-not $full) { throw "GameRoot does not exist: $Path" }
    if (-not (Test-Path (Join-Path $full.Path "default.xex"))) {
        Write-Warning "GameRoot does not contain default.xex. Continuing anyway..."
    }
    return $full.Path
}

function Get-UserDataRoot {
    param([string]$GameRoot)
    $requestedRoot = $script:UserDataRoot
    if ([string]::IsNullOrWhiteSpace($requestedRoot)) { $requestedRoot = $env:REX_USER_DATA_ROOT }
    if ([string]::IsNullOrWhiteSpace($requestedRoot)) {
        throw "Specify -UserDataRoot with the active guest root. Documents is no longer assumed."
    }
    return [IO.Path]::GetFullPath($requestedRoot)
}

function Get-DlcRoot {
    param([string]$UserDataRoot)
    return Join-Path $UserDataRoot "$CommonXuid\$TitleId\$MarketplaceContentType"
}

function Test-StfsMagic {
    param([string]$FilePath)
    try {
        $fs = [System.IO.File]::OpenRead($FilePath)
        $magic = New-Object byte[] 4
        $fs.Read($magic, 0, 4) | Out-Null
        $fs.Close()
        $s = [BitConverter]::ToString($magic) -replace '-', ''
        # CON, LIVE, PIRS in big-endian order
        return $s -in @("434F4E20", "4C495645", "50495253")
    } catch {
        return $false
    }
}

function Invoke-PrepareDlcFolders {
    param([string]$GameRoot)
    $userRoot = Get-UserDataRoot -GameRoot $GameRoot
    $dlcRoot = Get-DlcRoot -UserDataRoot $userRoot
    New-Item -ItemType Directory -Path $dlcRoot -Force | Out-Null
    Write-Host "DLC folder tree created at: $dlcRoot" -ForegroundColor Green
    Write-Host "Place extracted DLC folders here (one folder per DLC)." -ForegroundColor Cyan
}

function Invoke-Scan {
    param([string]$Folder)
    if (-not (Test-Path $Folder)) { throw "Scan folder does not exist: $Folder" }
    $files = Get-ChildItem -Path $Folder -Recurse -File
    $found = @()
    foreach ($f in $files) {
        if ($f.Length -lt 0x344) { continue }
        if (Test-StfsMagic -FilePath $f.FullName) {
            $found += $f.FullName
        }
    }
    if ($found.Count -eq 0) {
        Write-Host "No STFS packages found in $Folder." -ForegroundColor Yellow
    } else {
        Write-Host "STFS packages found (extract with an external tool):" -ForegroundColor Cyan
        $found | ForEach-Object { Write-Host "  $_" }
        Write-Host ""
        Write-Host "Use wxPirs, Xenia or another tool to extract these files." -ForegroundColor Cyan
        Write-Host "Use Add DLC in the menu, or -InstallLoose with -UserDataRoot set to the active guest root." -ForegroundColor Cyan
    }
}

function Invoke-InstallLoose {
    param(
        [string]$GameRoot,
        [string]$Source
    )
    if (-not (Test-Path $Source)) { throw "Source folder does not exist: $Source" }
    $userRoot = Get-UserDataRoot -GameRoot $GameRoot
    $dlcRoot = Get-DlcRoot -UserDataRoot $userRoot
    New-Item -ItemType Directory -Path $dlcRoot -Force | Out-Null

    $sourceName = (Get-Item $Source).Name
    $dest = Join-Path $dlcRoot $sourceName

    Write-Host "Copying '$Source' to '$dest' ..." -ForegroundColor Cyan
    if (Test-Path $dest) {
        Write-Warning "Destination already exists. Merging..."
    } else {
        New-Item -ItemType Directory -Path $dest -Force | Out-Null
    }

    Get-ChildItem -Path $Source -Recurse | ForEach-Object {
        $target = $_.FullName.Replace($Source, $dest)
        if ($_.PSIsContainer) {
            New-Item -ItemType Directory -Path $target -Force | Out-Null
        } else {
            $parent = Split-Path -Parent $target
            if (-not (Test-Path $parent)) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }
            Copy-Item -Path $_.FullName -Destination $target -Force
        }
    }

    Write-Host "DLC installed at: $dest" -ForegroundColor Green
}

function Invoke-TitleUpdate {
    param(
        [string]$GameRoot,
        [string]$Source
    )
    if (-not (Test-Path $Source)) { throw "Source folder does not exist: $Source" }
    $tuDest = Join-Path (Split-Path -Parent $GameRoot) "title_update"
    New-Item -ItemType Directory -Path $tuDest -Force | Out-Null

    Write-Host "Copying Title Update from '$Source' to '$tuDest' ..." -ForegroundColor Cyan
    Get-ChildItem -Path $Source -Recurse | ForEach-Object {
        $target = $_.FullName.Replace($Source, $tuDest)
        if ($_.PSIsContainer) {
            New-Item -ItemType Directory -Path $target -Force | Out-Null
        } else {
            $parent = Split-Path -Parent $target
            if (-not (Test-Path $parent)) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }
            Copy-Item -Path $_.FullName -Destination $target -Force
        }
    }

    Write-Host "Title Update installed at: $tuDest" -ForegroundColor Green
    Write-Host "The port will detect 'title_update/' beside 'game/' on the next launch." -ForegroundColor Cyan
}

# --- main ---
if (-not $PrepareDlcFolders -and [string]::IsNullOrWhiteSpace($Scan) -and
    [string]::IsNullOrWhiteSpace($InstallLoose) -and [string]::IsNullOrWhiteSpace($TitleUpdate)) {
    Get-Help $MyInvocation.MyCommand.Path -Detailed
    exit 0
}

$resolvedGameRoot = $null
if (-not [string]::IsNullOrWhiteSpace($TitleUpdate)) {
    $resolvedGameRoot = Resolve-GameRoot -Path $GameRoot
    Write-Host "Game data: $resolvedGameRoot" -ForegroundColor Cyan
}

if ($PrepareDlcFolders) {
    Invoke-PrepareDlcFolders -GameRoot $resolvedGameRoot
}

if (-not [string]::IsNullOrWhiteSpace($Scan)) {
    Invoke-Scan -Folder $Scan
}

if (-not [string]::IsNullOrWhiteSpace($InstallLoose)) {
    Invoke-InstallLoose -GameRoot $resolvedGameRoot -Source $InstallLoose
}

if (-not [string]::IsNullOrWhiteSpace($TitleUpdate)) {
    Invoke-TitleUpdate -GameRoot $resolvedGameRoot -Source $TitleUpdate
}
