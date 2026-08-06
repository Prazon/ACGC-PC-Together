param(
    [string]$BuildDirectory = "",
    [string]$OutputDirectory = "",
    [string]$Version = "dev"
)

$ErrorActionPreference = "Stop"
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
if ([string]::IsNullOrWhiteSpace($BuildDirectory)) {
    $BuildDirectory = Join-Path $projectRoot "pc\build64"
}
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $projectRoot "dist"
}
$bin = Join-Path $BuildDirectory "bin"
$client = Join-Path $bin "AnimalCrossing.exe"
$server = Join-Path $bin "AnimalCrossingServer.exe"
$sdl = Join-Path $bin "SDL2.dll"
$sqlite = Get-ChildItem -LiteralPath $bin -File -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -in @("sqlite3.dll", "libsqlite3-0.dll") } |
    Select-Object -First 1

foreach ($required in @($client, $server, $sdl)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Required release file is missing: $required"
    }
}
if ($null -eq $sqlite) { throw "SQLite runtime DLL is missing from $bin" }

function Assert-Pe64([string]$Path) {
    $bytes = [System.IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -lt 256 -or $bytes[0] -ne 0x4D -or $bytes[1] -ne 0x5A) {
        throw "Not a PE executable: $Path"
    }
    $pe = [BitConverter]::ToInt32($bytes, 0x3C)
    if ($pe -lt 0 -or $pe + 26 -gt $bytes.Length -or
        [BitConverter]::ToUInt32($bytes, $pe) -ne 0x00004550 -or
        [BitConverter]::ToUInt16($bytes, $pe + 4) -ne 0x8664 -or
        [BitConverter]::ToUInt16($bytes, $pe + 24) -ne 0x20B) {
        throw "Release executable is not PE32+ x86-64: $Path"
    }
}
Assert-Pe64 $client
Assert-Pe64 $server

New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$packageName = "ACGC-PC-Port-$Version-Windows-x86_64"
$stage = Join-Path $OutputDirectory $packageName
$zip = Join-Path $OutputDirectory "$packageName.zip"
if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
New-Item -ItemType Directory -Force -Path $stage | Out-Null

Copy-Item -LiteralPath $client, $server, $sdl, $sqlite.FullName -Destination $stage
Copy-Item -LiteralPath (Join-Path $bin "shaders") -Destination $stage -Recurse
foreach ($directory in @("rom", "save", "texture_pack", "towns\default", "docs")) {
    New-Item -ItemType Directory -Force -Path (Join-Path $stage $directory) | Out-Null
}
Copy-Item -LiteralPath (Join-Path $projectRoot "packaging\start_server.cmd") -Destination $stage
Copy-Item -LiteralPath (Join-Path $projectRoot "packaging\server.ini") -Destination $stage
Copy-Item -LiteralPath (Join-Path $projectRoot "packaging\network.ini") -Destination $stage
Copy-Item -LiteralPath (Join-Path $projectRoot "packaging\README.txt") -Destination $stage
Copy-Item -LiteralPath (Join-Path $projectRoot "LICENSE") -Destination $stage
Copy-Item -LiteralPath (Join-Path $projectRoot "docs\netcode\DEPLOYMENT.md") -Destination (Join-Path $stage "docs")
Copy-Item -LiteralPath (Join-Path $projectRoot "docs\netcode\PROTOCOL.md") -Destination (Join-Path $stage "docs")
Copy-Item -LiteralPath (Join-Path $projectRoot "docs\netcode\PERSISTENCE.md") -Destination (Join-Path $stage "docs")

$forbidden = Get-ChildItem -LiteralPath $stage -Recurse -File | Where-Object {
    $_.Extension -match '^\.(iso|gcm|ciso|gci)$' -or
    $_.Name -in @("town.db", "operations.log", "clean.shutdown", "config.toml")
}
if ($forbidden) {
    throw "Forbidden game/save/server state entered package: $($forbidden.FullName -join ', ')"
}

$hashLines = Get-ChildItem -LiteralPath $stage -Recurse -File |
    Sort-Object FullName |
    ForEach-Object {
        $relative = $_.FullName.Substring($stage.Length + 1).Replace('\', '/')
        "{0}  {1}" -f (Get-FileHash -Algorithm SHA256 -LiteralPath $_.FullName).Hash.ToLowerInvariant(), $relative
    }
$hashLines | Set-Content -LiteralPath (Join-Path $stage "SHA256SUMS.txt") -Encoding ASCII
Compress-Archive -LiteralPath $stage -DestinationPath $zip -CompressionLevel Optimal
Write-Output "PACKAGE_OK path=$zip files=$((Get-ChildItem -LiteralPath $stage -Recurse -File).Count)"
