<#
.SYNOPSIS
    Installs or removes the nvcuda.dll diagnostic proxy in the VEGAS folder.

.DESCRIPTION
    This is a DIAGNOSTIC, not part of the fix. It logs the CUDA calls VEGAS's
    encoder plugin makes - device enumeration, compute capability and context
    lifetime - which all happen before the first NVENC call and are therefore
    invisible to the NVENC shim.

    It forwards all 769 nvcuda exports unchanged and alters no behaviour, but it
    sits in front of a core driver library for one application, so remove it
    once the question is answered.

    Anything already at the destination is backed up, timestamped, before it is
    touched, and removal verifies the file's hash against the manifest first.

.EXAMPLE
    .\cuda-trace.ps1 -Install
.EXAMPLE
    .\cuda-trace.ps1 -Remove
#>
[CmdletBinding(DefaultParameterSetName = 'Install')]
param(
    [Parameter(ParameterSetName = 'Install')] [switch] $Install,
    [Parameter(ParameterSetName = 'Remove')]  [switch] $Remove,
    [string] $VegasPath,
    [switch] $Force
)

$ErrorActionPreference = 'Stop'
$DllName = 'nvcuda.dll'
$Root    = Split-Path -Parent $PSScriptRoot
$Source  = Join-Path $Root "build\$DllName"

function Write-Step { param($m) Write-Host "==> $m" -ForegroundColor Cyan }
function Write-Ok   { param($m) Write-Host "    $m" -ForegroundColor Green }
function Write-Warn { param($m) Write-Host "    $m" -ForegroundColor Yellow }

$isAdmin = ([Security.Principal.WindowsPrincipal] `
            [Security.Principal.WindowsIdentity]::GetCurrent()
          ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

if (-not $VegasPath) {
    $cands = @()
    foreach ($base in @("$env:ProgramFiles\VEGAS", "${env:ProgramFiles(x86)}\VEGAS")) {
        if (Test-Path $base) {
            $cands += Get-ChildItem $base -Directory -Filter 'VEGAS Pro *' -ErrorAction SilentlyContinue
        }
    }
    $found = $cands | Where-Object { Get-ChildItem $_.FullName -Filter 'vegas*.exe' -ErrorAction SilentlyContinue }
    if (-not $found)        { throw 'No VEGAS Pro installation found. Pass -VegasPath.' }
    if ($found.Count -gt 1) { throw 'Several installations found. Pass -VegasPath.' }
    $VegasPath = $found[0].FullName
}
Write-Ok "VEGAS: $VegasPath"

$Dest      = Join-Path $VegasPath $DllName
$BackupDir = Join-Path $VegasPath 'vegas-nvenc-fix-backup'
$Manifest  = Join-Path $BackupDir 'cuda-trace-manifest.json'

if (-not $isAdmin) { throw "Administrator rights are required to modify $VegasPath." }

$exe = Get-ChildItem $VegasPath -Filter 'vegas*.exe' -ErrorAction SilentlyContinue | Select-Object -First 1
if ($exe) {
    $running = Get-Process -Name $exe.BaseName -ErrorAction SilentlyContinue
    if ($running) { throw "$($exe.Name) is running. Close VEGAS and try again." }
}

# ---------------------------------------------------------------- install --
if ($PSCmdlet.ParameterSetName -eq 'Install' -or $Install) {
    if (-not (Test-Path $Source)) {
        throw "Not built. Run:  scripts\build.cmd cuda   (expected $Source)"
    }

    $m = [ordered]@{
        installedAt   = (Get-Date).ToString('o')
        purpose       = 'diagnostic CUDA call trace - remove when finished'
        vegasPath     = $VegasPath
        dll           = $Dest
        preexisting   = $false
        backupOfPrior = $null
        sourceSha256  = (Get-FileHash $Source -Algorithm SHA256).Hash
    }

    if (Test-Path $Dest) {
        Write-Step 'Backing up the file already at the destination'
        New-Item -ItemType Directory -Force -Path $BackupDir | Out-Null
        $b = Join-Path $BackupDir "$DllName.$(Get-Date -Format 'yyyyMMdd-HHmmss').bak"
        Copy-Item $Dest $b -Force
        $m.preexisting = $true; $m.backupOfPrior = $b
        Write-Ok "Saved: $b"
    } else {
        Write-Ok 'No existing file at the destination - clean addition.'
    }

    Write-Step "Installing the $DllName trace proxy"
    Copy-Item $Source $Dest -Force
    if ((Get-FileHash $Dest -Algorithm SHA256).Hash -ne $m.sourceSha256) {
        throw "Copy verification failed for $Dest."
    }
    New-Item -ItemType Directory -Force -Path $BackupDir | Out-Null
    $m | ConvertTo-Json -Depth 4 | Set-Content $Manifest -Encoding UTF8
    Write-Ok "Installed: $Dest"

    Write-Host ''
    Write-Host 'Now start VEGAS and reproduce the failure.' -ForegroundColor Green
    Write-Host "  CUDA trace : $env:LOCALAPPDATA\vegas-nvenc-fix\cuda-shim.log" -ForegroundColor Gray
    Write-Host "  NVENC trace: $env:LOCALAPPDATA\vegas-nvenc-fix\nvenc-shim.log" -ForegroundColor Gray
    Write-Host ''
    Write-Host 'Remove it afterwards:  .\scripts\cuda-trace.ps1 -Remove' -ForegroundColor Yellow
    return
}

# ----------------------------------------------------------------- remove --
if (-not (Test-Path $Dest)) { Write-Ok "$DllName is not present - nothing to remove."; return }

$man = if (Test-Path $Manifest) { Get-Content $Manifest -Raw | ConvertFrom-Json } else { $null }
$hash = (Get-FileHash $Dest -Algorithm SHA256).Hash

if ($man -and $man.sourceSha256 -and $hash -ne $man.sourceSha256 -and -not $Force) {
    Write-Warn "manifest: $($man.sourceSha256)"
    Write-Warn "on disk:  $hash"
    throw "The file at $Dest is not the proxy this script installed. Re-run with -Force if you are sure."
}
if (-not $man -and -not $Force) {
    throw "No manifest at $Manifest. Refusing to delete $Dest. Re-run with -Force if you are sure."
}

Write-Step 'Removing the CUDA trace proxy'
Remove-Item $Dest -Force
Write-Ok "Removed: $Dest"

if ($man -and $man.preexisting -and $man.backupOfPrior -and (Test-Path $man.backupOfPrior)) {
    Copy-Item $man.backupOfPrior $Dest -Force
    Write-Ok "Restored the previous file: $Dest"
}
Write-Host ''
Write-Host 'Done. VEGAS will use the system nvcuda.dll again.' -ForegroundColor Green
