param(
    [Parameter(Mandatory = $true)]
    [string]$Executable,

    [string]$WorkingDirectory = "",

    [string[]]$GameArguments = @("--verbose"),

    [ValidateRange(1, 300)]
    [int]$Seconds = 15,

    [string]$LogPrefix = "smoke"
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path -LiteralPath $Executable -PathType Leaf)) {
    throw "Executable does not exist: $Executable"
}

if ([string]::IsNullOrWhiteSpace($WorkingDirectory)) {
    $WorkingDirectory = Split-Path -Parent $Executable
}

$stdoutPath = Join-Path $WorkingDirectory "$LogPrefix.stdout.log"
$stderrPath = Join-Path $WorkingDirectory "$LogPrefix.stderr.log"

$process = Start-Process `
    -FilePath $Executable `
    -ArgumentList $GameArguments `
    -WorkingDirectory $WorkingDirectory `
    -RedirectStandardOutput $stdoutPath `
    -RedirectStandardError $stderrPath `
    -PassThru

Start-Sleep -Seconds $Seconds
$process.Refresh()

if ($process.HasExited) {
    Write-Output "SMOKE_EXITED code=$($process.ExitCode)"
    exit $process.ExitCode
}

Write-Output "SMOKE_ALIVE pid=$($process.Id) seconds=$Seconds"
Stop-Process -Id $process.Id
$process.WaitForExit()
Write-Output "SMOKE_STOPPED pid=$($process.Id)"
