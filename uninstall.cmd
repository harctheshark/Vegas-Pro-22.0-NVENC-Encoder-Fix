@echo off
setlocal
rem ---------------------------------------------------------------------------
rem  vegas-nvenc-fix - one-click uninstaller
rem
rem  Double-click this. It asks for admin itself and removes the shim, putting
rem  back anything that was displaced when it was installed.
rem ---------------------------------------------------------------------------

net session >nul 2>&1
if %errorlevel% equ 0 goto :elevated
if "%~1"=="--elevated" goto :elevated

echo Requesting administrator rights...
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "Start-Process -FilePath '%~f0' -ArgumentList '--elevated','%~1' -Verb RunAs"
exit /b 0

:elevated
set "VEGASPATH=%~1"
if "%VEGASPATH%"=="--elevated" set "VEGASPATH=%~2"

cd /d "%~dp0"

echo.
echo  ================================================================
echo   vegas-nvenc-fix - uninstall
echo  ================================================================
echo.

powershell -NoProfile -ExecutionPolicy Bypass -File "scripts\uninstall.ps1" -VegasPath "%VEGASPATH%" -Interactive
set "RC=%errorlevel%"

echo.
pause
exit /b %RC%
