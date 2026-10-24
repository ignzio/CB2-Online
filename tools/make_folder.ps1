param(
    [string]$Game = "",
    [string]$Out = "dist\Crash2"
)
# Builds the game folder: the windowed player (Crash2.exe, with the crate icon), your game data in data\, the settings file and
# launchers for playing alone, hosting and joining. Run after
#   cmake --build build --config Release --target cb2_game
# The folder contains your own game files; it is for your own use, do not share it.
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if (-not $Game) { $Game = (Resolve-Path (Join-Path $root "..")).Path }
$player = Join-Path $root "build\Release\cb2_game.exe"
$server = Join-Path $root "build\Release\cb2_server.exe"
foreach ($f in $player, $server) { if (-not (Test-Path $f)) { throw "missing $f - build the cb2_game and cb2_server targets first" } }
$out = Join-Path $root $Out
New-Item -ItemType Directory -Force $out | Out-Null
Get-ChildItem $out -File | Where-Object { $_.Name -ne "memcard0.mcr" -and $_.Name -ne "cb2.cfg" } | Remove-Item -Force

Copy-Item $player (Join-Path $out "Crash2.exe") -Force
Copy-Item $server (Join-Path $out "Crash2 Server.exe") -Force
Set-Content (Join-Path $out "FONT-LICENSE.txt") -Encoding ascii -Value "The online menu uses the font Crash-a-Like by Aryel Filipe (https://www.dafont.com/crash-a-like.font), listed on dafont.com as 100% Free."
$data = Join-Path $out "data"
New-Item -ItemType Directory -Force $data | Out-Null
foreach ($f in "S0", "S1", "S2", "S3", "SCUS_941.54", "SYSTEM.CNF") {
    $src = Join-Path $Game $f
    if (Test-Path $src) { Copy-Item $src (Join-Path $data $f) -Force } elseif ($f -ne "SYSTEM.CNF") { throw "missing $src" }
}
Get-ChildItem $Game -File | Where-Object { $_.Name -match '^(CRASH|S\d+|SCUS|SYSTEM)' -and -not (Test-Path (Join-Path $data $_.Name)) } |
    ForEach-Object { Copy-Item $_.FullName (Join-Path $data $_.Name) -Force }

if (-not (Test-Path (Join-Path $out "cb2.cfg"))) {
@"
# Crash 2 settings. Command-line options override these.
game = data
# name = YourName          name shown over your Crash in online play (default: your Windows user name)
# room = PUBLIC            online room: players who type the same room meet (no host, no addresses)
# The lobby service and relay server are built into Crash2.exe (see CMakeLists.txt); these lines only override them:
# rendezvous = 1.2.3.4     relay server for players who cannot connect directly
# directory =              (empty) turns the online lobby service off
# lobby = off              do not show the online lobby before the game
scale = 4
"@ | Set-Content (Join-Path $out "cb2.cfg") -Encoding ascii
}

function Bat($name, $lines) { Set-Content (Join-Path $out $name) -Value (@("@echo off", 'cd /d "%~dp0"') + $lines) -Encoding ascii }
Bat "Play.bat" @('start "" "Crash2.exe"')
Bat "Play offline (no lobby).bat" @('start "" "Crash2.exe" --no-lobby')
Bat "Start rendezvous server.bat" @('echo Players put this machine address as rendezvous in cb2.cfg (or in the lobby). Close this window to stop the server.', '"Crash2 Server.exe"', 'pause')
"Game folder ready: $out"
