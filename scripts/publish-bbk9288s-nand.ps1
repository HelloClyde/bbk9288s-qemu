param(
    [Parameter(Mandatory)]
    [string]$NandPath,

    [string]$Repository,

    [string]$Tag = "nand-test-image"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$dist = Join-Path $root "dist"
$archive = Join-Path $dist "bbk9288s-test-nand.zip"

New-Item -ItemType Directory -Force -Path $dist | Out-Null
Remove-Item -LiteralPath $archive, "$archive.sha256" `
    -Force -ErrorAction SilentlyContinue

& (Join-Path $root "scripts\package-bbk9288s-nand.ps1") `
    -NandPath $NandPath -ArchivePath $archive

$repoArgs = if ($Repository) { @("--repo", $Repository) } else { @() }
$releaseNotes = @"
Persistent NAND image for emulator testing. The board loader reads kernel.bin
directly from the FAT16 filesystem inside this image. Verify redistribution
rights before replacing or publishing this asset.
"@
& gh auth status
if ($LASTEXITCODE -ne 0) {
    throw "GitHub CLI authentication is required"
}

& gh release view $Tag @repoArgs *> $null
if ($LASTEXITCODE -ne 0) {
    & gh release create $Tag @repoArgs `
        --title "BBK 9288S test NAND image" `
        --notes $releaseNotes `
        --prerelease
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to create NAND release"
    }
}

& gh release edit $Tag @repoArgs `
    --title "BBK 9288S test NAND image" `
    --notes $releaseNotes `
    --prerelease
if ($LASTEXITCODE -ne 0) {
    throw "Failed to update NAND release notes"
}

& gh release upload $Tag $archive "$archive.sha256" @repoArgs --clobber
if ($LASTEXITCODE -ne 0) {
    throw "Failed to upload NAND release assets"
}
