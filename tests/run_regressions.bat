@echo off
setlocal
set "VSDEV=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VSDEV%" exit /b 2
call "%VSDEV%" >nul || exit /b 2
cd /d "%~dp0.."
if not exist build\tests mkdir build\tests
set "CF=/nologo /std:c++20 /EHsc /O2 /MT /W3 /utf-8 /Brepro /DUNICODE /D_UNICODE /Fobuild\tests\"
cl %CF% /Febuild\tests\taskbar_recovery_test.exe tests\taskbar_recovery_test.cpp /link /Brepro user32.lib gdi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib iphlpapi.lib advapi32.lib || exit /b 1
cl %CF% /Febuild\tests\installer_cleanup_test.exe tests\installer_cleanup_test.cpp /link /Brepro user32.lib advapi32.lib shell32.lib ole32.lib comctl32.lib || exit /b 1
build\tests\taskbar_recovery_test.exe || exit /b 1
build\tests\installer_cleanup_test.exe || exit /b 1
exit /b 0
