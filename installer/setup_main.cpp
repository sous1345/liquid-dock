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
    RunHidden(L"\"" + dir + L"\\LiquidDock.exe\" --restore", 8000);   // restores the taskbar if a crash left it hidden

    // optional CPU-temperature helper task (created elevated, so removing it needs one UAC prompt)
    if (!g_silent && RunHidden(L"schtasks.exe /Query /TN LiquidDockSensors", 8000) == 0) {
        std::wstring helper = dir + L"\\LiquidDockSensors.exe";
        SHELLEXECUTEINFOW sei = { sizeof(sei) };
        sei.fMask = SEE_MASK_NOCLOSEPROCESS;
        sei.lpVerb = L"runas"; sei.lpFile = helper.c_str(); sei.lpParameters = L"--uninstall"; sei.nShow = SW_HIDE;
        if (ShellExecuteExW(&sei) && sei.hProcess) { WaitForSingleObject(sei.hProcess, 15000); CloseHandle(sei.hProcess); }
    }

    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) { RegDeleteValueW(k, L"LiquidDock"); RegCloseKey(k); }
    RunHidden(L"schtasks.exe /Delete /F /TN LiquidDock", 8000);   // per-user logon task (no admin needed)
    RegDeleteTreeW(HKEY_CURRENT_USER, kUninstKey);
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\LiquidDock");
    DeleteFileW(ShortcutPath().c_str());
    DeleteFileW(RestoreShortcutPath().c_str());

    // the helper may still be running for up to 30 s after the dock quits; the folder is removed
    // by a detached cmd once this process (uninstall.exe) has exited
    std::wstring cmd = L"cmd.exe /c ping 127.0.0.1 -n 3 >nul & rmdir /s /q \"" + dir + L"\" & "
                       L"ping 127.0.0.1 -n 35 >nul & rmdir /s /q \"" + dir + L"\"";
    STARTUPINFOW si = { sizeof(si) }; si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    // CREATE_NO_WINDOW alone: cmd gets a hidden console that ping inherits (DETACHED_PROCESS would make
    // ping open its own visible console window)
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, L"C:\\", &si, &pi)) {
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
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
    int rc = args.find(L"/uninstall") != std::wstring::npos ? Uninstall() : Install();
    CoUninitialize();
    return rc;
}
