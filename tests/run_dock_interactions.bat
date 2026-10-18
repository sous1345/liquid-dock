@echo off
rem Run build.bat first. Test fixtures are hidden and never inject mouse/keyboard input.
setlocal
set "VSDEV=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VSDEV%" exit /b 2
call "%VSDEV%" >nul || exit /b 2
cd /d "%~dp0.."
for %%S in (apps capture common dock_record gfx native_tray sensors system window_preview) do if not exist build\%%S.obj (
  echo Run build.bat before this test.
  exit /b 2
)
if not exist build\tests\dock-interaction mkdir build\tests\dock-interaction
cl /nologo /std:c++20 /EHsc /O2 /MT /W3 /utf-8 /Brepro /DUNICODE /D_UNICODE ^
 /Fobuild\tests\dock-interaction\ /Febuild\tests\dock-interaction\dock_interaction_test.exe tests\dock_interaction_test.cpp ^
 build\apps.obj build\capture.obj build\common.obj build\dock_record.obj build\gfx.obj build\native_tray.obj build\sensors.obj build\system.obj build\window_preview.obj ^
 /link /Brepro /SUBSYSTEM:CONSOLE d3d11.lib dxgi.lib d2d1.lib dwrite.lib dcomp.lib d3dcompiler.lib dxguid.lib windowsapp.lib ^
 shell32.lib ole32.lib oleaut32.lib user32.lib gdi32.lib dwmapi.lib shlwapi.lib propsys.lib version.lib ^
 iphlpapi.lib advapi32.lib shcore.lib windowscodecs.lib uuid.lib pdh.lib || exit /b 1
build\tests\dock-interaction\dock_interaction_test.exe %*
exit /b %errorlevel%
