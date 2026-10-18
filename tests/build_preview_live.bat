@echo off
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 2
cd /d "%~dp0.."
if not exist build\tests\preview-live mkdir build\tests\preview-live
cl /nologo /std:c++20 /EHsc /O2 /MT /W3 /utf-8 /Brepro /DUNICODE /D_UNICODE /Fobuild\tests\preview-live\ /Febuild\tests\preview-live\preview_live_test.exe tests\preview_live_test.cpp build\apps.obj build\gfx.obj build\common.obj /link /Brepro d3d11.lib dxgi.lib d2d1.lib dwrite.lib dcomp.lib d3dcompiler.lib dxguid.lib windowsapp.lib shell32.lib ole32.lib oleaut32.lib user32.lib gdi32.lib dwmapi.lib shlwapi.lib propsys.lib version.lib advapi32.lib shcore.lib windowscodecs.lib uuid.lib || exit /b 1
