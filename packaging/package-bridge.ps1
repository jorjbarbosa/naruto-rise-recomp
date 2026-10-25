# Optional compatibility asset for launchers that can only update through Inno.
param (
    [string]$PackageDir = "",
    [string]$OutputRootDir = "",
    [string]$IsccPath = ""
)

$ErrorActionPreference = 'Stop'
$RootDir = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if (-not $OutputRootDir) { $OutputRootDir = Join-Path $RootDir 'dist' }
$OutputRootDir = [IO.Path]::GetFullPath($OutputRootDir)
if (-not $PackageDir) { $PackageDir = Join-Path $OutputRootDir 'naruto-rise-recomp-win-amd64' }
$PackageDir = (Resolve-Path -LiteralPath $PackageDir).Path
foreach ($name in @('narutorise.exe', 'rexruntime.dll', 'narutorise.toml', 'narutorise.version', 'licenses\README.txt')) {
    if (-not (Test-Path -LiteralPath (Join-Path $PackageDir $name) -PathType Leaf)) {
        throw "Missing package file: $name. Run package-release.ps1 first."
    }
}
foreach ($name in @('narutorise_launcher.exe', 'narutorise_setup_helper.exe', 'extract-xiso.exe', 'install.ps1')) {
    if (Test-Path -LiteralPath (Join-Path $PackageDir $name)) { throw "Obsolete file in bridge payload: $name" }
}
# The bridge must have exactly the version of its ZIP, even for -Version builds.
$Version = (Get-Content -LiteralPath (Join-Path $PackageDir 'narutorise.version') -Raw).Trim()
if ($Version -notmatch '^\d+\.\d+\.\d+([-+][A-Za-z0-9.-]+)?$') { throw 'Invalid package version.' }
if (-not $IsccPath) {
    $compiler = Get-Command ISCC.exe -ErrorAction SilentlyContinue
    if ($compiler) { $IsccPath = $compiler.Source }
    foreach ($base in @(${env:ProgramFiles(x86)}, $env:ProgramFiles, $env:LOCALAPPDATA)) {
        if (-not $base) { continue }
        foreach ($sub in @('Inno Setup 6\ISCC.exe', 'Programs\Inno Setup 6\ISCC.exe')) {
            $candidate = Join-Path $base $sub
            if (-not $IsccPath -and (Test-Path -LiteralPath $candidate -PathType Leaf)) { $IsccPath = $candidate }
        }
    }
}
if (-not $IsccPath -or -not (Test-Path -LiteralPath $IsccPath -PathType Leaf)) {
    throw 'Install Inno Setup 6 or pass -IsccPath to build the optional bridge.'
}
$Stage = Join-Path $OutputRootDir ('.bridge-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $Stage -Force | Out-Null
try {
    & $IsccPath "/DPackageDir=$PackageDir" "/DMyAppVersion=$Version" "/O$Stage" (Join-Path $PSScriptRoot 'installer.iss')
    if ($LASTEXITCODE -ne 0) { throw "Bridge compilation failed ($LASTEXITCODE)." }
    $BridgeName = 'NarutoRiseInstaller.exe'
    $Bridge = Join-Path $Stage $BridgeName
    if (-not (Test-Path -LiteralPath $Bridge -PathType Leaf) -or (Get-Item -LiteralPath $Bridge).Length -eq 0) {
        throw 'Bridge output missing.'
    }
    $Destination = Join-Path $OutputRootDir $BridgeName
    $Backup = Join-Path $OutputRootDir ('previous-bridge-' + [guid]::NewGuid().ToString('N') + '.exe')
    $HadPrevious = Test-Path -LiteralPath $Destination
    if ($HadPrevious) { Move-Item -LiteralPath $Destination -Destination $Backup }
    try {
        Move-Item -LiteralPath $Bridge -Destination $Destination
    } catch {
        if ($HadPrevious) { Move-Item -LiteralPath $Backup -Destination $Destination }
        throw
    }
    Write-Host "Compatibility bridge $Version created in $OutputRootDir" -ForegroundColor Green
} finally {
    if (Test-Path -LiteralPath $Stage) {
        $resolvedStage = (Resolve-Path -LiteralPath $Stage).Path
        if ((Split-Path $resolvedStage -Parent) -ne (Resolve-Path -LiteralPath $OutputRootDir).Path -or
            (Split-Path $resolvedStage -Leaf) -notlike '.bridge-*') {
            throw "Refusing to remove a staging directory outside the bridge output: $resolvedStage"
        }
        Remove-Item -LiteralPath $resolvedStage -Recurse -Force -ErrorAction SilentlyContinue
    }
}
