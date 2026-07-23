param(
    [Parameter(Mandatory)]
    [string]$PackageDir,

    [ValidateRange(0, 99)]
    [int]$VncDisplay = 99
)

$ErrorActionPreference = "Stop"
$package = [System.IO.Path]::GetFullPath($PackageDir)
$qemu = Join-Path $package "qemu-system-s1c33.exe"
$dataDir = Join-Path $package "share"
$stderr = Join-Path $package "package-smoke.stderr.log"
$port = 5900 + $VncDisplay

foreach ($path in @($qemu, (Join-Path $dataDir "keymaps\en-us"))) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Package smoke test input is missing: $path"
    }
}
if (Get-NetTCPConnection -State Listen -LocalPort $port -ErrorAction SilentlyContinue) {
    throw "Package smoke test TCP port is already in use: $port"
}

$arguments = @(
    "-L", "share",
    "-machine", "none",
    "-display", "vnc=127.0.0.1:$VncDisplay",
    "-serial", "none",
    "-monitor", "none"
)
$process = Start-Process `
    -FilePath $qemu `
    -ArgumentList $arguments `
    -WorkingDirectory $package `
    -WindowStyle Hidden `
    -RedirectStandardError $stderr `
    -PassThru

try {
    $ready = $false
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    while ([DateTime]::UtcNow -lt $deadline) {
        $process.Refresh()
        if ($process.HasExited) {
            $details = Get-Content -LiteralPath $stderr -Raw -ErrorAction SilentlyContinue
            throw "Packaged QEMU exited with code $($process.ExitCode):`n$details"
        }
        try {
            $client = [System.Net.Sockets.TcpClient]::new()
            $client.Connect("127.0.0.1", $port)
            $client.Dispose()
            $ready = $true
            break
        } catch {
            if ($client) {
                $client.Dispose()
            }
            Start-Sleep -Milliseconds 100
        }
    }
    if (-not $ready) {
        $details = Get-Content -LiteralPath $stderr -Raw -ErrorAction SilentlyContinue
        throw "Packaged QEMU VNC did not start on port ${port}:`n$details"
    }
    Write-Host "Packaged QEMU VNC smoke test passed on port $port"
} finally {
    $process.Refresh()
    if (-not $process.HasExited) {
        Stop-Process -Id $process.Id
        Wait-Process -Id $process.Id -Timeout 10 -ErrorAction SilentlyContinue
    }
    Remove-Item -LiteralPath $stderr -ErrorAction SilentlyContinue
}
