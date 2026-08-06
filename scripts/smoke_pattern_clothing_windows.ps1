param(
    [string]$BuildDirectory = "",
    [string]$FixtureDirectory = "",
    [ValidateRange(1024, 65535)]
    [int]$Port = 24682,
    [ValidateRange(1, 4)]
    [int]$ResidentSlot = 1,
    [ValidateRange(10, 180)]
    [int]$TimeoutSeconds = 120,
    [switch]$KeepSession
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($BuildDirectory)) {
    $BuildDirectory = Join-Path $repo "pc\build64\bin"
}
if ([string]::IsNullOrWhiteSpace($FixtureDirectory)) {
    $FixtureDirectory = Join-Path $repo "pc\build64\manual-two-client-test"
}

$clientSource = Join-Path $BuildDirectory "AnimalCrossing.exe"
$serverSource = Join-Path $BuildDirectory "AnimalCrossingServer.exe"
$fixtureClient1 = Join-Path $FixtureDirectory "client1"
$fixtureClient2 = Join-Path $FixtureDirectory "client2"
$fixtureConfig = Join-Path $FixtureDirectory "server.ini"
$fixtureTown = Join-Path $FixtureDirectory "town"
$required = @(
    $clientSource,
    $serverSource,
    (Join-Path $BuildDirectory "SDL2.dll"),
    (Join-Path $BuildDirectory "libsqlite3-0.dll"),
    $fixtureConfig,
    (Join-Path $fixtureClient1 "save\card_a\DobutsunomoriP_MURA.gci"),
    (Join-Path $fixtureClient2 "save\card_a\DobutsunomoriP_MURA.gci")
)
foreach ($file in $required) {
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) {
        throw "Pattern-clothing smoke prerequisite is missing: $file"
    }
}
if (-not (Test-Path -LiteralPath $fixtureTown -PathType Container)) {
    throw "Pattern-clothing smoke town fixture is missing: $fixtureTown"
}
foreach ($fixtureClient in @($fixtureClient1, $fixtureClient2)) {
    if (-not (Get-ChildItem -LiteralPath (Join-Path $fixtureClient "rom") -File |
            Where-Object { $_.Extension -in @(".iso", ".gcm", ".ciso") } |
            Select-Object -First 1)) {
        throw "The fixture needs a user-supplied legitimate disc image in $fixtureClient\rom"
    }
}

$session = Join-Path ([System.IO.Path]::GetTempPath()) `
    ("acgc-pattern-clothing-smoke-" + [Guid]::NewGuid().ToString("N"))
$client1Directory = Join-Path $session "client1"
$client2Directory = Join-Path $session "client2"
$server = $null
$client1 = $null
$client2 = $null
$passed = $false

function Set-PatternClothing([string]$Path, [int]$Slot) {
    # GCI values are big-endian. This patches only a temporary copy: clothing
    # design slot 0 has texture index 0x100 and the RSV_CLOTH item 0xFE20.
    $bytes = [System.IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -ne 0x72040) {
        throw "Unexpected GCI size: $($bytes.Length)"
    }
    $saveOffsets = @(0x26040, 0x4C040)
    $privateOffset = 0x20 + (($Slot - 1) * 0x2440)
    foreach ($saveOffset in $saveOffsets) {
        $private = $saveOffset + $privateOffset
        if ($bytes[$private + 0x1086] -eq 0) {
            throw "Resident slot $Slot does not exist in the GCI fixture"
        }
        $bytes[$private + 0x1088] = 0x01
        $bytes[$private + 0x1089] = 0x00
        $bytes[$private + 0x108A] = 0xFE
        $bytes[$private + 0x108B] = 0x20

        # Save_t's checksum is the two's complement of all BE u16 words with
        # the checksum field cleared.
        $bytes[$saveOffset + 0x12] = 0
        $bytes[$saveOffset + 0x13] = 0
        [uint32]$sum = 0
        for ($offset = 0; $offset -lt 0x242A0; $offset += 2) {
            $word = ([uint32]$bytes[$saveOffset + $offset] -shl 8) -bor `
                    [uint32]$bytes[$saveOffset + $offset + 1]
            $sum = ($sum + $word) -band 0xFFFFFFFFL
        }
        $checksum = (-$sum) -band 0xFFFF
        $bytes[$saveOffset + 0x12] = ($checksum -shr 8) -band 0xFF
        $bytes[$saveOffset + 0x13] = $checksum -band 0xFF
    }
    [System.IO.File]::WriteAllBytes($Path, $bytes)
}

try {
    $null = New-Item -ItemType Directory -Path $session
    Copy-Item -LiteralPath $fixtureClient1 -Destination $client1Directory -Recurse
    Copy-Item -LiteralPath $fixtureClient2 -Destination $client2Directory -Recurse
    Copy-Item -LiteralPath $fixtureConfig -Destination (Join-Path $session "server.ini")
    Copy-Item -LiteralPath $fixtureTown -Destination (Join-Path $session "town") -Recurse
    foreach ($clientDirectory in @($client1Directory, $client2Directory)) {
        Copy-Item -LiteralPath $clientSource, (Join-Path $BuildDirectory "SDL2.dll") `
            -Destination $clientDirectory -Force
    }
    Copy-Item -LiteralPath $serverSource, (Join-Path $BuildDirectory "libsqlite3-0.dll") `
        -Destination $session -Force

    $gci = Join-Path $client1Directory "save\card_a\DobutsunomoriP_MURA.gci"
    Set-PatternClothing $gci $ResidentSlot

    $serverOut = Join-Path $session "server.stdout.log"
    $serverErr = Join-Path $session "server.stderr.log"
    $client1Out = Join-Path $session "client1.stdout.log"
    $client1Err = Join-Path $session "client1.stderr.log"
    $client2Out = Join-Path $session "client2.stdout.log"
    $client2Err = Join-Path $session "client2.stderr.log"
    $server = Start-Process -FilePath (Join-Path $session "AnimalCrossingServer.exe") `
        -WorkingDirectory $session `
        -ArgumentList @("--config", "server.ini", "--port", "$Port", "--no-dashboard") `
        -RedirectStandardOutput $serverOut -RedirectStandardError $serverErr -PassThru
    Start-Sleep -Seconds 1
    $server.Refresh()
    if ($server.HasExited) {
        throw "Dedicated server exited with code $($server.ExitCode): $((Get-Content $serverErr -Raw))"
    }

    $client1 = Start-Process -FilePath (Join-Path $client1Directory "AnimalCrossing.exe") `
        -WorkingDirectory $client1Directory `
        -ArgumentList @("--verbose", "--online", "127.0.0.1:$Port", "--town", "1",
                        "--account", "1001", "--invite-key", "local-two-client-test",
                        "--quickstart", "Pattern", "--quickstart-gender", "male") `
        -RedirectStandardOutput $client1Out -RedirectStandardError $client1Err -PassThru
    $client2 = Start-Process -FilePath (Join-Path $client2Directory "AnimalCrossing.exe") `
        -WorkingDirectory $client2Directory `
        -ArgumentList @("--verbose", "--online", "127.0.0.1:$Port", "--town", "1",
                        "--account", "1002", "--invite-key", "local-two-client-test",
                        "--quickstart", "Observer", "--quickstart-gender", "female") `
        -RedirectStandardOutput $client2Out -RedirectStandardError $client2Err -PassThru

    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    $client1Ready = $false
    $client2Ready = $false
    do {
        Start-Sleep -Milliseconds 250
        foreach ($entry in @(
            @{ Name = "pattern client"; Process = $client1; ErrorLog = $client1Err },
            @{ Name = "observer client"; Process = $client2; ErrorLog = $client2Err }
        )) {
            $entry.Process.Refresh()
            if ($entry.Process.HasExited) {
                $entry.Process.WaitForExit()
                $entry.Process.Refresh()
                $exitCode = $entry.Process.ExitCode
                $stderr = if (Test-Path $entry.ErrorLog) {
                    Get-Content -LiteralPath $entry.ErrorLog -Raw
                } else { "" }
                throw "$($entry.Name) exited with code ${exitCode}: $stderr"
            }
        }
        if (Test-Path $client1Out) {
            $client1Ready = (Get-Content -LiteralPath $client1Out -Raw) -match '\[NET\] gameplay ready'
        }
        if (Test-Path $client2Out) {
            $client2Ready = (Get-Content -LiteralPath $client2Out -Raw) -match '\[NET\] gameplay ready'
        }
    } while ((-not $client1Ready -or -not $client2Ready) -and [DateTime]::UtcNow -lt $deadline)
    if (-not $client1Ready -or -not $client2Ready) {
        throw "Both pattern-clothing clients did not reach gameplay within $TimeoutSeconds seconds"
    }
    Start-Sleep -Seconds 3
    $client1.Refresh()
    $client2.Refresh()
    if ($client1.HasExited -or $client2.HasExited) {
        throw "A pattern-clothing client exited immediately after both reached gameplay"
    }
    $observerOutput = Get-Content -LiteralPath $client2Out -Raw
    if ($observerOutput -notmatch '\[NET\] remote pattern loaded account=1001 ') {
        throw "Observer did not load account 1001's replicated pattern"
    }
    $passed = $true
    Write-Output "PATTERN_CLOTHING_SMOKE_OK resident_slot=$ResidentSlot clients=2"
} finally {
    foreach ($process in @($client1, $client2, $server)) {
        if ($null -ne $process) {
            $process.Refresh()
            if (-not $process.HasExited) {
                Stop-Process -Id $process.Id
                $process.WaitForExit()
            }
        }
    }
    if ($KeepSession -or -not $passed) {
        Write-Output "PATTERN_CLOTHING_SMOKE_SESSION=$session"
    } elseif (Test-Path -LiteralPath $session) {
        Remove-Item -LiteralPath $session -Recurse -Force
    }
}
