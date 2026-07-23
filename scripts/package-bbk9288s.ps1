param(
    [Parameter(Mandatory)]
    [string]$QemuPath,

    [Parameter(Mandatory)]
    [string]$OutputDir,

    [string]$ArchivePath,

    [string]$MsysBin = "C:\msys64\ucrt64\bin"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$qemu = [System.IO.Path]::GetFullPath($QemuPath)
$output = [System.IO.Path]::GetFullPath($OutputDir)

if (-not (Test-Path -LiteralPath $qemu -PathType Leaf)) {
    throw "QEMU executable is missing: $qemu"
}
if (Test-Path -LiteralPath $output) {
    throw "Output directory already exists: $output"
}
if (-not (Test-Path -LiteralPath (Join-Path $root "web\dist\index.html"))) {
    throw "Build the Web frontend before packaging"
}

New-Item -ItemType Directory -Path $output | Out-Null
New-Item -ItemType Directory -Path (Join-Path $output "scripts") | Out-Null
New-Item -ItemType Directory -Path (Join-Path $output "share\keymaps") `
    -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $output "web") | Out-Null
New-Item -ItemType Directory -Path (Join-Path $output "runtime") | Out-Null

Copy-Item -LiteralPath $qemu -Destination (Join-Path $output "qemu-system-s1c33.exe")
Copy-Item -LiteralPath (Join-Path $root "run-bbk9288s-web.cmd") -Destination $output
Copy-Item -LiteralPath (Join-Path $root "run-bbk9288s-web.ps1") -Destination $output
Copy-Item -LiteralPath (Join-Path $root "requirements-bbk9288s.txt") -Destination $output
Copy-Item -LiteralPath (Join-Path $root "README.md") -Destination $output
Copy-Item -LiteralPath (Join-Path $root "SECURITY.md") -Destination $output
Copy-Item -LiteralPath (Join-Path $root "CONTRIBUTING.md") -Destination $output
Copy-Item -LiteralPath (Join-Path $root "LICENSE") -Destination $output
Copy-Item -LiteralPath (Join-Path $root "COPYING") -Destination $output
Copy-Item -LiteralPath (Join-Path $root "COPYING.LIB") -Destination $output
Copy-Item -LiteralPath (Join-Path $root "docs") -Destination $output -Recurse
Copy-Item -LiteralPath (Join-Path $root "scripts\bbk9288s_nand_image.py") `
    -Destination (Join-Path $output "scripts")
Copy-Item -LiteralPath (Join-Path $root "scripts\bbk9288s_web_server.py") `
    -Destination (Join-Path $output "scripts")
Copy-Item -LiteralPath (Join-Path $root "pc-bios\keymaps\en-us") `
    -Destination (Join-Path $output "share\keymaps")
Copy-Item -LiteralPath (Join-Path $root "web\dist") `
    -Destination (Join-Path $output "web") -Recurse

$runtimeReadme = @"
Place this file in this directory before starting the emulator:

  nand-user.raw    276,824,064-byte raw NAND image including OOB

The board loader finds kernel.bin inside the NAND FAT16 filesystem.
The test NAND release archive already uses this directory layout.
"@
Set-Content -LiteralPath (Join-Path $output "runtime\README.txt") `
    -Value $runtimeReadme -Encoding utf8

$objdump = Join-Path $MsysBin "objdump.exe"
if (-not (Test-Path -LiteralPath $objdump)) {
    throw "MSYS2 objdump is missing: $objdump"
}

$seen = [System.Collections.Generic.HashSet[string]]::new(
    [System.StringComparer]::OrdinalIgnoreCase
)
$queue = [System.Collections.Generic.Queue[string]]::new()
$queue.Enqueue($qemu)

while ($queue.Count -gt 0) {
    $binary = $queue.Dequeue()
    $imports = & $objdump -p $binary |
        Select-String "DLL Name:" |
        ForEach-Object { ($_ -split "DLL Name:", 2)[1].Trim() }
    foreach ($name in $imports) {
        $dependency = Join-Path $MsysBin $name
        if (-not (Test-Path -LiteralPath $dependency -PathType Leaf)) {
            continue
        }
        if (-not $seen.Add($name)) {
            continue
        }
        Copy-Item -LiteralPath $dependency -Destination (Join-Path $output $name)
        $queue.Enqueue($dependency)
    }
}

$manifest = Get-ChildItem -LiteralPath $output -Recurse -File |
    ForEach-Object {
        $hash = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        $relative = $_.FullName.Substring($output.Length)
        $relative = $relative.TrimStart([char[]]"\/").Replace("\", "/")
        "$hash  $relative"
    }
Set-Content -LiteralPath (Join-Path $output "SHA256SUMS.txt") `
    -Value $manifest -Encoding ascii

if ($ArchivePath) {
    $archive = [System.IO.Path]::GetFullPath($ArchivePath)
    if (Test-Path -LiteralPath $archive) {
        throw "Archive already exists: $archive"
    }
    Compress-Archive -LiteralPath $output -DestinationPath $archive `
        -CompressionLevel Optimal
    Write-Host "Archive: $archive"
}

Write-Host "Package: $output"
Write-Host "MSYS2 runtime DLLs: $($seen.Count)"
