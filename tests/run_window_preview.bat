@echo off
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 2
cd /d "%~dp0.."
if not exist build\tests\preview mkdir build\tests\preview
cl /nologo /std:c++20 /EHsc /O2 /MT /W3 /utf-8 /Brepro /DUNICODE /D_UNICODE /Fobuild\tests\preview\ /Febuild\tests\preview\window_preview_test.exe tests\window_preview_test.cpp /link /Brepro user32.lib gdi32.lib dwmapi.lib || exit /b 1
if /i "%1"=="build-only" exit /b 0
build\tests\preview\window_preview_test.exe
exit /b %errorlevel%
