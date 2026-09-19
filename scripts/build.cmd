@echo off
rem Builds the shim DLL and the diagnostic probe with MSVC.
rem Usage: build.cmd [probe|dll|all]   (default: all)
rem
rem NOTE: this file deliberately avoids parenthesised IF blocks around
rem %ProgramFiles(x86)% - the ")" inside that variable name closes the block
rem early under cmd.exe.

setlocal
set "TARGET=%~1"
if "%TARGET%"=="" set "TARGET=all"

set "ROOT=%~dp0.."
pushd "%ROOT%"

rem Already inside a developer command prompt?
if defined VCINSTALLDIR goto :have_env
where cl.exe >nul 2>&1 && where ml64.exe >nul 2>&1 && goto :have_env

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto :no_vswhere

rem Capture vswhere's output through a temp file. A FOR /F backquote block
rem cannot reliably run a quoted path that itself contains parentheses.
set "VNFTMP=%TEMP%\vnf_vswhere_%RANDOM%.txt"
"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath > "%VNFTMP%" 2>nul

set "VSDIR="
if exist "%VNFTMP%" set /p VSDIR=<"%VNFTMP%"
del "%VNFTMP%" >nul 2>&1
if not defined VSDIR goto :no_toolset
if not exist "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" goto :no_toolset

rem stderr is discarded too: vcvars64.bat probes for helpers by bare name, which
rem emits harmless "not recognized" noise when NoDefaultCurrentDirectoryInExePath
rem is set on the machine.
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 goto :no_toolset

:have_env
if not exist build mkdir build

set "CFLAGS=/nologo /O2 /W3 /MT /DNDEBUG /D_CRT_SECURE_NO_WARNINGS /Ithird_party"

if /i "%TARGET%"=="probe" goto :probe
if /i "%TARGET%"=="dll"   goto :dll
if /i "%TARGET%"=="all"   goto :dll
echo Unknown target "%TARGET%"
goto :fail

:dll
echo === assembling NvTool pass-through thunks ===
ml64 /nologo /c /Fo build\nvenc_thunks.obj src\nvenc_thunks.asm
if errorlevel 1 goto :fail

echo.
echo === building nvEncodeAPI64.dll (shim) ===
cl %CFLAGS% /LD src\nvenc_shim.c build\nvenc_thunks.obj /Fe:build\nvEncodeAPI64.dll /Fo:build\ /Fd:build\ /link /DEF:src\nvenc_shim.def /OUT:build\nvEncodeAPI64.dll kernel32.lib
if errorlevel 1 goto :fail
if /i "%TARGET%"=="dll" goto :done

:probe
echo.
echo === building nvenc_probe.exe ===
cl %CFLAGS% tools\nvenc_probe.c /Fe:build\nvenc_probe.exe /Fo:build\ /Fd:build\ /link d3d11.lib
if errorlevel 1 goto :fail

echo.
echo === building nvenc_verify.exe ===
cl %CFLAGS% tools\nvenc_verify.c /Fe:build\nvenc_verify.exe /Fo:build\ /Fd:build\ /link d3d11.lib
if errorlevel 1 goto :fail

:done
echo.
echo === build complete -^> %ROOT%\build ===
dir /b build\*.dll build\*.exe 2>nul
popd
endlocal
exit /b 0

:no_vswhere
echo ERROR: vswhere.exe not found. Install Visual Studio 2022 (or Build Tools)
echo        with the "Desktop development with C++" workload.
goto :fail

:no_toolset
echo ERROR: no MSVC x64 toolset found in the Visual Studio installation.
goto :fail

:fail
echo.
echo *** BUILD FAILED ***
popd
endlocal
exit /b 1
