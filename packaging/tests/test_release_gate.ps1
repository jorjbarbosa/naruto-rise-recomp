# Exercise rejection of an otherwise configured build before binaries or release
# artifacts are touched. CMake configuration is the only external service stub.
$ErrorActionPreference = 'Stop'
$Fixture = Join-Path ([IO.Path]::GetTempPath()) ('narutorise-release-gate-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path "$Fixture/packaging", "$Fixture/mods/pt-br", "$Fixture/app/out/build/win-amd64-release", "$Fixture/release" -Force | Out-Null
try {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot '../package-release.ps1') -Destination "$Fixture/packaging/package-release.ps1"
    Set-Content -LiteralPath "$Fixture/packaging/version.iss" -Value '#define MyAppVersion "0.3.0-beta"'
    Set-Content -LiteralPath "$Fixture/mods/pt-br/mod.toml" -Value 'fixture'
    Set-Content -LiteralPath "$Fixture/mods/pt-br/texts.json" -Value '{}'
    Set-Content -LiteralPath "$Fixture/app/out/build/win-amd64-release/CMakeCache.txt" -Value 'NARUTORISE_NATIVE_RENDERER_BOOTSTRAP:BOOL=OFF'
    $Previous = [byte[]](10, 20, 30, 40)
    [IO.File]::WriteAllBytes("$Fixture/release/naruto-rise-recomp-win-amd64.zip", $Previous)
    $ConfigureCalls = [Collections.Generic.List[string]]::new()
    function cmake {
        if ($args -contains '--build') { throw 'The rejected package attempted a build.' }
        $ConfigureCalls.Add('configure')
        $global:LASTEXITCODE = 0
    }
    $Rejected = $false
    try {
        & "$Fixture/packaging/package-release.ps1" -OutputRootDir "$Fixture/release"
    } catch {
        if ($_.Exception.Message -notlike '*ZIP requires the native setup screen*') { throw }
        $Rejected = $true
    }
    if (-not $Rejected -or $ConfigureCalls.Count -ne 1) { throw 'A build without native setup was accepted.' }
    if ([Convert]::ToBase64String([IO.File]::ReadAllBytes("$Fixture/release/naruto-rise-recomp-win-amd64.zip")) -ne
        [Convert]::ToBase64String($Previous)) { throw 'An existing release changed.' }
    if (@(Get-ChildItem -LiteralPath "$Fixture/release").Count -ne 1) { throw 'The rejected package staged artifacts.' }
    Write-Output 'PASS: native OFF rejected before build/staging; previous release preserved'
} finally {
    if (Test-Path -LiteralPath $Fixture) {
        $Resolved = (Resolve-Path -LiteralPath $Fixture).Path
        $TempRoot = (Resolve-Path -LiteralPath ([IO.Path]::GetTempPath())).Path.TrimEnd('\', '/')
        if ((Split-Path $Resolved -Parent).TrimEnd('\', '/') -ne $TempRoot -or
            (Split-Path $Resolved -Leaf) -notlike 'narutorise-release-gate-*') { throw "Unsafe fixture path: $Resolved" }
        Remove-Item -LiteralPath $Resolved -Recurse -Force
    }
}
