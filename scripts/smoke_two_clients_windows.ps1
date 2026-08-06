param(
    [string]$BuildDirectory = "",
    [ValidateRange(1024, 65535)]
    [int]$Port = 24682,
    [ValidateRange(10, 120)]
    [int]$Seconds = 20
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($BuildDirectory)) {
    $BuildDirectory = Join-Path $repo "pc\build64\bin"
}
$clientExe = Join-Path $BuildDirectory "AnimalCrossing.exe"
$serverExe = Join-Path $BuildDirectory "AnimalCrossingServer.exe"
foreach ($file in @($clientExe, $serverExe)) {
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) {
        throw "Required smoke-test executable is missing: $file"
    }
}
$rom = Get-ChildItem -LiteralPath (Join-Path $BuildDirectory "rom") -File |
    Where-Object { $_.Extension -in @(".iso", ".gcm", ".ciso") } |
    Select-Object -First 1
if ($null -eq $rom) {
    throw "Place a legitimate disc image in $BuildDirectory\rom before graphical smoke testing"
}

$key = "local-two-client-smoke-key"
$townData = Join-Path ([System.IO.Path]::GetTempPath()) ("acgc-two-client-smoke-" + [Guid]::NewGuid().ToString("N"))
$null = New-Item -ItemType Directory -Force -Path $townData
$serverConfig = Join-Path $townData "server.ini"
Copy-Item -LiteralPath (Join-Path $repo "packaging\server.ini") -Destination $serverConfig
$serverStdout = Join-Path $BuildDirectory "smoke-x64-two-client-server.stdout.log"
$serverStderr = Join-Path $BuildDirectory "smoke-x64-two-client-server.stderr.log"
$serverTicks = ($Seconds + 8) * 60
$clients = @()
$server = Start-Process -FilePath $serverExe -WorkingDirectory $BuildDirectory `
    -ArgumentList @("--config", $serverConfig, "--port", "$Port", "--town", "1", "--data", $townData,
                    "--invite-key", $key, "--ticks", "$serverTicks") `
    -RedirectStandardOutput $serverStdout -RedirectStandardError $serverStderr -PassThru

try {
    Start-Sleep -Seconds 1
    $server.Refresh()
    if ($server.HasExited) {
        throw "Dedicated server exited before client start with code $($server.ExitCode)"
    }

    foreach ($index in 0..1) {
        $account = 9101 + $index
        $stdout = Join-Path $BuildDirectory "smoke-x64-client-$($index + 1).stdout.log"
        $stderr = Join-Path $BuildDirectory "smoke-x64-client-$($index + 1).stderr.log"
        $clients += Start-Process -FilePath $clientExe -WorkingDirectory $BuildDirectory `
            -ArgumentList @("--verbose", "--online", "127.0.0.1:$Port", "--town", "1",
                            "--account", "$account", "--invite-key", $key) `
            -RedirectStandardOutput $stdout -RedirectStandardError $stderr -PassThru
    }

    Start-Sleep -Seconds $Seconds
    foreach ($client in $clients) {
        $client.Refresh()
        if ($client.HasExited) {
            throw "Graphical client $($client.Id) exited early with code $($client.ExitCode)"
        }
    }

    $bothConnected = $false
    $dashboardDeadline = [DateTime]::UtcNow.AddSeconds(8)
    do {
        $serverOutput = Get-Content -LiteralPath $serverStdout -Raw
        $bothConnected = $serverOutput -match '(?m)Players\s+(?:\: )?2/16'
        if (-not $bothConnected) { Start-Sleep -Milliseconds 250 }
    } while (-not $bothConnected -and [DateTime]::UtcNow -lt $dashboardDeadline)
    if (-not $bothConnected) {
        throw "Dedicated server never reported both graphical clients connected"
    }
    foreach ($index in 0..1) {
        $stdout = Join-Path $BuildDirectory "smoke-x64-client-$($index + 1).stdout.log"
        $stderr = Join-Path $BuildDirectory "smoke-x64-client-$($index + 1).stderr.log"
        $clientOutput = Get-Content -LiteralPath $stdout -Raw
        $clientErrors = Get-Content -LiteralPath $stderr -Raw
        if ($clientOutput -notmatch '\[NET\] joined town=' -or $clientOutput -notmatch '\[NET\] connection status=2') {
            throw "Graphical client $($index + 1) did not complete the online handshake"
        }
        if ($clientErrors -match '(?i)(unable to start online client|fatal error)') {
            throw "Graphical client $($index + 1) reported a fatal startup error"
        }
    }
    Write-Output "TWO_CLIENT_SMOKE_OK client_seconds=$Seconds port=$Port rom=$($rom.Name)"
} finally {
    foreach ($client in $clients) {
        $client.Refresh()
        if (-not $client.HasExited) {
            Stop-Process -Id $client.Id
            $client.WaitForExit()
        }
    }
    $server.Refresh()
    if (-not $server.HasExited) {
        Stop-Process -Id $server.Id
        $server.WaitForExit()
    }
    if (Test-Path -LiteralPath $townData) {
        Remove-Item -LiteralPath $townData -Recurse -Force
    }
}
