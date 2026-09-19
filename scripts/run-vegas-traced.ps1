<#
.SYNOPSIS
    Launches VEGAS with stderr captured, so the encoder plugin's own diagnostics
    are recorded.

.DESCRIPTION
    mxavcaacplug.dll reports its failures with fprintf to stderr - the logger at
    sub_180049E70 calls __acrt_iob_func(2) and then a CRT stdio routine, not
    OutputDebugString. VEGAS is a GUI application with no console attached, so
    every one of those messages is discarded, which is why capturing the Win32
    debug channel produced nothing.

    Starting the process with an inherited stderr handle makes the CRT write
    there instead. The messages that become visible include the exact reason the
    NVENC path gives up, for example:

        <path>\NvHWEncoder.cpp line 755: 10 bit is not supported with H264

    Nothing is installed or modified; this only changes how VEGAS is started.

.EXAMPLE
    .\run-vegas-traced.ps1
    # then render with NVENC, close VEGAS, and read the log it prints
#>
[CmdletBinding()]
param(
    [string] $VegasPath,
    [string] $OutFile = "$env:USERPROFILE\Desktop\vegas-stderr.log"
)

$ErrorActionPreference = 'Stop'

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

$exe = Get-ChildItem $VegasPath -Filter 'vegas*.exe' | Select-Object -First 1
if (-not $exe) { throw "No vegas*.exe in $VegasPath" }

if (Get-Process -Name $exe.BaseName -ErrorAction SilentlyContinue) {
    throw "$($exe.Name) is already running. Close it first - the capture only works for a process started by this script."
}

$outDir = Split-Path -Parent $OutFile
if ($outDir -and -not (Test-Path $outDir)) { New-Item -ItemType Directory -Force -Path $outDir | Out-Null }
$stdoutFile = [IO.Path]::ChangeExtension($OutFile, '.stdout.log')

Write-Host "==> Launching $($exe.Name) with stderr captured" -ForegroundColor Cyan
Write-Host "    stderr -> $OutFile"
Write-Host "    stdout -> $stdoutFile"
Write-Host ''
Write-Host '    Now: render with NVENC, let it fail, then CLOSE VEGAS.' -ForegroundColor Yellow
Write-Host '    (the log is flushed as the process exits)'
Write-Host ''

$p = Start-Process -FilePath $exe.FullName `
                   -WorkingDirectory $VegasPath `
                   -RedirectStandardError $OutFile `
                   -RedirectStandardOutput $stdoutFile `
                   -PassThru

Write-Host "    PID $($p.Id) - waiting for VEGAS to exit..." -ForegroundColor Gray
$p.WaitForExit()

Write-Host ''
Write-Host '==> VEGAS exited.' -ForegroundColor Cyan
foreach ($f in @($OutFile, $stdoutFile)) {
    if ((Test-Path $f) -and (Get-Item $f).Length -gt 0) {
        Write-Host "--- $f ---" -ForegroundColor Green
        Get-Content $f | Select-Object -Last 60
    } else {
        Write-Host "    $f is empty" -ForegroundColor Gray
    }
}
Write-Host ''
Write-Host 'Lines mentioning NvHWEncoder.cpp name the exact check that failed.' -ForegroundColor Yellow
