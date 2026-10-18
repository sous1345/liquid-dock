@echo off
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 2
cd /d "%~dp0.."
if not exist build\tests\workspace mkdir build\tests\workspace
cl /nologo /std:c++20 /EHsc /O2 /MT /W3 /utf-8 /Brepro /DUNICODE /D_UNICODE /Fobuild\tests\workspace\ /Febuild\tests\workspace\overflow_workspace_test.exe tests\overflow_workspace_test.cpp src\system.cpp src\common.cpp /link /Brepro user32.lib gdi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib iphlpapi.lib advapi32.lib || exit /b 1
