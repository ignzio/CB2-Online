param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [string]$ArgLine = "",
    [int]$Seconds = 10,
    [string]$Log = ""
)
# Runs a program with a hard wall-clock limit (the process is killed when it is exceeded). Output goes to $Log (default
# analysis\last_run.log) and the last lines are echoed. Usage:
#   powershell -File tools\runlimit.ps1 -Exe build\Release\cb2_boot.exe -ArgLine ".. 900000000 --native" -Seconds 10
if (-not $Log) { $Log = Join-Path (Split-Path $PSScriptRoot -Parent) "analysis\last_run.log" }
$errFile = "$Log.err"
$p = Start-Process -FilePath $Exe -ArgumentList $ArgLine -PassThru -NoNewWindow -RedirectStandardOutput $Log -RedirectStandardError $errFile
$sw = [Diagnostics.Stopwatch]::StartNew()
while (-not $p.HasExited -and $sw.Elapsed.TotalSeconds -lt $Seconds) { Start-Sleep -Milliseconds 100 }
if (-not $p.HasExited) {
    Stop-Process -Id $p.Id -Force
    $p.WaitForExit(2000) | Out-Null
    "[runlimit] KILLED after $Seconds s"
    $code = 124
} else {
    $p.WaitForExit()
    $code = $p.ExitCode
    "[runlimit] finished in {0:n1}s, exit code {1}" -f $sw.Elapsed.TotalSeconds, $code
}
Get-Content $Log | Select-Object -Last 60
$e = Get-Content $errFile -ErrorAction SilentlyContinue | Select-Object -Last 20
if ($e) { "--- stderr ---"; $e }
exit $code
