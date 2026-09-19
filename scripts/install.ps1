<#
.SYNOPSIS
    Installs the vegas-nvenc-fix shim into a VEGAS Pro installation.

.DESCRIPTION
    Most people should run install.cmd instead - it elevates itself, handles the
    PowerShell execution policy and builds the shim if needed. This script is
    what it calls.

    The shim is copied next to vegas<ver>.exe, where the Windows loader finds it
    ahead of %SystemRoot%\System32. Nothing in VEGAS is modified, and nothing in
    System32 is touched.

    Anything already at the destination is backed up, timestamped, before being
    replaced, and a manifest records what was done so uninstall can reverse it
    exactly.

.PARAMETER VegasPath
    VEGAS program folder. Auto-detected when omitted - registry, running
    process, Program Files, then common folders on every fixed drive.

.PARAMETER Interactive
    Allow prompting when several installations are found.

.PARAMETER Force
    Reinstall even when the same build is already in place.
#>
[CmdletBinding()]
param(
    [string] $VegasPath,
    [switch] $Interactive,
    [switch] $Force
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Find-Vegas.ps1')

$DllName = 'nvEncodeAPI64.dll'
$Root    = Split-Path -Parent $PSScriptRoot
$Source  = Join-Path $Root "build\$DllName"

function Say    { param($m) Write-Host "    $m" }
function Step   { param($m) Write-Host "==> $m" -ForegroundColor Cyan }
function Good   { param($m) Write-Host "    $m" -ForegroundColor Green }
function Warn   { param($m) Write-Host "    $m" -ForegroundColor Yellow }
function Fail   { param($m) Write-Host "    $m" -ForegroundColor Red }

try {
    # ---- preconditions --------------------------------------------------
    if (-not (Test-Path $Source)) {
        throw "The shim is not built. Expected: $Source`nRun install.cmd, which builds it, or drop a released nvEncodeAPI64.dll there."
    }

    $isAdmin = ([Security.Principal.WindowsPrincipal] `
                [Security.Principal.WindowsIdentity]::GetCurrent()
              ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    if (-not $isAdmin) {
        throw "Administrator rights are required to write to the VEGAS folder.`nRun install.cmd instead - it asks for them automatically."
    }

    # ---- locate VEGAS ---------------------------------------------------
    Step 'Locating VEGAS Pro'
    if ([string]::IsNullOrWhiteSpace($VegasPath)) { $VegasPath = $null }
    $VegasPath = Find-VegasInstall -ExplicitPath $VegasPath -AllowInteractive:$Interactive

    $exe = Get-ChildItem $VegasPath -Filter 'vegas*.exe' | Select-Object -First 1
    Say "Host executable: $($exe.Name)"

    $running = Get-Process -Name $exe.BaseName -ErrorAction SilentlyContinue
    if ($running) {
        throw "$($exe.Name) is running (PID $($running.Id -join ', ')).`nClose VEGAS and run this again."
    }

    # Warn rather than fail: the shim targets the code in VEGAS 17-22's encoder
    # plug-in, and a much newer VEGAS may not need it or may differ.
    $plug = Join-Path $VegasPath 'FileIO Plug-Ins\mxavcaacplug\mxavcaacplug.dll'
    if (-not (Test-Path $plug)) {
        Warn "mxavcaacplug.dll not found in this installation."
        Warn "The shim will install but may have nothing to repair."
    }

    $Dest      = Join-Path $VegasPath $DllName
    $BackupDir = Join-Path $VegasPath 'vegas-nvenc-fix-backup'
    $srcHash   = (Get-FileHash $Source -Algorithm SHA256).Hash

    $manifest = [ordered]@{
        installedAt   = (Get-Date).ToString('o')
        installedBy   = 'vegas-nvenc-fix install.ps1'
        vegasPath     = $VegasPath
        dll           = $Dest
        preexisting   = $false
        backupOfPrior = $null
        sourceSha256  = $srcHash
    }

    # ---- back up anything already there ---------------------------------
    if (Test-Path $Dest) {
        if ((Get-FileHash $Dest -Algorithm SHA256).Hash -eq $srcHash -and -not $Force) {
            Good 'This exact build is already installed - nothing to do.'
            Say  'Use -Force to reinstall anyway.'
            exit 0
        }
        Step 'Backing up the file already at the destination'
        New-Item -ItemType Directory -Force -Path $BackupDir | Out-Null
        $backup = Join-Path $BackupDir "$DllName.$(Get-Date -Format 'yyyyMMdd-HHmmss').bak"
        Copy-Item $Dest $backup -Force
        $manifest.preexisting   = $true
        $manifest.backupOfPrior = $backup
        Good "Saved: $backup"
    } else {
        Say 'Nothing at the destination - clean install.'
    }

    # ---- install --------------------------------------------------------
    Step "Installing $DllName"
    Copy-Item $Source $Dest -Force
    if ((Get-FileHash $Dest -Algorithm SHA256).Hash -ne $srcHash) {
        throw "Copy verification failed for $Dest."
    }
    New-Item -ItemType Directory -Force -Path $BackupDir | Out-Null
    $manifest | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $BackupDir 'install-manifest.json') -Encoding UTF8

    Good "Installed: $Dest"
    Say  "SHA256: $srcHash"

    Write-Host ''
    Write-Host '  Installed successfully.' -ForegroundColor Green
    Write-Host ''
    Write-Host '  In VEGAS: Render As -> MAGIX AVC/AAC MP4 -> Customize Template'
    Write-Host '            Encode mode: NV Encoder'
    Write-Host '            Preset:      High quality   (recommended)'
    Write-Host ''
    Write-Host "  Log:       $env:LOCALAPPDATA\vegas-nvenc-fix\nvenc-shim.log"
    Write-Host '  To remove: uninstall.cmd'
    exit 0
}
catch {
    Write-Host ''
    Fail 'Install failed:'
    Write-Host ''
    Write-Host $_.Exception.Message
    Write-Host ''
    exit 1
}
