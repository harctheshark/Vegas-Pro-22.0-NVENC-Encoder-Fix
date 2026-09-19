<#
.SYNOPSIS
    Installs the vegas-nvenc-fix shim next to a VEGAS Pro executable.

.DESCRIPTION
    Copies nvEncodeAPI64.dll into the VEGAS program folder, where the Windows
    loader finds it ahead of %SystemRoot%\System32. Nothing in the VEGAS
    installation or in System32 is modified.

    Anything already present at the destination is backed up, timestamped,
    before it is touched, and a manifest is written so that uninstall.ps1 can
    put the folder back exactly as it was.

    Requires an elevated shell, because the VEGAS folder lives under
    C:\Program Files.

.PARAMETER VegasPath
    VEGAS program folder. Auto-detected when omitted.

.PARAMETER Force
    Reinstall even if an identical shim is already in place.

.EXAMPLE
    .\install.ps1
.EXAMPLE
    .\install.ps1 -VegasPath "D:\Apps\VEGAS Pro 21.0"
#>
[CmdletBinding()]
param(
    [string] $VegasPath,
    [switch] $Force
)

$ErrorActionPreference = 'Stop'

$DllName   = 'nvEncodeAPI64.dll'
$Root      = Split-Path -Parent $PSScriptRoot
$Source    = Join-Path $Root "build\$DllName"
$Stamp     = Get-Date -Format 'yyyyMMdd-HHmmss'

function Write-Step { param($m) Write-Host "==> $m" -ForegroundColor Cyan }
function Write-Ok   { param($m) Write-Host "    $m" -ForegroundColor Green }
function Write-Warn { param($m) Write-Host "    $m" -ForegroundColor Yellow }

# --- preconditions --------------------------------------------------------
if (-not (Test-Path $Source)) {
    throw "Shim not built. Run scripts\build.cmd first (expected $Source)."
}

$isAdmin = ([Security.Principal.WindowsPrincipal] `
            [Security.Principal.WindowsIdentity]::GetCurrent()
          ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

# --- locate VEGAS ---------------------------------------------------------
if (-not $VegasPath) {
    Write-Step 'Locating VEGAS Pro'
    $candidates = @()
    foreach ($base in @("$env:ProgramFiles\VEGAS", "$env:ProgramFiles\Sony",
                        "${env:ProgramFiles(x86)}\VEGAS", "${env:ProgramFiles(x86)}\Sony")) {
        if (Test-Path $base) {
            $candidates += Get-ChildItem $base -Directory -Filter 'VEGAS Pro *' -ErrorAction SilentlyContinue
        }
    }
    $found = $candidates | Where-Object {
        Get-ChildItem $_.FullName -Filter 'vegas*.exe' -ErrorAction SilentlyContinue
    }
    if (-not $found)            { throw "No VEGAS Pro installation found. Pass -VegasPath explicitly." }
    if ($found.Count -gt 1) {
        Write-Warn "Several installations found:"
        $found | ForEach-Object { Write-Warn "  $($_.FullName)" }
        throw "Pass -VegasPath to choose one."
    }
    $VegasPath = $found[0].FullName
}

if (-not (Test-Path $VegasPath)) { throw "Path not found: $VegasPath" }
$exe = Get-ChildItem $VegasPath -Filter 'vegas*.exe' -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $exe) { throw "$VegasPath does not look like a VEGAS program folder (no vegas*.exe)." }
Write-Ok "VEGAS: $VegasPath"
Write-Ok "Host executable: $($exe.Name)"

# --- safety checks --------------------------------------------------------
if (-not $isAdmin) {
    throw "Administrator rights are required to write to $VegasPath. Re-run this script from an elevated PowerShell."
}

$running = Get-Process -Name ($exe.BaseName) -ErrorAction SilentlyContinue
if ($running) {
    throw "$($exe.Name) is running (PID $($running.Id -join ', ')). Close VEGAS and try again."
}

$Dest = Join-Path $VegasPath $DllName

# --- back up before touching anything ------------------------------------
$BackupDir = Join-Path $VegasPath 'vegas-nvenc-fix-backup'
$manifest = [ordered]@{
    installedAt   = (Get-Date).ToString('o')
    installedBy   = 'vegas-nvenc-fix install.ps1'
    vegasPath     = $VegasPath
    dll           = $Dest
    preexisting   = $false
    backupOfPrior = $null
    sourceSha256  = (Get-FileHash $Source -Algorithm SHA256).Hash
}

if (Test-Path $Dest) {
    $existingHash = (Get-FileHash $Dest -Algorithm SHA256).Hash
    if ($existingHash -eq $manifest.sourceSha256 -and -not $Force) {
        Write-Ok 'This exact shim is already installed. Nothing to do (use -Force to reinstall).'
        return
    }
    Write-Step 'Backing up the file already at the destination'
    New-Item -ItemType Directory -Force -Path $BackupDir | Out-Null
    $backup = Join-Path $BackupDir "$DllName.$Stamp.bak"
    Copy-Item $Dest $backup -Force
    $manifest.preexisting   = $true
    $manifest.backupOfPrior = $backup
    Write-Ok "Saved: $backup"
} else {
    Write-Ok 'No existing file at the destination - this is a clean addition.'
}

# --- install --------------------------------------------------------------
Write-Step "Installing $DllName"
Copy-Item $Source $Dest -Force

$destHash = (Get-FileHash $Dest -Algorithm SHA256).Hash
if ($destHash -ne $manifest.sourceSha256) { throw "Copy verification failed for $Dest." }
Write-Ok "Installed: $Dest"
Write-Ok "SHA256: $destHash"

New-Item -ItemType Directory -Force -Path $BackupDir | Out-Null
$manifestPath = Join-Path $BackupDir 'install-manifest.json'
$manifest | ConvertTo-Json -Depth 4 | Set-Content $manifestPath -Encoding UTF8
Write-Ok "Manifest: $manifestPath"

Write-Host ''
Write-Host 'Done.' -ForegroundColor Green
Write-Host ''
Write-Host '  Start VEGAS and render again. The shim writes a log to:' -ForegroundColor Gray
Write-Host "    $env:LOCALAPPDATA\vegas-nvenc-fix\nvenc-shim.log" -ForegroundColor Gray
Write-Host ''
Write-Host '  To remove it:        .\scripts\uninstall.ps1' -ForegroundColor Gray
Write-Host '  To disable in place: set VEGAS_NVENC_FIX_DISABLE=1' -ForegroundColor Gray
