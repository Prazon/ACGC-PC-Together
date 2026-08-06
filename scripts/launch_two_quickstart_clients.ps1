param(
    [string]$SessionDirectory = "",
    [ValidateRange(1024, 65535)]
    [int]$Port = 24680,
    [string]$FirstName = "Tester1",
    [string]$SecondName = "Tester2",
    [switch]$ReuseServer
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($SessionDirectory)) {
    $SessionDirectory = Join-Path $repo "pc\build64\manual-two-client-test"
}
$sourceBin = Join-Path $repo "pc\build64\bin"
$serverExe = Join-Path $SessionDirectory "AnimalCrossingServer.exe"
$serverConfig = Join-Path $SessionDirectory "server.ini"
$client1Dir = Join-Path $SessionDirectory "client1"
$client2Dir = Join-Path $SessionDirectory "client2"
$client1Exe = Join-Path $client1Dir "AnimalCrossing.exe"
$client2Exe = Join-Path $client2Dir "AnimalCrossing.exe"

foreach ($required in @(
    (Join-Path $sourceBin "AnimalCrossing.exe"),
    (Join-Path $sourceBin "AnimalCrossingServer.exe"),
    (Join-Path $sourceBin "SDL2.dll"),
    (Join-Path $sourceBin "libsqlite3-0.dll"),
    $serverConfig,
    (Join-Path $client1Dir "network.ini"),
    (Join-Path $client2Dir "network.ini")
)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Required two-client test file is missing: $required"
    }
}
foreach ($clientDir in @($client1Dir, $client2Dir)) {
    if (-not (Get-ChildItem -LiteralPath (Join-Path $clientDir "rom") -File |
            Where-Object { $_.Extension -in @(".iso", ".gcm", ".ciso") } |
            Select-Object -First 1)) {
        throw "No legitimate disc image is present in $clientDir\rom"
    }
}

$runningClients = @(Get-CimInstance Win32_Process | Where-Object { $_.Name -eq "AnimalCrossing.exe" })
if ($runningClients.Count -ne 0) {
    throw "Animal Crossing clients are already running; refusing to replace the active test session"
}
$runningServers = @(Get-CimInstance Win32_Process | Where-Object { $_.Name -eq "AnimalCrossingServer.exe" })
if ($ReuseServer) {
    $matchingServer = @($runningServers | Where-Object { $_.ExecutablePath -eq $serverExe })
    if ($matchingServer.Count -ne 1) {
        throw "-ReuseServer requires exactly one server running from $serverExe"
    }
    $server = Get-Process -Id $matchingServer[0].ProcessId
} elseif ($runningServers.Count -ne 0) {
    throw "An Animal Crossing server is already running; use -ReuseServer for this test town"
}

if (-not $ReuseServer) {
    Copy-Item -LiteralPath (Join-Path $sourceBin "AnimalCrossingServer.exe"), `
        (Join-Path $sourceBin "libsqlite3-0.dll") -Destination $SessionDirectory -Force
}
foreach ($clientDir in @($client1Dir, $client2Dir)) {
    Copy-Item -LiteralPath (Join-Path $sourceBin "AnimalCrossing.exe"), `
        (Join-Path $sourceBin "SDL2.dll") -Destination $clientDir -Force
    Copy-Item -LiteralPath (Join-Path $sourceBin "shaders\default.vert"), `
        (Join-Path $sourceBin "shaders\default.frag") `
        -Destination (Join-Path $clientDir "shaders") -Force
}

$serverOut = Join-Path $SessionDirectory "server-live.stdout.log"
$serverErr = Join-Path $SessionDirectory "server-live.stderr.log"
$client1Out = Join-Path $client1Dir "client-live.stdout.log"
$client1Err = Join-Path $client1Dir "client-live.stderr.log"
$client2Out = Join-Path $client2Dir "client-live.stdout.log"
$client2Err = Join-Path $client2Dir "client-live.stderr.log"

if (-not $ReuseServer) {
    $server = Start-Process -FilePath $serverExe -WorkingDirectory $SessionDirectory `
        -ArgumentList @("--config", $serverConfig, "--port", "$Port") `
        -RedirectStandardOutput $serverOut -RedirectStandardError $serverErr -PassThru
    Start-Sleep -Seconds 2
    $server.Refresh()
    if ($server.HasExited) {
        throw "Dedicated server exited with code $($server.ExitCode): $((Get-Content $serverErr -Raw))"
    }
}

$common = @("--verbose", "--online", "127.0.0.1:$Port", "--town", "1",
            "--invite-key", "local-two-client-test")
$client1 = Start-Process -FilePath $client1Exe -WorkingDirectory $client1Dir `
    -ArgumentList ($common + @("--account", "1001", "--quickstart", $FirstName,
                               "--quickstart-gender", "male")) `
    -RedirectStandardOutput $client1Out -RedirectStandardError $client1Err -PassThru
$client2 = Start-Process -FilePath $client2Exe -WorkingDirectory $client2Dir `
    -ArgumentList ($common + @("--account", "1002", "--quickstart", $SecondName,
                               "--quickstart-gender", "female")) `
    -RedirectStandardOutput $client2Out -RedirectStandardError $client2Err -PassThru

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class QuickstartWindowNative {
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool MoveWindow(IntPtr h, int x, int y, int w, int hgt, bool repaint);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT rect);
}
"@

function Wait-Window([System.Diagnostics.Process]$Process) {
    $deadline = [DateTime]::UtcNow.AddSeconds(20)
    do {
        Start-Sleep -Milliseconds 200
        $Process.Refresh()
        if ($Process.HasExited) { throw "Client exited with code $($Process.ExitCode)" }
    } while ($Process.MainWindowHandle -eq [IntPtr]::Zero -and [DateTime]::UtcNow -lt $deadline)
    if ($Process.MainWindowHandle -eq [IntPtr]::Zero) { throw "Client did not create a window" }
    return $Process.MainWindowHandle
}

$window1 = Wait-Window $client1
$window2 = Wait-Window $client2
$area = [System.Windows.Forms.Screen]::PrimaryScreen.WorkingArea
$width = [Math]::Min(640, [Math]::Floor($area.Width / 2))
$height = [Math]::Min(480, $area.Height)
[QuickstartWindowNative]::MoveWindow($window1, $area.Left, $area.Top, $width, $height, $true) | Out-Null
[QuickstartWindowNative]::MoveWindow($window2, $area.Left + $width, $area.Top, $width, $height, $true) | Out-Null

$arrived = $false
for ($round = 0; $round -lt 180; $round++) {
    foreach ($client in @($client1, $client2)) {
        $client.Refresh()
        if ($client.HasExited) { throw "Client exited with code $($client.ExitCode)" }
    }
    Start-Sleep -Seconds 1
    $log1 = if (Test-Path $client1Out) { Get-Content -LiteralPath $client1Out -Raw } else { "" }
    $log2 = if (Test-Path $client2Out) { Get-Content -LiteralPath $client2Out -Raw } else { "" }
    $client1Ready = $log1 -match '\[NET\] (Online resident initialized and saved|gameplay ready)'
    $client2Ready = $log2 -match '\[NET\] (Online resident initialized and saved|gameplay ready)'
    if ($client1Ready -and $client2Ready) {
        $arrived = $true
        break
    }
}

function Capture-Window([IntPtr]$Handle, [string]$Path) {
    $rect = New-Object QuickstartWindowNative+RECT
    if (-not [QuickstartWindowNative]::GetWindowRect($Handle, [ref]$rect)) { return }
    $bitmap = New-Object System.Drawing.Bitmap($($rect.Right - $rect.Left), $($rect.Bottom - $rect.Top))
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    try {
        $graphics.CopyFromScreen($rect.Left, $rect.Top, 0, 0, $bitmap.Size)
        $bitmap.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
    } finally {
        $graphics.Dispose()
        $bitmap.Dispose()
    }
}
Capture-Window $window1 (Join-Path $client1Dir "quickstart-live.png")
Capture-Window $window2 (Join-Path $client2Dir "quickstart-live.png")

@{
    server_pid = $server.Id
    client1_pid = $client1.Id
    client2_pid = $client2.Id
    arrived = $arrived
    started_at = [DateTime]::UtcNow.ToString("o")
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $SessionDirectory "live-session.json") -Encoding ASCII

Write-Output "TWO_CLIENTS_STARTED server_pid=$($server.Id) client1_pid=$($client1.Id) client2_pid=$($client2.Id) arrived=$arrived"
