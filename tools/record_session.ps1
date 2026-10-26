# Plays the game offline from the source build and records the scenery data of the session.
# Usage: powershell -ExecutionPolicy Bypass -File tools\record_session.ps1 [-Out session.txt]
param([string]$Out = "session.txt")

$root = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $root "build\Release\cb2_play.exe"
$game = Join-Path $root "dist\Crash2\data"
if (-not (Test-Path $exe)) { throw "Build first: $exe not found" }
if (-not (Test-Path (Join-Path $game "SCUS_941.54"))) { throw "Game files not found in $game" }

$env:CB2_SESSION = (Join-Path (Get-Location) $Out)
Write-Host "Recording to $env:CB2_SESSION - play, then close the game window to finish."
Push-Location (Split-Path $exe)
& $exe --game $game --no-lobby
Pop-Location
Remove-Item Env:CB2_SESSION
Write-Host "Done: $Out ($([int]((Get-Item $Out).Length / 1KB)) KB)"
