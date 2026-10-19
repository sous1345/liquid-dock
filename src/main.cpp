#include "common.h"
#include "gfx.h"
#include "dock.h"
#include "apps.h"
#include "system.h"
#include "capture.h"
#include "sensors.h"
#include "native_tray.h"
#include <dwmapi.h>
#include <shellapi.h>
#include <thread>
#include "oled.h"

Settings g_settings;
HWND g_mainWnd = nullptr;

static std::vector<std::unique_ptr<Dock>> g_docks;
static UINT g_msgShellHook = 0, g_msgTaskbarCreated = 0;
static bool g_running = true;
static double g_rebuildAt = 0;
static bool g_deviceResetRequested = false;
static double g_nextDeviceRetry = 0;
static bool g_docksReady = false;
static HANDLE g_watchdogProcess = nullptr;
static constexpr const wchar_t* kInstanceMutex = L"LiquidDock.SingleInstance";

static bool WatchdogReady() {
    return g_watchdogProcess && WaitForSingleObject(g_watchdogProcess, 0) == WAIT_TIMEOUT;
}

static void BuildDocks() {
    g_docksReady = false;
    g_docks.clear();
    std::vector<HMONITOR> mons;
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR m, HDC, LPRECT, LPARAM lp) -> BOOL {
        ((std::vector<HMONITOR>*)lp)->push_back(m); return TRUE;
    }, (LPARAM)&mons);
    HMONITOR primary = MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    int i = 0;
    for (HMONITOR m : mons) {
        auto d = std::make_unique<Dock>(m, i++, m == primary);
        if (d->Create()) g_docks.push_back(std::move(d));
        else Log(L"dock create failed for monitor %d", i - 1);
    }
    Log(L"docks: %d", (int)g_docks.size());
    // A replacement must exist on every monitor before hiding all native taskbars.
    g_docksReady = !mons.empty() && g_docks.size() == mons.size();
    if (!g_docksReady) g_docks.clear();
    if (g_docksReady && WatchdogReady()) { if (!g_taskbar.hidden) g_taskbar.Hide(); }
    else g_taskbar.Restore();
}

static void SetAutostart(bool on) {
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
        if (on) {
            std::wstring v = L"\"" + std::wstring(exe) + L"\"";
            RegSetValueExW(k, L"LiquidDock", 0, REG_SZ, (const BYTE*)v.c_str(), DWORD((v.size() + 1) * sizeof(wchar_t)));
        } else RegDeleteValueW(k, L"LiquidDock");
        RegCloseKey(k);
    }
    // The Run key starts ~10+ s after logon (Windows' startup delay), leaving the native taskbar on
    // screen meanwhile. A per-user logon task (no admin needed) starts right away; the Run key stays
    // as a fallback - a second instance just exits.
    std::wstring path = exe;
    std::thread([on, path] {
        auto run = [](std::wstring cmd) {
            STARTUPINFOW si = { sizeof(si) }; si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
            PROCESS_INFORMATION pi = {};
            if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return -1;
            WaitForSingleObject(pi.hProcess, 15000);
            DWORD code = 1; GetExitCodeProcess(pi.hProcess, &code);
            CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
            return (int)code;
        };
        if (!on) { run(SystemExe(L"schtasks.exe") + L" /Delete /F /TN LiquidDock"); return; }
        wchar_t user[256] = {}, dom[256] = {};
        GetEnvironmentVariableW(L"USERNAME", user, 256);
        GetEnvironmentVariableW(L"USERDOMAIN", dom, 256);
        auto esc = [](std::wstring s) {
            std::wstring o;
            for (wchar_t c : s) { if (c == L'&') o += L"&amp;"; else if (c == L'<') o += L"&lt;"; else if (c == L'>') o += L"&gt;"; else o += c; }
            return o;
        };
        std::wstring who = esc(std::wstring(dom) + L"\\" + user);
        std::wstring xml =
            L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>\r\n"
            L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\r\n"
            L"<RegistrationInfo><Description>Starts Liquid Dock at logon</Description></RegistrationInfo>\r\n"
            L"<Triggers><LogonTrigger><Enabled>true</Enabled><UserId>" + who + L"</UserId></LogonTrigger></Triggers>\r\n"
            L"<Principals><Principal id=\"Author\"><UserId>" + who + L"</UserId><LogonType>InteractiveToken</LogonType><RunLevel>LeastPrivilege</RunLevel></Principal></Principals>\r\n"
            L"<Settings><MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy><DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>"
            L"<StopIfGoingOnBatteries>false</StopIfGoingOnBatteries><ExecutionTimeLimit>PT0S</ExecutionTimeLimit><Priority>4</Priority><Enabled>true</Enabled></Settings>\r\n"
            L"<Actions Context=\"Author\"><Exec><Command>\"" + esc(path) + L"\"</Command></Exec></Actions>\r\n"
            L"</Task>\r\n";
        wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
        std::wstring file = std::wstring(tmp) + L"LiquidDockLogon.xml";
        HANDLE f = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE) return;
        DWORD bw; WORD bom = 0xFEFF;
        WriteFile(f, &bom, 2, &bw, nullptr);
        WriteFile(f, xml.data(), DWORD(xml.size() * 2), &bw, nullptr);
        CloseHandle(f);
        int rc = run(SystemExe(L"schtasks.exe") + L" /Create /F /TN LiquidDock /XML \"" + file + L"\"");
        DeleteFileW(file.c_str());
        Log(L"logon task rc=%d", rc);
    }).detach();
}

static LRESULT CALLBACK MainProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == g_msgShellHook && g_msgShellHook) { g_apps.OnShellHook(w, l); return 0; }
    if (m == g_msgTaskbarCreated && g_msgTaskbarCreated) {
        // A restored taskbar must stay available while the dock is unavailable or exiting.
        if (g_running && g_docksReady && WatchdogReady()) g_taskbar.Hide();
        else g_taskbar.Restore();
        oled::Reapply();   // a new Explorer shows the desktop icons again
        return 0;
    }
    switch (m) {
    case WM_DISPLAYCHANGE:
    case WM_DPICHANGED:
        g_rebuildAt = NowSeconds() + 0.8;
        return 0;
    case WM_SETTINGCHANGE:
        // (the enforcer thread re-applies auto-hide if this was explorer resetting it)
        return 0;
    case WM_APP_REBUILD:
        g_rebuildAt = NowSeconds();
        return 0;
    case WM_APP + 30:   // from the enforcer thread: a new taskbar needs its buttons cached here first
        g_taskbar.Enforce(true);
        return 0;
    case WM_APP_AUTOSTART:
        SetAutostart(g_settings.autostart);
        return 0;
    case WM_APP + 10: Dock::debugForceShow = true; return 0;
    case WM_APP + 11: Dock::debugForceShow = false; Dock::debugForcePanel = false; return 0;
    case WM_APP + 13: Dock::debugForceShow = true; Dock::debugForcePanel = true; return 0;
    case WM_APP + 14: g_sensors.alertPending = true; return 0;
    case WM_APP + 17:   // record the half of the primary monitor next to the dock
        for (auto& d : g_docks) if (d->primary) {
            MONITORINFO mi = { sizeof(mi) }; GetMonitorInfoW(d->monitor, &mi);
            int w = mi.rcMonitor.right - mi.rcMonitor.left, hgt = mi.rcMonitor.bottom - mi.rcMonitor.top;
            RECT crop = { 0, 0, w, hgt };
            switch (g_settings.edge) {
            case Edge::Left:   crop = { 0, 0, w / 2, hgt }; break;
            case Edge::Right:  crop = { w / 2, 0, w, hgt }; break;
            case Edge::Bottom: crop = { 0, hgt / 2, w, hgt }; break;
            case Edge::Top:    crop = { 0, 0, w, hgt / 2 }; break;
            }
            d->StartRecording(crop, 0.5f);
        }
        return 0;
    case WM_APP + 18: for (auto& d : g_docks) d->StopRecording(); return 0;
    case WM_APP + 19: {   // simulate a KakaoTalk notification
        std::shared_ptr<AppItem> target;
        for (auto& a : g_apps.items) if (a->exe.find(L"kakaotalk") != std::wstring::npos) { target = a; break; }
        if (!target) for (auto& a : g_apps.items) if (a->Running()) { target = a; break; }
        if (target) { target->attention = true; target->lastAttentionBounce = NowSeconds(); g_apps.attentionEvents.push_back({ target->id }); }
        return 0;
    }
    case WM_APP + 21: {   // developer: open the native hidden-icons window as the dock's chevron does
        POINT p; GetCursorPos(&p);
        RECT r = { p.x, p.y, p.x + 1, p.y + 1 };
        Log(L"debug overflow: %d", (int)nativetray::OpenOverflow(p, g_settings.edge, r));
        return 0;
    }
    case WM_APP + 22: nativetray::OpenCalendar(); return 0;
    case WM_APP + 23: { POINT p; GetCursorPos(&p); nativetray::OpenVolume(p); return 0; }
    case WM_APP + 24: nativetray::OpenQuickSettings(); return 0;
    case WM_APP + 25: g_deviceResetRequested = true; return 0;   // developer: simulate a GPU reset
    case WM_APP + 15: case WM_APP + 16:
        g_settings.autostart = (m == WM_APP + 15);
        g_settings.Save();
        SetAutostart(g_settings.autostart);
        Log(L"autostart %d", (int)g_settings.autostart);
        return 0;
    case WM_APP + 12: for (auto& d : g_docks) d->RequestSnapshot(); return 0;
    case WM_HOTKEY:
        g_running = false;
        return 0;
    case WM_QUERYENDSESSION:
        return TRUE;
    case WM_ENDSESSION:
        if (w) { g_running = false; g_taskbar.Restore(); }
        return 0;
    case WM_CLOSE:
        g_running = false;
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// Popup menus follow the Windows app theme (dark/light) like Explorer's. Undocumented but stable
// uxtheme exports (ordinal 135 SetPreferredAppMode, 136 FlushMenuThemes) since Windows 10 1903.
static void EnableThemedMenus() {
    HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!ux) return;
    using SetMode = int(WINAPI*)(int);
    using Flush = void(WINAPI*)();
    auto setMode = (SetMode)GetProcAddress(ux, MAKEINTRESOURCEA(135));
    auto flush = (Flush)GetProcAddress(ux, MAKEINTRESOURCEA(136));
    if (setMode) setMode(1);   // AllowDark: follow the system setting
    if (flush) flush();
}

static LONG WINAPI CrashHandler(EXCEPTION_POINTERS*) {
    g_taskbar.Restore();
    return EXCEPTION_CONTINUE_SEARCH;
}

// A second copy of the exe that waits for the dock to exit. If the dock was killed (Task Manager,
// crash the handler missed), the saved taskbar state is still there: restore the taskbar.
// Serialize recovery with the next dock instance, but release the mutex between retries
// so an Explorer restart cannot prevent the user starting a fresh dock.
static int RestoreTaskbarWhenIdle(DWORD timeoutMs) {
    ULONGLONG start = GetTickCount64();
    do {
        HANDLE m = CreateMutexW(nullptr, FALSE, kInstanceMutex);
        if (!m) return (int)GetLastError();
        DWORD wait = WaitForSingleObject(m, 0);
        if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) { CloseHandle(m); return ERROR_BUSY; }
        bool ok = g_taskbar.Restore();
        ReleaseMutex(m); CloseHandle(m);
        if (ok) return 0;
        if (GetTickCount64() - start >= timeoutMs) break;
        Sleep(250);
    } while (true);
    // The registry backup remains available to the restore shortcut or a later run.
    return ERROR_RETRY;
}

static int RunWatchdog(DWORD pid, const wchar_t* readyName) {
    HANDLE p = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!p && GetLastError() != ERROR_INVALID_PARAMETER) return 1;
    if (readyName) {
        HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, readyName);
        if (!p || !ready) { if (p) CloseHandle(p); if (ready) CloseHandle(ready); return 1; }
        BOOL signaled = SetEvent(ready); CloseHandle(ready);
        if (!signaled) { CloseHandle(p); return 1; }
    }
    if (p) {
        DWORD wait = WaitForSingleObject(p, INFINITE);
        CloseHandle(p);
        if (wait != WAIT_OBJECT_0) return 1;
    }
    DWORD st = 0, sz = sizeof(st);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\LiquidDock", L"TaskbarState", RRF_RT_REG_DWORD, nullptr, &st, &sz) != ERROR_SUCCESS)
        return 0;   // clean exit: already restored
    int rc = RestoreTaskbarWhenIdle(30000);
    return rc == ERROR_BUSY ? 0 : rc;   // a new dock owns recovery now
}

static bool StartWatchdog() {
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    GUID id; wchar_t guid[40] = {};
    if (FAILED(CoCreateGuid(&id)) || !StringFromGUID2(id, guid, ARRAYSIZE(guid))) return false;
    std::wstring name = L"Local\\LiquidDock.WatchdogReady." + std::wstring(guid);
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, name.c_str());
    if (!ready) return false;
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --watchdog " + std::to_wstring(GetCurrentProcessId()) + L" --ready " + name;
    STARTUPINFOW si = { sizeof(si) }; PROCESS_INFORMATION pi = {};
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        HANDLE waits[] = { ready, pi.hProcess };
        if (WaitForMultipleObjects(2, waits, FALSE, 5000) == WAIT_OBJECT_0) g_watchdogProcess = pi.hProcess;
        else CloseHandle(pi.hProcess);
    }
    CloseHandle(ready);
    return WatchdogReady();
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR cmd, int) {
    if (cmd && wcsstr(cmd, L"--watchdog ")) {
        const wchar_t* ready = wcsstr(cmd, L" --ready ");
        return RunWatchdog((DWORD)_wtoi(wcsstr(cmd, L"--watchdog ") + 11), ready ? ready + 9 : nullptr);
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    winrt::init_apartment(winrt::apartment_type::single_threaded);

    std::wstring args = cmd ? cmd : L"";
#ifdef LD_DEV
    // developer controls for a running instance (build.bat dev)
    struct { const wchar_t* flag; UINT msg; } dbg[] = { { L"--debug-show", WM_APP + 10 }, { L"--debug-hide", WM_APP + 11 }, { L"--snap", WM_APP + 12 },
                                                        { L"--debug-panel", WM_APP + 13 }, { L"--debug-alert", WM_APP + 14 },
                                                        { L"--autostart-on", WM_APP + 15 }, { L"--autostart-off", WM_APP + 16 },
                                                        { L"--rec-start", WM_APP + 17 }, { L"--rec-stop", WM_APP + 18 }, { L"--debug-bounce", WM_APP + 19 },
                                                        { L"--debug-overflow", WM_APP + 21 }, { L"--debug-calendar", WM_APP + 22 },
                                                        { L"--debug-volume", WM_APP + 23 }, { L"--debug-network", WM_APP + 24 },
                                                        { L"--debug-device-reset", WM_APP + 25 } };
    for (auto& d : dbg) {
        if (args.find(d.flag) != std::wstring::npos) {
            if (HWND other = FindWindowW(L"LiquidDockMain", nullptr)) PostMessageW(other, d.msg, 0, 0);
            return 0;
        }
    }
#endif
    if (args.find(L"--restore") != std::wstring::npos || args.find(L"--quit") != std::wstring::npos) {
        if (HWND other = FindWindowW(L"LiquidDockMain", nullptr)) {
            // ask nicely; a hung dock is ended and the taskbar restored from here
            DWORD pid = 0; GetWindowThreadProcessId(other, &pid);
            HANDLE p = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, pid);
            PostMessageW(other, WM_CLOSE, 0, 0);
            if (p && WaitForSingleObject(p, 3000) == WAIT_TIMEOUT) {
                TerminateProcess(p, 1);
                WaitForSingleObject(p, 2000);
            }
            if (p) CloseHandle(p);
        }
        // uninstall and the restore shortcut put the wallpaper and desktop icons back too
        if (args.find(L"--restore") != std::wstring::npos) oled::Disable();
        return RestoreTaskbarWhenIdle(5000);
    }

    HANDLE mutex = CreateMutexW(nullptr, FALSE, kInstanceMutex);
    if (!mutex) return 1;
    DWORD ownership = WaitForSingleObject(mutex, 3000);
    if (ownership != WAIT_OBJECT_0 && ownership != WAIT_ABANDONED) { CloseHandle(mutex); return 0; }

    SetUnhandledExceptionFilter(CrashHandler);
    EnableThemedMenus();
    g_settings.Load();
    if (!g_gfx.Init()) {
        g_taskbar.Restore();
        MessageBoxW(nullptr, L"GPU 초기화에 실패했습니다.", L"Liquid Dock", MB_ICONERROR);
        return 1;
    }
    MonitorCapture::RequestBorderlessAccess();

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = MainProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"LiquidDockMain";
    RegisterClassExW(&wc);
    g_mainWnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"Liquid Dock", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, hInst, nullptr);
    if (!g_mainWnd) { g_taskbar.Restore(); return 1; }
    g_msgShellHook = RegisterWindowMessageW(L"SHELLHOOK");
    g_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    ChangeWindowMessageFilterEx(g_mainWnd, g_msgTaskbarCreated, MSGFLT_ALLOW, nullptr);
    RegisterShellHookWindow(g_mainWnd);
    // emergency exit: Ctrl+Alt+Shift+F12
    RegisterHotKey(g_mainWnd, 1, MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, VK_F12);

    g_apps.Init();
    g_sys.Tick(NowSeconds());
    g_sys.Start();
    g_sensors.Start();
    g_taskbar.StartEnforcer();
    if (!StartWatchdog()) Log(L"watchdog not ready; keeping native taskbar");
    BuildDocks();
    oled::Reapply();
    if (g_settings.autostart) SetAutostart(true);
    // right after installing: show the dock for a few seconds so people see where it is
    double welcomeUntil = args.find(L"--welcome") != std::wstring::npos ? NowSeconds() + 3.5 : 0;
    if (welcomeUntil) Dock::debugForceShow = true;

    double last = NowSeconds(), nextRestoreTry = 0;
    while (g_running) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { g_running = false; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!g_running) break;
        double now = NowSeconds();
        float dt = (float)std::clamp(now - last, 0.0, 0.05);
        last = now;

        if (g_rebuildAt > 0 && now >= g_rebuildAt) { g_rebuildAt = 0; BuildDocks(); }
        if (welcomeUntil && now >= welcomeUntil) { welcomeUntil = 0; Dock::debugForceShow = false; }
        // GPU reset / driver update: the device is gone; rebuild everything on a new one
        if ((g_deviceResetRequested || g_gfx.DeviceLost()) && now >= g_nextDeviceRetry) {
            g_deviceResetRequested = false;
            Log(L"GPU device lost - recreating");
            g_docksReady = false;
            g_docks.clear();
            if (g_gfx.Reset()) { g_apps.ReloadIcons(); BuildDocks(); }
            else {
                g_nextDeviceRetry = now + 2.0; g_deviceResetRequested = true;
                Log(L"device re-create failed, retrying");
                if (g_taskbar.hidden) g_taskbar.Restore();   // usable taskbar until the dock is back
            }
        }

        if ((!g_docksReady || !WatchdogReady()) && now >= nextRestoreTry) {
            g_taskbar.Restore();
            nextRestoreTry = now + 1.0;
        }

        g_apps.Tick(now);
        g_sys.Tick(now);
        g_sensors.Tick(now);

        POINT c; GetCursorPos(&c);
        bool busy = false;
        for (auto& d : g_docks) {
            d->Update(now, dt, c);
            d->Render();
            d->RecordTick(now);
            busy |= d->Busy();
        }
        g_apps.attentionEvents.clear();
        g_sensors.alertPending = false;

        if (busy) DwmFlush();
        else {
            // idle: poll the cursor quickly only near a screen edge (where the dock can appear)
            MONITORINFO mi = { sizeof(mi) };
            GetMonitorInfoW(MonitorFromPoint(c, MONITOR_DEFAULTTONEAREST), &mi);
            const RECT& r = mi.rcMonitor;
            bool nearEdge = c.x - r.left < 120 || r.right - c.x < 120 || c.y - r.top < 120 || r.bottom - c.y < 120;
            MsgWaitForMultipleObjectsEx(0, nullptr, nearEdge ? 15 : 45, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        }
    }

    g_docksReady = false;
    g_docks.clear();
    g_taskbar.Restore();
    DeregisterShellHookWindow(g_mainWnd);
    if (mutex) { ReleaseMutex(mutex); CloseHandle(mutex); }
    if (g_watchdogProcess) CloseHandle(g_watchdogProcess);
    return 0;
}
