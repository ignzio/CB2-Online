param(
    [switch]$Publish,
    [string]$Notes = "",
    [string]$Repo = "ignzio/CB2-Online"
)
# Builds the files of a GitHub release from the built executables (no game data is ever included) and, with -Publish, creates the release
# and uploads them. The version is the VERSION file; the release tag is v<VERSION>. Steps (docs/RELEASING.md):
#   1. bump VERSION, commit and push to main
#   2. cmake --build build --config Release --target cb2_game cb2_server      (from a tree that has local/directory.txt, so the lobby service is built in)
#   3. powershell -File tools\make_release.ps1 -Publish -Notes "what changed"
# Assets:  CB2-Online-v<VERSION>.zip   first install (no game data: the player puts their own files into data\)
#          Crash2.exe, Crash2-Server.exe   what the in-game updater downloads (it checks them against the SHA-256 GitHub lists)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$version = (Get-Content (Join-Path $root "VERSION") -TotalCount 1).Trim()
$tag = "v$version"
$player = Join-Path $root "build\Release\cb2_game.exe"
$server = Join-Path $root "build\Release\cb2_server.exe"
foreach ($f in $player, $server) { if (-not (Test-Path $f)) { throw "missing $f - build the cb2_game and cb2_server targets first" } }

$out = Join-Path $root "dist\release"
if (Test-Path $out) { Remove-Item $out -Recurse -Force }
$pack = Join-Path $out "CB2-Online-$tag"
New-Item -ItemType Directory -Force (Join-Path $pack "data") | Out-Null
Copy-Item $player (Join-Path $out "Crash2.exe")
Copy-Item $server (Join-Path $out "Crash2-Server.exe")
Copy-Item $player (Join-Path $pack "Crash2.exe")
Copy-Item $server (Join-Path $pack "Crash2 Server.exe")
Copy-Item (Join-Path $root "LICENSE") $pack -ErrorAction SilentlyContinue
Set-Content (Join-Path $pack "FONT-LICENSE.txt") -Encoding ascii -Value "The online menu uses the font Crash-a-Like by Aryel Filipe (https://www.dafont.com/crash-a-like.font), listed on dafont.com as 100% Free."
Set-Content (Join-Path $pack "data\GAME FILES GO HERE.txt") -Encoding ascii -Value @"
CB2-Online does not include the game. Start Play.bat: the first time, it asks for your own Crash Bandicoot 2 disc image, European (PAL) or
American (NTSC-U) (.cue/.bin or .iso), and copies the files it needs into the data-pal (European) or data (American) folder.
Or copy SCES_009.67 (European) or SCUS_941.54 (American), SYSTEM.CNF and the S0..S3 folders there yourself.
"@
Set-Content (Join-Path $pack "cb2.cfg") -Encoding ascii -Value @"
# CB2-Online settings. Command-line options override these.
game = data
# name = YourName          name shown over your Crash in online play (default: your Windows user name)
# update = off             do not look for a newer version when the lobby opens
scale = 4
"@
function Bat($name, $lines) { Set-Content (Join-Path $pack $name) -Value (@("@echo off", 'cd /d "%~dp0"') + $lines) -Encoding ascii }
Bat "Play.bat" @('start "" "Crash2.exe"')
Bat "Play offline (no lobby).bat" @('start "" "Crash2.exe" --no-lobby')
Bat "Start rendezvous server.bat" @('echo Players put this machine address as rendezvous in cb2.cfg (or in the lobby). Close this window to stop the server.', '"Crash2 Server.exe"', 'pause')
$zip = Join-Path $out "CB2-Online-$tag.zip"
Compress-Archive -Path $pack -DestinationPath $zip -Force
Remove-Item $pack -Recurse -Force
Get-ChildItem $out | Select-Object Name, Length | Format-Table -AutoSize | Out-String | Write-Host
if (-not $Publish) { "Files for $tag are in $out (add -Publish to create the GitHub release)"; return }

$env:GIT_TERMINAL_PROMPT = "0"
$cred = "protocol=https`nhost=github.com`n`n" | git credential fill
$token = (($cred | Where-Object { $_ -like "password=*" }) -replace "^password=", "")
if (-not $token) { throw "no GitHub login found in the Git credential manager" }
$headers = @{ Authorization = "Bearer $token"; "User-Agent" = "cb2-release"; Accept = "application/vnd.github+json" }
$body = @{ tag_name = $tag; target_commitish = "main"; name = "CB2-Online $version"; body = $(if ($Notes) { $Notes } else { "CB2-Online $version" }); draft = $false; prerelease = $false } | ConvertTo-Json
$release = Invoke-RestMethod -Method Post "https://api.github.com/repos/$Repo/releases" -Headers $headers -Body $body -ContentType "application/json"
foreach ($f in Get-ChildItem $out -File) {
    $url = "https://uploads.github.com/repos/$Repo/releases/$($release.id)/assets?name=$([uri]::EscapeDataString($f.Name))"
    $null = Invoke-RestMethod -Method Post $url -Headers $headers -InFile $f.FullName -ContentType "application/octet-stream"
    "uploaded $($f.Name)"
}
"Published: $($release.html_url)"
