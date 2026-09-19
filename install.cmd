@echo off
setlocal
rem ---------------------------------------------------------------------------
rem  vegas-nvenc-fix - one-click installer
rem
rem  Double-click this. It asks for admin itself, finds VEGAS on any drive, and
rem  builds the shim first if a prebuilt one is not already present.
rem
rem  Optional: pass the VEGAS folder if it is somewhere unusual.
rem      install.cmd "D:\Wherever\VEGAS Pro 22.0"
rem ---------------------------------------------------------------------------

rem Re-launch elevated if we are not already. %~f0 keeps the path with spaces
rem intact; the extra argument marks the relaunch so it cannot loop.
net session >nul 2>&1
if %errorlevel% equ 0 goto :elevated
if "%~1"=="--elevated" goto :elevated

echo Requesting administrator rights ^(writing to the VEGAS folder needs them^)...
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "Start-Process -FilePath '%~f0' -ArgumentList '--elevated','%~1' -Verb RunAs"
exit /b 0

:elevated
rem Drop the marker so it is not mistaken for a path.
set "VEGASPATH=%~1"
if "%VEGASPATH%"=="--elevated" set "VEGASPATH=%~2"

cd /d "%~dp0"

echo.
echo  ================================================================
echo   vegas-nvenc-fix - restores NVENC rendering in VEGAS Pro 17-22
echo  ================================================================
echo.

rem Build only if there is no shim yet. A release ships one prebuilt, so most
rem people never need Visual Studio at all.
if exist "build\nvEncodeAPI64.dll" goto :haveshim
echo  No prebuilt shim found - building from source...
echo.
call "scripts\build.cmd" dll
if errorlevel 1 (
  echo.
  echo  BUILD FAILED.
  echo.
  echo  Either install Visual Studio 2022 with "Desktop development with C++",
  echo  or download a release that already contains nvEncodeAPI64.dll and put
  echo  it in the build\ folder next to this script.
  echo.
  pause
  exit /b 1
)
echo.

:haveshim
powershell -NoProfile -ExecutionPolicy Bypass -File "scripts\install.ps1" -VegasPath "%VEGASPATH%" -Interactive
set "RC=%errorlevel%"

echo.
if "%RC%"=="0" (
  echo  Done. Start VEGAS and render with NV Encoder.
) else (
  echo  Install did not complete - see the messages above.
)
echo.
pause
exit /b %RC%
