<#
.SYNOPSIS
    Locates VEGAS Pro installations. Dot-source this; it defines Find-VegasInstall.

.DESCRIPTION
    People install VEGAS wherever they like - a second SSD, a D: drive, a custom
    folder - so guessing C:\Program Files\VEGAS is not good enough. This looks in
    four independent places and merges the results:

      1. the uninstall registry, 64- and 32-bit views plus per-user
      2. a running vegas*.exe, whose image path is authoritative
      3. the usual Program Files locations
      4. every fixed drive, for VEGAS-shaped folders, as a last resort

    A folder only counts if it actually contains a vegas*.exe.
#>

function Get-VegasFromRegistry {
    $roots = @(
        'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall',
        'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall',
        'HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall'
    )
    foreach ($root in $roots) {
        if (-not (Test-Path $root)) { continue }
        Get-ChildItem $root -ErrorAction SilentlyContinue | ForEach-Object {
            $p = Get-ItemProperty $_.PSPath -ErrorAction SilentlyContinue
            if ($p.DisplayName -like 'VEGAS Pro*' -and $p.InstallLocation) {
                $p.InstallLocation.TrimEnd('\')
            }
        }
    }
}

function Get-VegasFromRunningProcess {
    Get-Process -ErrorAction SilentlyContinue |
        Where-Object { $_.ProcessName -like 'vegas*' } |
        ForEach-Object { try { Split-Path -Parent $_.MainModule.FileName } catch { } }
}

function Get-VegasFromProgramFiles {
    $bases = @("$env:ProgramFiles\VEGAS", "${env:ProgramFiles(x86)}\VEGAS",
               "$env:ProgramFiles\Sony",  "${env:ProgramFiles(x86)}\Sony",
               "$env:ProgramFiles",       "${env:ProgramFiles(x86)}")
    foreach ($b in $bases) {
        if (-not (Test-Path $b)) { continue }
        Get-ChildItem $b -Directory -Filter 'VEGAS Pro *' -ErrorAction SilentlyContinue |
            ForEach-Object { $_.FullName }
    }
}

function Get-VegasFromAllDrives {
    # Last resort, and deliberately shallow: a full recursive scan of every disk
    # would take minutes. VEGAS lives at a predictable depth even when moved.
    $drives = Get-CimInstance Win32_LogicalDisk -Filter 'DriveType=3' -ErrorAction SilentlyContinue |
              Select-Object -ExpandProperty DeviceID
    foreach ($d in $drives) {
        foreach ($pattern in @("$d\VEGAS", "$d\Program Files\VEGAS", "$d\Program Files (x86)\VEGAS",
                               "$d\Programs\VEGAS", "$d\Apps\VEGAS", "$d\Program Files", "$d\Programs", "$d\Apps")) {
            if (-not (Test-Path $pattern)) { continue }
            Get-ChildItem $pattern -Directory -Filter 'VEGAS Pro *' -ErrorAction SilentlyContinue |
                ForEach-Object { $_.FullName }
        }
    }
}

function Find-VegasInstall {
    [CmdletBinding()]
    param(
        [string] $ExplicitPath,
        [switch] $AllowInteractive
    )

    if ($ExplicitPath) {
        if (-not (Test-Path $ExplicitPath)) { throw "Path not found: $ExplicitPath" }
        if (-not (Get-ChildItem $ExplicitPath -Filter 'vegas*.exe' -ErrorAction SilentlyContinue)) {
            throw "$ExplicitPath contains no vegas*.exe - that is not a VEGAS program folder."
        }
        return (Resolve-Path $ExplicitPath).Path.TrimEnd('\')
    }

    $found = @()
    foreach ($src in @(
        @{ n = 'registry';       f = { Get-VegasFromRegistry } },
        @{ n = 'running VEGAS';  f = { Get-VegasFromRunningProcess } },
        @{ n = 'Program Files';  f = { Get-VegasFromProgramFiles } },
        @{ n = 'other drives';   f = { Get-VegasFromAllDrives } }
    )) {
        foreach ($p in (& $src.f)) {
            if (-not $p) { continue }
            if (-not (Test-Path $p)) { continue }
            if (-not (Get-ChildItem $p -Filter 'vegas*.exe' -ErrorAction SilentlyContinue)) { continue }
            $full = (Resolve-Path $p).Path.TrimEnd('\')
            if ($found.Path -notcontains $full) {
                $found += [pscustomobject]@{ Path = $full; Source = $src.n }
            }
        }
    }

    if ($found.Count -eq 0) {
        throw @"
No VEGAS Pro installation found.

Looked in the uninstall registry, any running VEGAS, Program Files, and common
folders on every fixed drive. If yours is somewhere unusual, pass it directly:

    .\install.cmd "D:\Wherever\VEGAS Pro 22.0"
"@
    }

    if ($found.Count -eq 1) {
        Write-Host "    Found VEGAS: $($found[0].Path)  (via $($found[0].Source))" -ForegroundColor Green
        return $found[0].Path
    }

    Write-Host ""
    Write-Host "  Several VEGAS installations found:" -ForegroundColor Yellow
    for ($i = 0; $i -lt $found.Count; $i++) {
        "    [{0}] {1}" -f ($i + 1), $found[$i].Path | Write-Host
    }
    Write-Host ""

    if (-not $AllowInteractive) {
        throw "Several installations found. Re-run with the one you want: .\install.cmd `"<path>`""
    }

    while ($true) {
        $sel = Read-Host "  Which one? (1-$($found.Count), or Q to quit)"
        if ($sel -match '^[Qq]') { throw 'Cancelled.' }
        if ($sel -match '^\d+$' -and [int]$sel -ge 1 -and [int]$sel -le $found.Count) {
            return $found[[int]$sel - 1].Path
        }
        Write-Host "    Enter a number between 1 and $($found.Count)." -ForegroundColor Red
    }
}
