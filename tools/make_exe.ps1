param(
    [string]$Game = "",
    [string]$Out = "dist\Crash2.exe"
)
# Builds the single-file game: the windowed player (with icon) plus your game data appended to it. Run after
#   cmake --build build --config Release --target cb2_game cb2_pack
# The result contains your own game files; it is for your own use, do not share it.
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if (-not $Game) { $Game = (Resolve-Path (Join-Path $root "..")).Path }
$player = Join-Path $root "build\Release\cb2_game.exe"
$pack = Join-Path $root "build\Release\cb2_pack.exe"
foreach ($f in $player, $pack) { if (-not (Test-Path $f)) { throw "missing $f - build the cb2_game and cb2_pack targets first" } }
$out = Join-Path $root $Out
New-Item -ItemType Directory -Force (Split-Path -Parent $out) | Out-Null
& $pack $player $Game $out
if ($LASTEXITCODE -ne 0) { throw "cb2_pack failed" }
