param(
    [Parameter(Mandatory)]
    [string]$NandPath,

    [Parameter(Mandatory)]
    [string]$ArchivePath
)

$ErrorActionPreference = "Stop"
$nand = [System.IO.Path]::GetFullPath($NandPath)
$archive = [System.IO.Path]::GetFullPath($ArchivePath)

if (-not (Test-Path -LiteralPath $nand -PathType Leaf)) {
    throw "NAND image is missing: $nand"
}
if ((Get-Item -LiteralPath $nand).Length -ne 276824064) {
    throw "Unexpected NAND image size; expected 276824064 bytes"
}
if (Test-Path -LiteralPath $archive) {
    throw "Archive already exists: $archive"
}

$work = Join-Path ([System.IO.Path]::GetDirectoryName($archive)) `
    ("nand-package-" + [System.Guid]::NewGuid().ToString("N"))
$runtime = Join-Path $work "runtime"
New-Item -ItemType Directory -Path $runtime | Out-Null

try {
    Copy-Item -LiteralPath $nand -Destination (Join-Path $runtime "nand-user.raw")
    $rawHash = (Get-FileHash -LiteralPath $nand -Algorithm SHA256).Hash.ToLowerInvariant()
    $readme = @"
BBK 9288S test NAND image

File: nand-user.raw
Size: 276,824,064 bytes (256 MiB data plus 64-byte OOB per page)
SHA-256: $rawHash

This image is intended only for emulator testing. Back it up before modifying it.
The board loader finds kernel.bin inside this NAND image at startup.
"@
    Set-Content -LiteralPath (Join-Path $runtime "README.txt") `
        -Value $readme -Encoding utf8
    Compress-Archive -LiteralPath $runtime -DestinationPath $archive `
        -CompressionLevel Optimal
} finally {
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}

$archiveHash = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
Set-Content -LiteralPath "$archive.sha256" `
    -Value "$archiveHash  $([System.IO.Path]::GetFileName($archive))" `
    -Encoding ascii

Write-Host "NAND archive: $archive"
Write-Host "Raw SHA-256: $rawHash"
Write-Host "Archive SHA-256: $archiveHash"
