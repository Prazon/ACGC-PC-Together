param(
    [string]$OutputDirectory = ""
)

$ErrorActionPreference = "Stop"
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $repo = Split-Path -Parent $PSScriptRoot
    $OutputDirectory = Join-Path $repo "pc\build64\manual-two-client-test"
}

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class ClientCaptureNative {
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT rect);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
}
"@

$clients = @(Get-Process AnimalCrossing -ErrorAction Stop | Sort-Object Id)
if ($clients.Count -ne 2) {
    throw "Expected exactly two AnimalCrossing clients; found $($clients.Count)"
}

for ($index = 0; $index -lt $clients.Count; $index++) {
    $client = $clients[$index]
    $client.Refresh()
    $rect = New-Object ClientCaptureNative+RECT
    if ($client.MainWindowHandle -eq [IntPtr]::Zero -or
        -not [ClientCaptureNative]::GetWindowRect($client.MainWindowHandle, [ref]$rect)) {
        throw "Client $($client.Id) has no capturable window"
    }

    $bitmap = New-Object System.Drawing.Bitmap($($rect.Right - $rect.Left), $($rect.Bottom - $rect.Top))
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $dc = $graphics.GetHdc()
    try {
        if (-not [ClientCaptureNative]::PrintWindow($client.MainWindowHandle, $dc, 2)) {
            throw "PrintWindow failed for client $($client.Id)"
        }
    } finally {
        $graphics.ReleaseHdc($dc)
        $graphics.Dispose()
    }

    $path = Join-Path $OutputDirectory "client$($index + 1)-live.png"
    try {
        $bitmap.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    } finally {
        $bitmap.Dispose()
    }
    Write-Output $path
}
