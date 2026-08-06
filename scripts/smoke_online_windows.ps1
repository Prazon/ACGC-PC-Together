param(
    [string]$BuildDirectory = "",
    [ValidateRange(1024, 65535)]
    [int]$Port = 24681,
    [ValidateRange(5, 120)]
    [int]$Seconds = 15
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($BuildDirectory)) {
    $BuildDirectory = Join-Path $repo "pc\build64\bin"
}
$client = Join-Path $BuildDirectory "AnimalCrossing.exe"
$serverExe = Join-Path $BuildDirectory "AnimalCrossingServer.exe"
$smokeScript = Join-Path $PSScriptRoot "smoke_windows.ps1"
foreach ($file in @($client, $serverExe, $smokeScript)) {
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) {
        throw "Required smoke-test file is missing: $file"
    }
}

$rom = Get-ChildItem -LiteralPath (Join-Path $BuildDirectory "rom") -File |
    Where-Object { $_.Extension -in @(".iso", ".gcm", ".ciso") } |
    Select-Object -First 1
if ($null -eq $rom) {
    throw "Place a legitimate disc image in $BuildDirectory\rom before graphical smoke testing"
}

$key = "local-automated-smoke-key"
$townData = Join-Path ([System.IO.Path]::GetTempPath()) ("acgc-online-smoke-" + [Guid]::NewGuid().ToString("N"))
$serverStdout = Join-Path $BuildDirectory "smoke-x64-server.stdout.log"
$serverStderr = Join-Path $BuildDirectory "smoke-x64-server.stderr.log"
$serverTicks = ($Seconds + 5) * 60
$server = Start-Process -FilePath $serverExe -WorkingDirectory $BuildDirectory `
    -ArgumentList @("--port", "$Port", "--town", "1", "--data", $townData,
                    "--invite-key", $key, "--ticks", "$serverTicks") `
    -RedirectStandardOutput $serverStdout -RedirectStandardError $serverStderr -PassThru

try {
    Start-Sleep -Seconds 1
    $server.Refresh()
    if ($server.HasExited) {
        throw "Dedicated server exited before client start with code $($server.ExitCode)"
    }
    & $smokeScript -Executable $client -WorkingDirectory $BuildDirectory `
        -GameArguments @("--verbose", "--online", "127.0.0.1:$Port", "--town", "1",
                         "--account", "9001", "--invite-key", $key) `
        -Seconds $Seconds -LogPrefix "smoke-x64-online-final"
    if (-not $server.WaitForExit(10000)) {
        throw "Dedicated server did not complete its orderly smoke interval"
    }
    $serverOutput = Get-Content -LiteralPath $serverStdout -Raw
    if ($serverOutput -notmatch '"event":"server_started"' -or
        $serverOutput -notmatch '"event":"server_stopped"' -or
        $serverOutput -notmatch 'Animal Crossing Dedicated Town Server' -or
        $serverOutput -notmatch '\[TOWN\].*Players.*Time.*Weather') {
        throw "Dedicated server did not record an orderly lifecycle"
    }
    $combined = (Get-Content -LiteralPath $serverStderr, `
                    (Join-Path $BuildDirectory "smoke-x64-online-final.stderr.log") -Raw) -join "`n"
    if ($combined -match "(?i)(unable to start online client|server initialization failed|fatal error)") {
        throw "Online smoke logs contain a fatal startup error"
    }
    Write-Output "ONLINE_SMOKE_OK client_seconds=$Seconds port=$Port rom=$($rom.Name)"
} finally {
    $server.Refresh()
    if (-not $server.HasExited) {
        Stop-Process -Id $server.Id
        $server.WaitForExit()
    }
    if (Test-Path -LiteralPath $townData) {
        Remove-Item -LiteralPath $townData -Recurse -Force
    }
}
