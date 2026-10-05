#Requires -Version 5.1
<#
.SYNOPSIS
    Auxilia na instalação de DLCs e Title Updates para Naruto: Rise of a Ninja (PC port).

.DESCRIPTION
    Este script organiza arquivos de DLC e Title Updates nas pastas esperadas
    pelo ReXGlue SDK / port de PC. Ele NÃO extrai pacotes STFS sozinho; para
    arquivos .live/.con/.pirs, use uma ferramenta externa (wxPirs, Xenia, etc.)
    e depois utilize este script para copiar os arquivos soltos.

.PARAMETER GameRoot
    Pasta dos dados do jogo (onde fica default.xex). Usada para descobrir
    user_data_root quando o launcher/TOML usam o padrão.

.PARAMETER PrepareDlcFolders
    Cria a hierarquia de pastas para DLCs em user_data_root.

.PARAMETER Scan
    Varre uma pasta em busca de pacotes STFS (.live/.con/.pirs) e lista o que
    encontrou, alertando que precisam ser extraídos externamente.

.PARAMETER InstallLoose
    Copia arquivos soltos (já extraídos) de uma pasta para a estrutura de DLC.

.PARAMETER TitleUpdate
    Copia o conteúdo de uma pasta para title_update/ ao lado de GameRoot.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools/install-content.ps1 `
        -GameRoot "C:\Games\Naruto - Rise of a Ninja\game" -PrepareDlcFolders

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools/install-content.ps1 `
        -GameRoot "C:\Games\Naruto - Rise of a Ninja\game" `
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
        # Tenta descobrir a partir do diretório atual ou de game_root/ ao lado.
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
        throw "Não foi possível encontrar a pasta do jogo. Especifique -GameRoot."
    }
    $full = Resolve-Path $Path -ErrorAction SilentlyContinue
    if (-not $full) { throw "GameRoot não existe: $Path" }
    if (-not (Test-Path (Join-Path $full.Path "default.xex"))) {
        Write-Warning "GameRoot não contém default.xex. Continuando assim mesmo..."
    }
    return $full.Path
}

function Get-UserDataRoot {
    param([string]$GameRoot)
    # ReXApp usa GetUserFolder() / "narutorise". No Windows, GetUserFolder()
    # tipicamente retorna Documents.
    $documents = [Environment]::GetFolderPath("MyDocuments")
    return Join-Path $documents "narutorise"
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
        # CON, LIVE, PIRS em big-endian
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
    Write-Host "Estrutura de DLC criada em: $dlcRoot" -ForegroundColor Green
    Write-Host "Coloque aqui as pastas dos DLCs já extraídos (uma pasta por DLC)." -ForegroundColor Cyan
}

function Invoke-Scan {
    param([string]$Folder)
    if (-not (Test-Path $Folder)) { throw "Pasta de scan não existe: $Folder" }
    $files = Get-ChildItem -Path $Folder -Recurse -File
    $found = @()
    foreach ($f in $files) {
        if ($f.Length -lt 0x344) { continue }
        if (Test-StfsMagic -FilePath $f.FullName) {
            $found += $f.FullName
        }
    }
    $defaultDlcRoot = Get-DlcRoot -UserDataRoot (Join-Path ([Environment]::GetFolderPath("MyDocuments")) "narutorise")
    if ($found.Count -eq 0) {
        Write-Host "Nenhum pacote STFS encontrado em $Folder." -ForegroundColor Yellow
    } else {
        Write-Host "Pacotes STFS encontrados (precisam ser extraídos externamente):" -ForegroundColor Cyan
        $found | ForEach-Object { Write-Host "  $_" }
        Write-Host ""
        Write-Host "Dica: use wxPirs, Xenia ou outra ferramenta para extrair esses arquivos." -ForegroundColor Cyan
        Write-Host "Depois, coloque as pastas extraídas em: $defaultDlcRoot" -ForegroundColor Cyan
    }
}

function Invoke-InstallLoose {
    param(
        [string]$GameRoot,
        [string]$Source
    )
    if (-not (Test-Path $Source)) { throw "Pasta de origem não existe: $Source" }
    $userRoot = Get-UserDataRoot -GameRoot $GameRoot
    $dlcRoot = Get-DlcRoot -UserDataRoot $userRoot
    New-Item -ItemType Directory -Path $dlcRoot -Force | Out-Null

    $sourceName = (Get-Item $Source).Name
    $dest = Join-Path $dlcRoot $sourceName

    Write-Host "Copiando '$Source' para '$dest' ..." -ForegroundColor Cyan
    if (Test-Path $dest) {
        Write-Warning "Destino já existe. Mesclando..."
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

    Write-Host "DLC instalado em: $dest" -ForegroundColor Green
}

function Invoke-TitleUpdate {
    param(
        [string]$GameRoot,
        [string]$Source
    )
    if (-not (Test-Path $Source)) { throw "Pasta de origem não existe: $Source" }
    $tuDest = Join-Path (Split-Path -Parent $GameRoot) "title_update"
    New-Item -ItemType Directory -Path $tuDest -Force | Out-Null

    Write-Host "Copiando Title Update de '$Source' para '$tuDest' ..." -ForegroundColor Cyan
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

    Write-Host "Title Update instalado em: $tuDest" -ForegroundColor Green
    Write-Host "O port detectará automaticamente 'title_update/' ao lado de 'game/' na próxima execução." -ForegroundColor Cyan
}

# --- main ---
if (-not $PrepareDlcFolders -and [string]::IsNullOrWhiteSpace($Scan) -and
    [string]::IsNullOrWhiteSpace($InstallLoose) -and [string]::IsNullOrWhiteSpace($TitleUpdate)) {
    Get-Help $MyInvocation.MyCommand.Path -Detailed
    exit 0
}

$resolvedGameRoot = $null
if ($PrepareDlcFolders -or -not [string]::IsNullOrWhiteSpace($InstallLoose) -or
    -not [string]::IsNullOrWhiteSpace($TitleUpdate)) {
    $resolvedGameRoot = Resolve-GameRoot -Path $GameRoot
    Write-Host "Dados do jogo: $resolvedGameRoot" -ForegroundColor Cyan
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
