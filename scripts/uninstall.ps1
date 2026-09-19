<#
.SYNOPSIS
    Removes the vegas-nvenc-fix shim and restores whatever it displaced.

.DESCRIPTION
    Most people should run uninstall.cmd instead - it elevates itself and
    handles the PowerShell execution policy. This script is what it calls.

    Refuses to delete a DLL it did not install: the file must hash-match the
    manifest written at install time, unless -Force is given.

.PARAMETER VegasPath
    VEGAS program folder. Auto-detected when omitted.

.PARAMETER Interactive
    Allow prompting when several installations are found.

.PARAMETER Force
    Remove the DLL even when it does not match the manifest.
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

function Say  { param($m) Write-Host "    $m" }
function Step { param($m) Write-Host "==> $m" -ForegroundColor Cyan }
function Good { param($m) Write-Host "    $m" -ForegroundColor Green }
function Warn { param($m) Write-Host "    $m" -ForegroundColor Yellow }
function Fail { param($m) Write-Host "    $m" -ForegroundColor Red }

try {
    $isAdmin = ([Security.Principal.WindowsPrincipal] `
                [Security.Principal.WindowsIdentity]::GetCurrent()
              ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

    Step 'Locating VEGAS Pro'
    if ([string]::IsNullOrWhiteSpace($VegasPath)) { $VegasPath = $null }
    $VegasPath = Find-VegasInstall -ExplicitPath $VegasPath -AllowInteractive:$Interactive

    $Dest      = Join-Path $VegasPath $DllName
    $BackupDir = Join-Path $VegasPath 'vegas-nvenc-fix-backup'
    $Manifest  = Join-Path $BackupDir 'install-manifest.json'

    if (-not (Test-Path $Dest)) {
        Good "$DllName is not present here - nothing to remove."
        exit 0
    }
    if (-not $isAdmin) {
        throw "Administrator rights are required.`nRun uninstall.cmd instead - it asks for them automatically."
    }

    $exe = Get-ChildItem $VegasPath -Filter 'vegas*.exe' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($exe) {
        $running = Get-Process -Name $exe.BaseName -ErrorAction SilentlyContinue
        if ($running) { throw "$($exe.Name) is running. Close VEGAS and run this again." }
    }

    # ---- only delete what we put there ----------------------------------
    $man  = if (Test-Path $Manifest) { Get-Content $Manifest -Raw | ConvertFrom-Json } else { $null }
    $hash = (Get-FileHash $Dest -Algorithm SHA256).Hash

    if ($man -and $man.sourceSha256 -and $hash -ne $man.sourceSha256 -and -not $Force) {
        Warn "The file here does not match the install manifest."
        Warn "  manifest: $($man.sourceSha256)"
        Warn "  on disk:  $hash"
        throw "Refusing to delete a file this script did not install. Use -Force if you are sure."
    }
    if (-not $man -and -not $Force) {
        Warn "No manifest at $Manifest"
        throw "Refusing to delete $Dest without a manifest. Use -Force if you are sure."
    }

    Step 'Removing the shim'
    New-Item -ItemType Directory -Force -Path $BackupDir | Out-Null
    $kept = Join-Path $BackupDir "$DllName.removed-$(Get-Date -Format 'yyyyMMdd-HHmmss').bak"
    Copy-Item $Dest $kept -Force
    Remove-Item $Dest -Force
    Good "Removed: $Dest"
    Say  "A copy was kept at: $kept"

    if ($man -and $man.preexisting -and $man.backupOfPrior) {
        if (Test-Path $man.backupOfPrior) {
            Step 'Restoring the file that was here before'
            Copy-Item $man.backupOfPrior $Dest -Force
            Good "Restored: $Dest"
        } else {
            Warn "Manifest references a missing backup: $($man.backupOfPrior)"
        }
    }

    Write-Host ''
    Write-Host '  Removed. VEGAS will use the system NVENC library again,' -ForegroundColor Green
    Write-Host '  which means the original render error returns.' -ForegroundColor Green
    Write-Host ''
    Write-Host "  Backups remain in: $BackupDir"
    exit 0
}
catch {
    Write-Host ''
    Fail 'Uninstall failed:'
    Write-Host ''
    Write-Host $_.Exception.Message
    Write-Host ''
    exit 1
}
