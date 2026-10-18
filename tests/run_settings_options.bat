@echo off
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 2
cd /d "%~dp0.."
if not exist build\tests\settings-options mkdir build\tests\settings-options
cl /nologo /std:c++20 /EHsc /O2 /MT /W3 /utf-8 /Brepro /DUNICODE /D_UNICODE ^
 /Fobuild\tests\settings-options\ /Febuild\tests\settings-options\settings_options_test.exe ^
 tests\settings_options_test.cpp src\common.cpp /link /Brepro /SUBSYSTEM:CONSOLE user32.lib || exit /b 1
build\tests\settings-options\settings_options_test.exe
exit /b %errorlevel%
