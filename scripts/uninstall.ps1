<#
.SYNOPSIS
    Removes the vegas-nvenc-fix shim and restores whatever was there before.

.DESCRIPTION
    Reads the manifest written by install.ps1, removes the shim DLL, and if a
    file had been displaced during install, puts the original back.

    Refuses to delete a DLL it did not install: the file at the destination must
    hash-match the one recorded in the manifest, unless -Force is given.

.PARAMETER VegasPath
    VEGAS program folder. Auto-detected when omitted.

.PARAMETER Force
    Remove the DLL even when it does not match the manifest.
#>
[CmdletBinding()]
param(
    [string] $VegasPath,
    [switch] $Force
)

$ErrorActionPreference = 'Stop'
$DllName = 'nvEncodeAPI64.dll'

function Write-Step { param($m) Write-Host "==> $m" -ForegroundColor Cyan }
function Write-Ok   { param($m) Write-Host "    $m" -ForegroundColor Green }
function Write-Warn { param($m) Write-Host "    $m" -ForegroundColor Yellow }

$isAdmin = ([Security.Principal.WindowsPrincipal] `
            [Security.Principal.WindowsIdentity]::GetCurrent()
          ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

if (-not $VegasPath) {
    Write-Step 'Locating VEGAS Pro'
    $candidates = @()
    foreach ($base in @("$env:ProgramFiles\VEGAS", "$env:ProgramFiles\Sony",
                        "${env:ProgramFiles(x86)}\VEGAS", "${env:ProgramFiles(x86)}\Sony")) {
        if (Test-Path $base) {
            $candidates += Get-ChildItem $base -Directory -Filter 'VEGAS Pro *' -ErrorAction SilentlyContinue
        }
    }
    $found = $candidates | Where-Object { Test-Path (Join-Path $_.FullName $DllName) }
    if (-not $found)          { throw "No VEGAS installation with $DllName found. Pass -VegasPath." }
    if ($found.Count -gt 1)   { throw "Several matches. Pass -VegasPath to choose one." }
    $VegasPath = $found[0].FullName
}

$Dest       = Join-Path $VegasPath $DllName
$BackupDir  = Join-Path $VegasPath 'vegas-nvenc-fix-backup'
$ManifestPath = Join-Path $BackupDir 'install-manifest.json'

Write-Ok "VEGAS: $VegasPath"

if (-not (Test-Path $Dest)) {
    Write-Ok "$DllName is not present - nothing to remove."
    return
}

if (-not $isAdmin) {
    throw "Administrator rights are required to modify $VegasPath. Re-run from an elevated PowerShell."
}

$exe = Get-ChildItem $VegasPath -Filter 'vegas*.exe' -ErrorAction SilentlyContinue | Select-Object -First 1
if ($exe) {
    $running = Get-Process -Name ($exe.BaseName) -ErrorAction SilentlyContinue
    if ($running) { throw "$($exe.Name) is running. Close VEGAS and try again." }
}

# --- verify this is our file before deleting anything ---------------------
$manifest = $null
if (Test-Path $ManifestPath) {
    $manifest = Get-Content $ManifestPath -Raw | ConvertFrom-Json
}

$destHash = (Get-FileHash $Dest -Algorithm SHA256).Hash
if ($manifest -and $manifest.sourceSha256 -and $destHash -ne $manifest.sourceSha256) {
    if (-not $Force) {
        Write-Warn "The file at $Dest does not match the manifest."
        Write-Warn "  manifest: $($manifest.sourceSha256)"
        Write-Warn "  on disk:  $destHash"
        throw "Refusing to delete a file this script did not install. Re-run with -Force if you are sure."
    }
    Write-Warn 'Hash mismatch overridden by -Force.'
}
if (-not $manifest -and -not $Force) {
    Write-Warn "No manifest found at $ManifestPath."
    throw "Refusing to delete $Dest without a manifest. Re-run with -Force if you are sure."
}

# --- keep a copy of what we remove, then remove it ------------------------
Write-Step 'Removing the shim'
New-Item -ItemType Directory -Force -Path $BackupDir | Out-Null
$removedCopy = Join-Path $BackupDir "$DllName.removed-$(Get-Date -Format 'yyyyMMdd-HHmmss').bak"
Copy-Item $Dest $removedCopy -Force
Remove-Item $Dest -Force
Write-Ok "Removed: $Dest"
Write-Ok "Copy kept at: $removedCopy"

# --- restore anything the install displaced -------------------------------
if ($manifest -and $manifest.preexisting -and $manifest.backupOfPrior) {
    if (Test-Path $manifest.backupOfPrior) {
        Write-Step 'Restoring the file that was there before install'
        Copy-Item $manifest.backupOfPrior $Dest -Force
        Write-Ok "Restored: $Dest"
    } else {
        Write-Warn "Manifest references a backup that is missing: $($manifest.backupOfPrior)"
    }
}

Write-Host ''
Write-Host 'Done. VEGAS will use the system NVENC DLL again.' -ForegroundColor Green
Write-Host "Backups remain in: $BackupDir" -ForegroundColor Gray
