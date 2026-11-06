# Plays a silent WAV through the WinRT MediaPlayer for a while: a media session
# that the system's media transport controls report as playing, without adding
# sound to a loopback recording. The trigger of the music attenuation test
# (audio_live_boot_test.py). Windows PowerShell 5.1 (WinRT types).
param([int]$Seconds = 15)
$ErrorActionPreference = 'Stop'
$wav = Join-Path $env:TEMP 'narutorise-silence.wav'
if (-not (Test-Path $wav)) {
    $rate = 48000; $seconds = 5; $data = $rate * $seconds * 4
    $writer = New-Object IO.BinaryWriter([IO.File]::Create($wav))
    $writer.Write([Text.Encoding]::ASCII.GetBytes('RIFF')); $writer.Write([int](36 + $data))
    $writer.Write([Text.Encoding]::ASCII.GetBytes('WAVEfmt ')); $writer.Write([int]16)
    $writer.Write([int16]1); $writer.Write([int16]2); $writer.Write([int]$rate); $writer.Write([int]($rate * 4))
    $writer.Write([int16]4); $writer.Write([int16]16)
    $writer.Write([Text.Encoding]::ASCII.GetBytes('data')); $writer.Write([int]$data)
    $writer.Write((New-Object byte[] $data)); $writer.Close()
}
Add-Type -AssemblyName System.Runtime.WindowsRuntime
$null = [Windows.Media.Playback.MediaPlayer, Windows.Media.Playback, ContentType = WindowsRuntime]
$null = [Windows.Media.Core.MediaSource, Windows.Media.Core, ContentType = WindowsRuntime]
$player = New-Object Windows.Media.Playback.MediaPlayer
$player.IsLoopingEnabled = $true
$player.Source = [Windows.Media.Core.MediaSource]::CreateFromUri([Uri]$wav)
$player.Play()
Start-Sleep -Seconds $Seconds
$player.Pause()
$player.Dispose()
