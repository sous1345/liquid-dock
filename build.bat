@echo off
rem Builds the dock, the optional sensor helper and LiquidDock-Setup.exe.
rem   build.bat        release build
rem   build.bat dev    adds developer commands (--debug-*, --snap, --rec-*)
setlocal
set "VSDEV=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VSDEV%" for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set "VSDEV=%%i\VC\Auxiliary\Build\vcvars64.bat"
call "%VSDEV%" >nul || exit /b 1
cd /d "%~dp0"
if not exist build mkdir build
if not exist build\sensors mkdir build\sensors
if not exist build\setup mkdir build\setup

set "DEFS=/DUNICODE /D_UNICODE"
if /i "%1"=="dev" set "DEFS=%DEFS% /DLD_DEV"
rem /Brepro: no build timestamps; no /Zi: no PDB paths in the binaries
set "CF=/nologo /std:c++20 /EHsc /O2 /MT /W3 /utf-8 /Brepro %DEFS%"
rem /MANIFEST:EMBED: copies of the exe (uninstall.exe, installed helper) must carry their manifest
set "LF=/Brepro /SUBSYSTEM:WINDOWS /MANIFEST:EMBED"

rem ---- dock
rc /nologo /fo build\dock.res src\dock.rc || exit /b 1
cl %CF% /Fobuild\ /Febuild\LiquidDock.exe src\*.cpp build\dock.res ^
  /link %LF% d3d11.lib dxgi.lib d2d1.lib dwrite.lib dcomp.lib d3dcompiler.lib dxguid.lib windowsapp.lib ^
  shell32.lib ole32.lib oleaut32.lib user32.lib gdi32.lib dwmapi.lib shlwapi.lib propsys.lib version.lib ^
  iphlpapi.lib advapi32.lib shcore.lib windowscodecs.lib uuid.lib pdh.lib psapi.lib || exit /b 1

rem ---- optional elevated sensor helper (embeds the PawnIO modules); skipped while it is running
tasklist /FI "IMAGENAME eq LiquidDockSensors.exe" | find /I "LiquidDockSensors.exe" >nul && (
  echo sensor helper is running - skipped rebuilding it and the installer
  exit /b 0
)
rc /nologo /fo build\sensors\sensors.res sensors\sensors.rc || exit /b 1
cl %CF% /Fobuild\sensors\ /Febuild\LiquidDockSensors.exe sensors\sensors_main.cpp build\sensors\sensors.res ^
  /link %LF% /MANIFESTUAC:"level='requireAdministrator' uiAccess='false'" /DEPENDENTLOADFLAG:0x800 ^
  user32.lib advapi32.lib pdh.lib secur32.lib shell32.lib ole32.lib || exit /b 1

rem ---- installer (embeds the two programs above)
rc /nologo /fo build\setup\setup.res installer\setup.rc || exit /b 1
cl %CF% /Fobuild\setup\ /Febuild\LiquidDock-Setup.exe installer\setup_main.cpp build\setup\setup.res ^
  /link %LF% user32.lib advapi32.lib shell32.lib ole32.lib comctl32.lib || exit /b 1
exit /b 0
