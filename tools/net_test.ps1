param([int]$Port = 7790)
# Runs the network self-test (LAN / same-PC discovery, rendezvous, relay, leave and rejoin) against a local rendezvous server.
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
$srvExe = "$root\build\Release\cb2_server.exe"
$test = "$root\build\Release\cb2_nettest.exe"
$srv = Start-Process $srvExe -ArgumentList "$Port" -PassThru -NoNewWindow -RedirectStandardOutput "$root\analysis\nettest_server.log" -RedirectStandardError "$root\analysis\nettest_server.err"
try {
    Start-Sleep 1
    & $test "127.0.0.1:$Port"
    $main = $LASTEXITCODE
    Write-Host "--- server survival: the server is stopped while two players are connected"
    $go = "$root\analysis\survive.go"
    Remove-Item $go -ErrorAction SilentlyContinue
    $out = "$root\analysis\survive.log"
    $t = Start-Process $test -ArgumentList "--survive 127.0.0.1:$Port `"$go`"" -PassThru -NoNewWindow -RedirectStandardOutput $out
    $null = $t.Handle
    $deadline = (Get-Date).AddSeconds(15)
    while ((Get-Date) -lt $deadline -and -not ((Test-Path $out) -and (Select-String -Path $out -Pattern 'CONNECTED' -Quiet))) { Start-Sleep -Milliseconds 200 }
    $srv.Kill(); $srv.WaitForExit()
    Set-Content $go "go"
    $t.WaitForExit(30000) | Out-Null
    Get-Content $out
    $surv = $t.ExitCode
} finally {
    if (-not $srv.HasExited) { $srv.Kill() }
}
# ---- lobby service tests: only when the maintainer's private Worker folder (worker/, git-ignored) is present and wrangler is installed ---------------------------------------------------------------------
$dirResult = 0
function Stop-Worker { Get-CimInstance Win32_Process | Where-Object { $_.ProcessId -ne $PID -and $_.CommandLine -match 'wrangler|workerd' -and $_.CommandLine -notmatch 'net_test' } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue } }
if (Test-Path "$root\worker\node_modules\.bin\wrangler.cmd") {
    Write-Host "--- lobby directory: Worker running locally, a dedicated server announcing itself, players joining through it"
    $dport = 8799
    Stop-Worker
    $null = Start-Process cmd.exe -ArgumentList '/c', "node_modules\.bin\wrangler.cmd dev --port $dport --local --var ALLOW_PRIVATE:1" -WorkingDirectory "$root\worker" -PassThru -WindowStyle Hidden -RedirectStandardOutput "$root\analysis\wrangler.log" -RedirectStandardError "$root\analysis\wrangler.err"
    $ds = $null
    try {
        $up = $false
        for ($i = 0; $i -lt 80 -and -not $up; $i++) { Start-Sleep -Milliseconds 500; $r = curl.exe -s -m 2 "http://127.0.0.1:$dport/health"; $up = "$r" -like 'OK*' }
        if (-not $up) { throw "the local Worker did not start (see analysis\wrangler.log)" }
        $ds = Start-Process $srvExe -ArgumentList "7791 --announce http://127.0.0.1:$dport --name `"Test Server`" --public-port 7791" -PassThru -NoNewWindow -RedirectStandardOutput "$root\analysis\dirserver.log" -RedirectStandardError "$root\analysis\dirserver.err"
        $null = $ds.Handle
        $dt = Start-Process $test -ArgumentList "--directory http://127.0.0.1:$dport `"Test Server`"" -PassThru -NoNewWindow -RedirectStandardOutput "$root\analysis\dirtest.log"
        $null = $dt.Handle
        $dt.WaitForExit(240000) | Out-Null
        Get-Content "$root\analysis\dirtest.log"
        $dirResult = $dt.ExitCode
    } finally {
        if ($ds -and -not $ds.HasExited) { $ds.Kill() }
        Stop-Worker
    }
    Write-Host "--- STUN (needs the internet)"
    & $test --stun
}
Write-Host "--- server log"
Get-Content "$root\analysis\nettest_server.log" | Select -First 20
if ($main -ne 0 -or $surv -ne 0 -or $dirResult -ne 0) { Write-Host "NET TEST FAILED"; exit 1 } else { Write-Host "NET TEST PASSED" }
