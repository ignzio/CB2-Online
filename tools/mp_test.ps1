# Two game instances on this machine in the same room, each with a scripted pad (skip the cutscenes, walk into the Turtle Woods portal);
# each saves what its window shows. Nobody types an address: without -Server they find each other on this PC by themselves; with
# -Server host:port they are introduced by a rendezvous server (cb2_server) and LAN probing is switched off.
# Usage: powershell -File tools\mp_test.ps1 [-Scale 2] [-Server 127.0.0.1:7790]
param([int]$Scale = 2, [string]$Server = "")
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
$exe = "$root\build\Release\cb2_play.exe"
$game = (Resolve-Path "..").Path
function Launch($name, $extra, $shot) {
    $argList = @("`"$game`"", '--hidden', '--scale', "$Scale", '--autostart', '--pace-from', '4300', '--press', '4000-4004:start', '--press', '5500-5504:start', '--window-capture', '--capture-after-puppet', '150',
                 '--capture', "9000:$shot", '--name', $name) + $extra
    Start-Process -FilePath $exe -ArgumentList $argList -PassThru -NoNewWindow -RedirectStandardOutput "$root\analysis\mp_$name.log" -RedirectStandardError "$root\analysis\mp_$name.err"
}
# Alice stands still; Bob runs right and jumps, so they stand in different places and poses.
$room = "MPTEST" + (Get-Random -Maximum 9999)
$net = @('--online', '--room', $room)
if ($Server) { $net += @('--rendezvous', $Server); $env:CB2_NOLAN = '1' }
$a = Launch 'ALICE' ($net + @('--press', '6200-6700:up')) 'analysis\mp_alice.png'
$b = Launch 'BOB' ($net + @('--press', '6300-6800:up')) 'analysis\mp_bob.png'
$limit = 420000
$null = $a.WaitForExit($limit)
$null = $b.WaitForExit(30000)
foreach ($p in $a, $b) { if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force; "killed pid $($p.Id) (timeout)" } }
foreach ($n in 'ALICE', 'BOB') { "--- $n"; Get-Content "$root\analysis\mp_$n.log"; $e = Get-Content "$root\analysis\mp_$n.err" -ErrorAction SilentlyContinue; if ($e) { "stderr:"; $e } }
