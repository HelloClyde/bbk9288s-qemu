param(
    [ValidateRange(1, 65535)]
    [int]$HttpPort = 8000,

    [ValidateRange(1, 65535)]
    [int]$WebSocketPort = 6081,

    [ValidateRange(1, 65535)]
    [int]$QmpPort = 6082,

    [string]$RuntimeDir
)

$ErrorActionPreference = "Stop"
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new()
$env:PYTHONUTF8 = "1"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$runtimeBin = "C:\msys64\ucrt64\bin"
$packagedQemu = Join-Path $root "qemu-system-s1c33.exe"
$sourceQemu = Join-Path $root "build\qemu-system-s1c33.exe"
$releaseQemu = Join-Path $root "_build\qemu-system-s1c33.exe"
$qemu = if (Test-Path -LiteralPath $packagedQemu) {
    $packagedQemu
} elseif (Test-Path -LiteralPath $sourceQemu) {
    $sourceQemu
} else {
    $releaseQemu
}
if (-not $RuntimeDir) {
    $RuntimeDir = if (Test-Path -LiteralPath $packagedQemu) {
        Join-Path $root "runtime"
    } elseif ($env:BBK9288S_RUNTIME_DIR) {
        $env:BBK9288S_RUNTIME_DIR
    } else {
        Join-Path (Split-Path -Parent $root) "eebbk9288s-runtime"
    }
}
$RuntimeDir = [System.IO.Path]::GetFullPath($RuntimeDir)
$kernel = Join-Path $RuntimeDir "kernel.bin"
$userNand = Join-Path $RuntimeDir "nand-user.raw"
$nandTool = Join-Path $root "scripts\bbk9288s_nand_image.py"
$webServer = Join-Path $root "scripts\bbk9288s_web_server.py"
$webRoot = Join-Path $root "web"
$webDist = Join-Path $webRoot "dist"
$python = (Get-Command python -ErrorAction Stop).Source

foreach ($path in @($qemu, $kernel, $userNand, $nandTool, $webServer)) {
    if (-not (Test-Path -LiteralPath $path)) {
        throw "Required emulator file is missing: $path"
    }
}

$ports = @($HttpPort, $WebSocketPort, $QmpPort)
if (($ports | Select-Object -Unique).Count -ne 3) {
    throw "HttpPort, WebSocketPort, and QmpPort must be different"
}

foreach ($port in $ports) {
    if (Get-NetTCPConnection -State Listen -LocalPort $port -ErrorAction SilentlyContinue) {
        throw "TCP port $port is already in use"
    }
}
if (Get-NetTCPConnection -State Listen -LocalPort 5900 -ErrorAction SilentlyContinue) {
    throw "QEMU VNC port 5900 is already in use"
}

if (Test-Path -LiteralPath $runtimeBin) {
    $env:PATH = "$runtimeBin;$env:PATH"
}

if (Test-Path -LiteralPath (Join-Path $webRoot "package.json")) {
    Push-Location $webRoot
    try {
        if (-not (Test-Path -LiteralPath (Join-Path $webRoot "node_modules"))) {
            & npm ci
            if ($LASTEXITCODE -ne 0) {
                throw "npm ci failed"
            }
        }
        & npm run build
        if ($LASTEXITCODE -ne 0) {
            throw "Web frontend build failed"
        }
    } finally {
        Pop-Location
    }
}
if (-not (Test-Path -LiteralPath (Join-Path $webDist "index.html"))) {
    throw "Web frontend is missing: $webDist"
}

$query = if ($WebSocketPort -eq 6081) { "" } else { "?wsPort=$WebSocketPort" }
Write-Host ""
Write-Host "BBK 9288S Web is running:"
Write-Host "  Local: http://127.0.0.1:$HttpPort/$query"
Write-Host "  Data:  $RuntimeDir"
$addresses = Get-NetIPAddress -AddressFamily IPv4 -ErrorAction SilentlyContinue |
    Where-Object {
        $_.IPAddress -notlike "127.*" -and
        $_.IPAddress -notlike "169.254.*" -and
        $_.PrefixOrigin -ne "WellKnown"
    } |
    Select-Object -ExpandProperty IPAddress -Unique
foreach ($address in $addresses) {
    Write-Host "  LAN:   http://${address}:$HttpPort/$query"
}
Write-Host ""

Push-Location $root
try {
    & $python $webServer `
        --root $root `
        --runtime-dir $RuntimeDir `
        --qemu $qemu `
        --kernel $kernel `
        --nand $userNand `
        --dist $webDist `
        --http-port $HttpPort `
        --websocket-port $WebSocketPort `
        --qmp-port $QmpPort
    if ($LASTEXITCODE -ne 0) {
        throw "Web server exited with code $LASTEXITCODE"
    }
} finally {
    Pop-Location
}
