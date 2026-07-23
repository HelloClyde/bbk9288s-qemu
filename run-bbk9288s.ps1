param(
    [ValidateSet("gtk", "sdl")]
    [string]$Display = "gtk",

    [string]$RuntimeDir
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$runtimeBin = "C:\msys64\ucrt64\bin"
$qemu = Join-Path $root "build\qemu-system-s1c33.exe"
if (-not $RuntimeDir) {
    $RuntimeDir = if ($env:BBK9288S_RUNTIME_DIR) {
        $env:BBK9288S_RUNTIME_DIR
    } else {
        Join-Path (Split-Path -Parent $root) "eebbk9288s-runtime"
    }
}
$RuntimeDir = [System.IO.Path]::GetFullPath($RuntimeDir)
$userNand = Join-Path $RuntimeDir "nand-user.raw"

foreach ($path in @($qemu, $userNand)) {
    if (-not (Test-Path -LiteralPath $path)) {
        throw "Required emulator file is missing: $path"
    }
}

$env:PATH = "$runtimeBin;$env:PATH"

$displayOptions = if ($Display -eq "gtk") {
    "gtk,show-tabs=off,show-cursor=on,window-close=on"
} else {
    "sdl,show-cursor=on,window-close=on"
}

Push-Location $root
try {
    & $qemu `
        -name "BBK 9288S Emulator" `
        -machine "bbk9288s,nand-image=$($userNand.Replace('\', '/'))" `
        -cpu "c33l05,exit-on-halt=off" `
        -rtc "base=localtime" `
        -display $displayOptions `
        -serial none `
        -monitor none
    if ($LASTEXITCODE -ne 0) {
        throw "QEMU exited with code $LASTEXITCODE"
    }
} finally {
    Pop-Location
}
