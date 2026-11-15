# Shared helpers for the build scripts. Dot-source it: . "$PSScriptRoot/common.ps1"

$RepoRoot = Split-Path -Parent $PSScriptRoot

function Fail([string]$message) {
    Write-Host "error: $message" -ForegroundColor Red
    exit 1
}

# clang-cl picks the C++ headers of the newest Visual Studio when INCLUDE is not set, which may not
# match its own version. Unless the script already runs inside a developer prompt, load the developer
# environment of the newest Visual Studio that ships clang-cl, so compiler and headers always match.
function Use-VsDevEnvironment {
    if (-not $env:VSCMD_VER) {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
        if (-not (Test-Path $vswhere)) {
            Fail 'Visual Studio was not found (vswhere.exe is missing)'
        }
        $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Llvm.Clang -property installationPath
        if (-not $vs) {
            Fail 'no Visual Studio installation with "C++ Clang tools for Windows" was found'
        }
        $vcvars = Join-Path $vs 'VC/Auxiliary/Build/vcvars64.bat'
        Write-Host "using the developer environment of $vs"
        foreach ($line in (cmd /c "`"$vcvars`" >nul 2>nul && set")) {
            if ($line -match '^([^=]+)=(.*)$') {
                Set-Item -Path "env:$($Matches[1])" -Value $Matches[2]
            }
        }
        # Without this, a clang-cl from another install could be found first on PATH.
        $env:PATH = (Join-Path $vs 'VC/Tools/Llvm/x64/bin') + ';' + $env:PATH
    }
    foreach ($tool in 'clang-cl', 'lld-link', 'cmake', 'ninja') {
        if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) { Fail "$tool not found" }
    }
}

# Configures (once) and builds a CMake project with clang-cl, lld-link and Ninja.
function Invoke-CMakeBuild([string]$source, [string]$build, [string[]]$targets) {
    if (-not (Test-Path (Join-Path $build 'CMakeCache.txt'))) {
        $clangCl = (Get-Command clang-cl).Source
        $lldLink = (Get-Command lld-link).Source
        cmake -S $source -B $build -G Ninja -DCMAKE_BUILD_TYPE=Release `
            "-DCMAKE_C_COMPILER=$clangCl" "-DCMAKE_CXX_COMPILER=$clangCl" "-DCMAKE_LINKER=$lldLink"
        if ($LASTEXITCODE -ne 0) { Fail "CMake configuration of $source failed" }
    }
    $buildArgs = @('--build', $build)
    if ($targets) { $buildArgs += '--target'; $buildArgs += $targets }
    cmake @buildArgs
    if ($LASTEXITCODE -ne 0) { Fail "build of $source failed" }
}
