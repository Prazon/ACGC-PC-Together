param(
    [Parameter(Mandatory = $true)]
    [string]$Executable,
    [string]$WorkingDirectory = "",
    [string[]]$GameArguments = @("--verbose"),
    [ValidateRange(1, 300)]
    [int]$DelaySeconds = 12,
    [ValidateRange(1, 20)]
    [int]$FrameCount = 2,
    [ValidateRange(50, 5000)]
    [int]$FrameIntervalMs = 250,
    [string]$OutputPrefix = "capture",
    [string]$FallbackWindowTitle = ""
)

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class WindowCaptureNative {
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")]
    public static extern bool GetWindowRect(IntPtr hwnd, out RECT rect);
}
"@

if ([string]::IsNullOrWhiteSpace($WorkingDirectory)) {
    $WorkingDirectory = Split-Path -Parent $Executable
}

$process = Start-Process -FilePath $Executable -ArgumentList $GameArguments `
    -WorkingDirectory $WorkingDirectory -PassThru

try {
    Start-Sleep -Seconds $DelaySeconds
    $process.Refresh()
    if ($process.HasExited) {
        throw "Game exited before capture with code $($process.ExitCode)"
    }

    $deadline = [DateTime]::UtcNow.AddSeconds(5)
    $windowHandle = $process.MainWindowHandle
    while ($windowHandle -eq [IntPtr]::Zero -and [DateTime]::UtcNow -lt $deadline) {
        if (-not [string]::IsNullOrWhiteSpace($FallbackWindowTitle)) {
            $hostWindow = Get-Process | Where-Object { $_.MainWindowTitle -eq $FallbackWindowTitle } |
                Select-Object -First 1
            if ($null -ne $hostWindow) {
                $windowHandle = $hostWindow.MainWindowHandle
                break
            }
        }
        Start-Sleep -Milliseconds 100
        $process.Refresh()
        $windowHandle = $process.MainWindowHandle
    }
    if ($windowHandle -eq [IntPtr]::Zero) {
        throw "Game did not create a capturable window"
    }

    $rect = New-Object WindowCaptureNative+RECT
    if (-not [WindowCaptureNative]::GetWindowRect($windowHandle, [ref]$rect)) {
        throw "GetWindowRect failed"
    }
    $width = $rect.Right - $rect.Left
    $height = $rect.Bottom - $rect.Top

    for ($i = 0; $i -lt $FrameCount; $i++) {
        $bitmap = New-Object System.Drawing.Bitmap($width, $height)
        $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
        try {
            $graphics.CopyFromScreen($rect.Left, $rect.Top, 0, 0, $bitmap.Size)
            $path = Join-Path $WorkingDirectory ("{0}-{1:D2}.png" -f $OutputPrefix, $i)
            $bitmap.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
            Write-Output "CAPTURED $path"
        } finally {
            $graphics.Dispose()
            $bitmap.Dispose()
        }
        if ($i + 1 -lt $FrameCount) {
            Start-Sleep -Milliseconds $FrameIntervalMs
        }
    }
} finally {
    $process.Refresh()
    if (-not $process.HasExited) {
        Stop-Process -Id $process.Id
        $process.WaitForExit()
    }
}
