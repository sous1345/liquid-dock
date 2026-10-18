// LiquidDock-Setup.exe: per-user installer (no admin) with the program files embedded.
//   (no args)    install to %LOCALAPPDATA%\Programs\LiquidDock, start-menu shortcut, autostart, launch
//   /uninstall   (run as uninstall.exe from the install folder) remove everything and restore the taskbar
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <string>
#include <vector>
#include "resource.h"
#include "../src/version.h"

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

static const wchar_t* kTitle = L"Liquid Dock";
static bool g_silent = false;   // /silent: no dialogs (scripted installs and tests)
static const wchar_t* kUninstKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\LiquidDock";
static const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

static std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr; std::wstring s;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &p))) { s = p; CoTaskMemFree(p); }
    return s;
}
static std::wstring InstallDir() { return KnownFolder(FOLDERID_UserProgramFiles) + L"\\LiquidDock"; }
static std::wstring ShortcutPath() { return KnownFolder(FOLDERID_Programs) + L"\\Liquid Dock.lnk"; }
static std::wstring RestoreShortcutPath() { return KnownFolder(FOLDERID_Programs) + L"\\Liquid Dock - 작업표시줄 복원.lnk"; }
static std::wstring SelfPath() { wchar_t p[MAX_PATH]; GetModuleFileNameW(nullptr, p, MAX_PATH); return p; }

// Retire the old directory before scheduling cleanup. A new install may immediately
// reuse InstallDir(), but it can never be the target of this generation's cleanup.
static bool RetireInstallation(const std::wstring& dir, std::wstring& retired) {
    retired.clear();
    DWORD attrs = GetFileAttributesW(dir.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY) || (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
    wchar_t full[MAX_PATH];
    DWORD length = GetFullPathNameW(dir.c_str(), ARRAYSIZE(full), full, nullptr);
    if (!length || length >= ARRAYSIZE(full) || _wcsicmp(full, dir.c_str()) != 0) return false;
    GUID id; wchar_t suffix[40] = {};
    if (FAILED(CoCreateGuid(&id)) || !StringFromGUID2(id, suffix, ARRAYSIZE(suffix))) return false;
    std::wstring target = dir + L".removing-" + suffix;
    if (!MoveFileExW(dir.c_str(), target.c_str(), 0)) return false;   // no overwrite, same parent
    retired = std::move(target);
    return true;
}

static bool ScheduleCleanup(const std::wstring& retired, DWORD firstDelay = 3, DWORD lastDelay = 35) {
    // Only a directory retired by the function above is accepted. The fixed environment
    // reference avoids expanding percent signs in user-directory names as cmd variables.
    size_t tag = retired.rfind(L".removing-{");
    if (tag == std::wstring::npos || retired.size() != tag + 10 + 38 || retired.back() != L'}' ||
        retired.find_first_of(L"\"\r\n") != std::wstring::npos) return false;
    GUID id;
    if (FAILED(CLSIDFromString(retired.c_str() + tag + 10, &id))) return false;
    wchar_t sys[MAX_PATH];
    if (!GetSystemDirectoryW(sys, ARRAYSIZE(sys))) return false;
    std::wstring cmdExe = std::wstring(sys) + L"\\cmd.exe", ping = std::wstring(sys) + L"\\ping.exe";
    // The retirement suffix can take existing child paths past MAX_PATH.
    std::wstring cleanupPath = retired.rfind(L"\\\\", 0) == 0 ? L"\\\\?\\UNC\\" + retired.substr(2) : L"\\\\?\\" + retired;
    if (!SetEnvironmentVariableW(L"LIQUIDDOCK_RETIRED_DIRECTORY", cleanupPath.c_str())) return false;
    std::wstring command = L"\"" + cmdExe + L"\" /d /v:off /c \"\"" + ping + L"\" 127.0.0.1 -n " + std::to_wstring(firstDelay) +
        L" >nul & rmdir /s /q \"%LIQUIDDOCK_RETIRED_DIRECTORY%\" & \"" + ping + L"\" 127.0.0.1 -n " + std::to_wstring(lastDelay) +
        L" >nul & rmdir /s /q \"%LIQUIDDOCK_RETIRED_DIRECTORY%\"\"";
    STARTUPINFOW si = { sizeof(si) }; si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    BOOL ok = CreateProcessW(cmdExe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, sys, &si, &pi);
    SetEnvironmentVariableW(L"LIQUIDDOCK_RETIRED_DIRECTORY", nullptr);
    if (ok) { CloseHandle(pi.hProcess); CloseHandle(pi.hThread); }
    return ok != FALSE;
}

static int Dialog(const wchar_t* main, const wchar_t* content, TASKDIALOG_COMMON_BUTTON_FLAGS buttons,
                  PCWSTR icon = TD_INFORMATION_ICON, const TASKDIALOG_BUTTON* custom = nullptr, UINT ncustom = 0) {
    if (g_silent) return (buttons & TDCBF_YES_BUTTON) ? IDYES : IDOK;
    TASKDIALOGCONFIG c = { sizeof(c) };
    c.hInstance = GetModuleHandleW(nullptr);
    c.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
    c.pszWindowTitle = kTitle;
    c.pszMainIcon = icon;
    c.pszMainInstruction = main;
    c.pszContent = content;
    c.dwCommonButtons = buttons;
    c.pButtons = custom; c.cButtons = ncustom;
    int btn = 0;
    TaskDialogIndirect(&c, &btn, nullptr, nullptr);
    return btn;
}

static int RunHidden(const std::wstring& cmd, DWORD timeoutMs) {
    STARTUPINFOW si = { sizeof(si) }; si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    std::wstring c = cmd;
    if (!CreateProcessW(nullptr, c.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return -1;
    WaitForSingleObject(pi.hProcess, timeoutMs);
    DWORD code = 1; GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return (int)code;
}

// Ask a running dock to quit (it restores the native taskbar on the way out) and wait for it.
static void StopDock() {
    HWND h = FindWindowW(L"LiquidDockMain", nullptr);
    if (!h) return;
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
    HANDLE p = OpenProcess(SYNCHRONIZE, FALSE, pid);
    PostMessageW(h, WM_CLOSE, 0, 0);
    if (p) { WaitForSingleObject(p, 8000); CloseHandle(p); }
}

// Write an embedded file. A running exe can't be overwritten but can be renamed, so move it aside first.
static bool Extract(int id, const std::wstring& path) {
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!r) return false;
    const void* data = LockResource(LoadResource(nullptr, r));
    DWORD size = SizeofResource(nullptr, r);
    std::wstring old = path + L".old";
    DeleteFileW(old.c_str());
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES && !DeleteFileW(path.c_str()))
        MoveFileExW(path.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING);
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD bw = 0;
    BOOL ok = WriteFile(f, data, size, &bw, nullptr) && bw == size;
    CloseHandle(f);
    return ok;
}

static bool CreateShortcut(const std::wstring& target, const std::wstring& lnk,
                           const wchar_t* args = L"", const wchar_t* desc = L"Liquid Dock") {
    IShellLinkW* sl = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl)))) return false;
    sl->SetPath(target.c_str());
    sl->SetArguments(args);
    sl->SetWorkingDirectory(target.substr(0, target.find_last_of(L'\\')).c_str());
    sl->SetDescription(desc);
    IPersistFile* pf = nullptr;
    HRESULT hr = sl->QueryInterface(IID_PPV_ARGS(&pf));
    if (SUCCEEDED(hr)) { hr = pf->Save(lnk.c_str(), TRUE); pf->Release(); }
    sl->Release();
    return SUCCEEDED(hr);
}

static void SetStr(HKEY k, const wchar_t* name, const std::wstring& v) {
    RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)v.c_str(), DWORD((v.size() + 1) * sizeof(wchar_t)));
}
static void SetDword(HKEY k, const wchar_t* name, DWORD v) { RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE*)&v, sizeof(v)); }

// One click: running the setup installs right away (per user, no admin) and starts the dock,
// which shows itself for a moment so people see where it lives. Only failures show a dialog.
static int Install() {
    StopDock();
    std::wstring dir = InstallDir();
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    std::wstring dock = dir + L"\\LiquidDock.exe";
    bool ok = Extract(IDR_DOCK, dock)
           && Extract(IDR_SENSORS, dir + L"\\LiquidDockSensors.exe")
           && Extract(IDR_LICENSE, dir + L"\\LICENSE.txt")
           && Extract(IDR_NOTICES, dir + L"\\THIRD_PARTY_NOTICES.txt");
    std::wstring uninst = dir + L"\\uninstall.exe";
    if (ok && _wcsicmp(SelfPath().c_str(), uninst.c_str()) != 0) ok = CopyFileW(SelfPath().c_str(), uninst.c_str(), FALSE) != 0;
    if (!ok) {
        Dialog(L"설치하지 못했습니다", L"파일을 쓸 수 없습니다. 실행 중인 Liquid Dock을 종료한 뒤 다시 시도해 주세요.", TDCBF_OK_BUTTON, TD_ERROR_ICON);
        return 2;
    }

    // first install: autostart on (the dock writes the Run value itself from this setting)
    std::wstring data = dir + L"\\data";
    CreateDirectoryW(data.c_str(), nullptr);
    std::wstring ini = data + L"\\settings.ini";
    if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES) {
        HANDLE f = CreateFileW(ini.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr);
        if (f != INVALID_HANDLE_VALUE) { const char s[] = "autostart=1\n"; DWORD bw; WriteFile(f, s, sizeof(s) - 1, &bw, nullptr); CloseHandle(f); }
    }
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
        SetStr(k, L"LiquidDock", L"\"" + dock + L"\""); RegCloseKey(k);
    }
    CreateShortcut(dock, ShortcutPath());
    // escape hatch: quits the dock (or, if it is gone, restores the native taskbar)
    CreateShortcut(dock, RestoreShortcutPath(), L"--restore", L"Liquid Dock을 끄고 Windows 작업표시줄을 되돌립니다");
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kUninstKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
        SetStr(k, L"DisplayName", L"Liquid Dock");
        SetStr(k, L"DisplayVersion", LD_VERSION_WSTR);
        SetStr(k, L"Publisher", L"Liquid Dock");
        SetStr(k, L"DisplayIcon", dock + L",0");
        SetStr(k, L"InstallLocation", dir);
        SetStr(k, L"UninstallString", L"\"" + uninst + L"\" /uninstall");
        SetDword(k, L"NoModify", 1);
        SetDword(k, L"NoRepair", 1);
        SetDword(k, L"EstimatedSize", 2048);
        RegCloseKey(k);
    }

    STARTUPINFOW si = { sizeof(si) }; PROCESS_INFORMATION pi = {};
    std::wstring cmd = L"\"" + dock + L"\" --welcome";
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi)) {
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    }
    return 0;
}

static int Uninstall() {
    if (Dialog(L"Liquid Dock을 제거할까요?", L"독을 종료하고 Windows 기본 작업표시줄을 원래대로 되돌립니다.",
               TDCBF_YES_BUTTON | TDCBF_NO_BUTTON, TD_WARNING_ICON) != IDYES) return 1;
    std::wstring dir = InstallDir();
    StopDock();
    if (RunHidden(L"\"" + dir + L"\\LiquidDock.exe\" --restore", 12000) != 0) {
        Dialog(L"제거를 완료하지 못했습니다", L"작업표시줄 복원을 확인하지 못했습니다. 복원 정보와 설치 파일을 유지합니다. 잠시 후 다시 시도해 주세요.", TDCBF_OK_BUTTON, TD_WARNING_ICON);
        return 3;
    }

    // optional CPU-temperature helper task (created elevated, so removing it needs one UAC prompt)
    if (!g_silent && RunHidden(L"schtasks.exe /Query /TN LiquidDockSensors", 8000) == 0) {
        std::wstring helper = dir + L"\\LiquidDockSensors.exe";
        SHELLEXECUTEINFOW sei = { sizeof(sei) };
        sei.fMask = SEE_MASK_NOCLOSEPROCESS;
        sei.lpVerb = L"runas"; sei.lpFile = helper.c_str(); sei.lpParameters = L"--uninstall"; sei.nShow = SW_HIDE;
        if (ShellExecuteExW(&sei) && sei.hProcess) { WaitForSingleObject(sei.hProcess, 15000); CloseHandle(sei.hProcess); }
    }

    std::wstring retired;
    wchar_t cleanupWorkingDir[MAX_PATH];
    if (!GetSystemDirectoryW(cleanupWorkingDir, ARRAYSIZE(cleanupWorkingDir)) || !SetCurrentDirectoryW(cleanupWorkingDir)) return 4;
    if (!RetireInstallation(dir, retired)) {
        Dialog(L"제거를 완료하지 못했습니다", L"설치 폴더를 정리할 준비가 되지 않았습니다. 실행 중인 Liquid Dock을 종료한 뒤 다시 시도해 주세요.", TDCBF_OK_BUTTON, TD_WARNING_ICON);
        return 4;
    }

    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) { RegDeleteValueW(k, L"LiquidDock"); RegCloseKey(k); }
    RunHidden(L"schtasks.exe /Delete /F /TN LiquidDock", 8000);   // per-user logon task (no admin needed)
    RegDeleteTreeW(HKEY_CURRENT_USER, kUninstKey);
    // Do not erase a recovery record if Explorer disappeared after --restore returned.
    DeleteFileW(ShortcutPath().c_str());
    DeleteFileW(RestoreShortcutPath().c_str());

    if (!ScheduleCleanup(retired)) {
        Dialog(L"파일 정리가 남아 있습니다", L"자동 정리를 시작하지 못했습니다. 이전 설치 폴더는 별도 이름으로 보존돼 있으며 새 설치 파일에는 영향을 주지 않습니다.", TDCBF_OK_BUTTON, TD_WARNING_ICON);
        return 5;
    }
    Dialog(L"제거했습니다", L"Windows 기본 작업표시줄이 복원되었습니다.", TDCBF_OK_BUTTON);
    return 0;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR cmd, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);
    std::wstring args = cmd ? cmd : L"";
    g_silent = args.find(L"/silent") != std::wstring::npos;
    HANDLE operation = CreateMutexW(nullptr, FALSE, L"Local\\LiquidDock.Setup");
    DWORD wait = operation ? WaitForSingleObject(operation, 0) : WAIT_FAILED;
    if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) {
        if (operation) CloseHandle(operation);
        Dialog(L"설치 또는 제거가 진행 중입니다", L"진행 중인 작업을 마친 뒤 다시 시도해 주세요.", TDCBF_OK_BUTTON, TD_WARNING_ICON);
        CoUninitialize();
        return ERROR_INSTALL_ALREADY_RUNNING;
    }
    int rc = args.find(L"/uninstall") != std::wstring::npos ? Uninstall() : Install();
    ReleaseMutex(operation); CloseHandle(operation);
    CoUninitialize();
    return rc;
}
