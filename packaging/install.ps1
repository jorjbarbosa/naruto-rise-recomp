# Naruto: Rise of a Ninja - Shortcut Creator (Windows)
# Running this script creates shortcuts on your Desktop and Start Menu

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$LauncherExe = Join-Path $ScriptDir "narutorise_launcher.exe"

if (-not (Test-Path $LauncherExe)) {
    Write-Host "[ERROR] narutorise_launcher.exe not found in: $ScriptDir" -ForegroundColor Red
    pause
    exit 1
}

$WshShell = New-Object -ComObject WScript.Shell

# Desktop shortcut
$DesktopPath = [System.Environment]::GetFolderPath([System.Environment+SpecialFolder]::Desktop)
$ShortcutDesktop = $WshShell.CreateShortcut((Join-Path $DesktopPath "Naruto - Rise of a Ninja.lnk"))
$ShortcutDesktop.TargetPath = $LauncherExe
$ShortcutDesktop.WorkingDirectory = $ScriptDir
$ShortcutDesktop.Description = "Naruto: Rise of a Ninja PC Port"
$ShortcutDesktop.IconLocation = "$LauncherExe,0"
$ShortcutDesktop.Save()

# Start Menu shortcut
$ProgramsPath = [System.Environment]::GetFolderPath([System.Environment+SpecialFolder]::Programs)
$StartMenuDir = Join-Path $ProgramsPath "Naruto Rise of a Ninja"
if (-not (Test-Path $StartMenuDir)) {
    New-Item -ItemType Directory -Path $StartMenuDir | Out-Null
}
$ShortcutStart = $WshShell.CreateShortcut((Join-Path $StartMenuDir "Naruto - Rise of a Ninja.lnk"))
$ShortcutStart.TargetPath = $LauncherExe
$ShortcutStart.WorkingDirectory = $ScriptDir
$ShortcutStart.Description = "Naruto: Rise of a Ninja PC Port"
$ShortcutStart.IconLocation = "$LauncherExe,0"
$ShortcutStart.Save()

Write-Host "==========================================================" -ForegroundColor Green
Write-Host " Shortcuts created successfully!" -ForegroundColor Green
Write-Host " - Desktop: Naruto - Rise of a Ninja.lnk" -ForegroundColor Cyan
Write-Host " - Start Menu: Naruto Rise of a Ninja" -ForegroundColor Cyan
Write-Host "==========================================================" -ForegroundColor Green
Write-Host ""
Write-Host "To play, run the Launcher or place your extracted game files (default.xex) in 'game'." -ForegroundColor Yellow
Write-Host ""
Start-Sleep -Seconds 3
