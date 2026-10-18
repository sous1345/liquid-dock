#include "native_tray.h"
#include "system.h"
#include <shellapi.h>
#include <shldisp.h>
#include <uiautomation.h>

// The native taskbar stays "shown" (just invisible, see NativeTaskbar), so Explorer's own tray
// windows open normally when their buttons are pressed.
namespace nativetray {

bool ToggleDesktop() {
    // Same Shell action as the native taskbar button. No synthetic key presses,
    // per-window minimization, or locally maintained restore list.
    ComPtr<IShellDispatch4> shell;
    HRESULT hr = CoCreateInstance(CLSID_Shell, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&shell));
    if (SUCCEEDED(hr)) hr = shell->ToggleDesktop();
    if (FAILED(hr)) Log(L"show desktop failed: %08x", hr);
    return SUCCEEDED(hr);
}

// Stock Windows 11 taskbar (XAML): its tray buttons are only reachable through UI Automation.
// The overflow host can be positioned after Explorer creates it.
//
// The XAML tray content only exists in the automation tree while the taskbar is expanded; once it
// auto-hides (as the dock makes it) it can't be found any more. Elements found earlier keep
// working though, so they are looked up before the taskbar vanishes (CacheTrayButtons) and reused.
enum Button { Chevron, Network, Clock, ButtonCount };
static const wchar_t* kButtonClass[ButtonCount] = { L"SystemTray.NormalButton",    // first one: hidden icons
                                                    L"SystemTray.AccentButton",    // network / quick settings
                                                    L"SystemTray.OmniButton" };    // clock / calendar
static ComPtr<IUIAutomation> g_uia;
static ComPtr<IUIAutomationElement> g_button[ButtonCount];
static HWND g_buttonTray = nullptr;

static ComPtr<IUIAutomationElement> FindButton(HWND tray, Button b) {
    ComPtr<IUIAutomationElement> root, btn;
    ComPtr<IUIAutomationCondition> cls;
    VARIANT v; v.vt = VT_BSTR; v.bstrVal = SysAllocString(kButtonClass[b]);
    g_uia->CreatePropertyCondition(UIA_ClassNamePropertyId, v, &cls);
    VariantClear(&v);
    if (cls && SUCCEEDED(g_uia->ElementFromHandle(tray, &root)) && root)
        root->FindFirst(TreeScope_Descendants, cls.Get(), &btn);
    return btn;
}

void CacheTrayButtons(HWND tray) {
    HWND notify = FindWindowExW(tray, nullptr, L"TrayNotifyWnd", nullptr);
    if (notify && GetDlgItem(notify, 1502)) return;   // classic taskbar, no XAML buttons
    if (g_buttonTray != tray) { for (auto& b : g_button) b.Reset(); g_buttonTray = tray; }
    if (!g_uia) {
        if (FAILED(CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_uia)))) return;
        // a busy explorer must not freeze the dock: give up quickly
        ComPtr<IUIAutomation2> u2;
        if (SUCCEEDED(g_uia.As(&u2))) { u2->put_ConnectionTimeout(500); u2->put_TransactionTimeout(500); }
    }
    for (int i = 0; i < ButtonCount; ++i)
        if (!g_button[i] && (g_button[i] = FindButton(tray, (Button)i))) Log(L"tray button %d cached", i);
}

static bool InvokeButton(Button b) {
    ComPtr<IUIAutomationInvokePattern> inv;
    if (g_button[b] && SUCCEEDED(g_button[b]->GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(&inv))) && inv && SUCCEEDED(inv->Invoke())) return true;
    g_button[b].Reset();   // stale (explorer restarted?)
    return false;
}

static bool OpenOverflowWin11(HWND tray) {
    bool cached = g_buttonTray == tray && g_button[Chevron], ok = cached && InvokeButton(Chevron);
    if (!ok) {
        // Materialize Explorer's automation content only under the opacity guard
        // acquired by OpenOverflow. An empty region alone is NOT sufficient:
        // Explorer rebuilds it while expanding its taskbar.
        APPBARDATA abd = { sizeof(abd) }; abd.hWnd = tray;
        UINT_PTR st = SHAppBarMessage(ABM_GETSTATE, &abd);
        bool autoHide = (st & ABS_AUTOHIDE) != 0;
        if (autoHide) { abd.lParam = st & ~ABS_AUTOHIDE; SHAppBarMessage(ABM_SETSTATE, &abd); }
        for (int i = 0; i < 40 && !g_button[Chevron]; ++i) { CacheTrayButtons(tray); if (!g_button[Chevron]) Sleep(25); }
        ok = InvokeButton(Chevron);
        if (autoHide) { abd.lParam = st; SHAppBarMessage(ABM_SETSTATE, &abd); }
    }
    Log(L"win11 overflow: cached=%d invoked=%d", (int)cached, (int)ok);
    return ok;
}

bool OpenOverflow(POINT anchor, Edge edge, RECT bar) {
    return g_taskbar.WithInvisibleTaskbars([&]() -> bool {
    HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!tray) return false;
    HWND notify = FindWindowExW(tray, nullptr, L"TrayNotifyWnd", nullptr);
    HWND chevron = notify ? GetDlgItem(notify, 1502) : nullptr;   // classic (Windows 10 style) taskbar
    const wchar_t* popupClass = chevron ? L"NotifyIconOverflowWindow" : L"TopLevelWindowForOverflowXamlIsland";
    HWND previous = FindWindowW(popupClass, nullptr);
    bool wasOpen = previous && IsWindowVisible(previous);
    if (!chevron) {
        if (!OpenOverflowWin11(tray)) return false;
    } else {
        DWORD_PTR r = 0;
        if (!SendMessageTimeoutW(chevron, BM_CLICK, 0, 0, SMTO_ABORTIFHUNG, 500, &r)) return false;
    }

    // A second click closes the existing flyout. Do not reopen or reposition it.
    if (wasOpen) return true;
    HWND ov = nullptr;
    for (int i = 0; i < 25 && !(ov = FindWindowW(popupClass, nullptr)); ++i) Sleep(10);
    for (int i = 0; i < 25 && ov && !IsWindowVisible(ov); ++i) Sleep(10);
    if (!ov || !IsWindowVisible(ov)) return false;
    RECT wr{}; if (!GetWindowRect(ov, &wr)) return false;
    int w = wr.right - wr.left, h = wr.bottom - wr.top, gap = 12;
    int x = anchor.x - w / 2, y = anchor.y - h / 2;
    switch (edge) {
    case Edge::Bottom: y = bar.top - h - gap; break;
    case Edge::Top:    y = bar.bottom + gap; break;
    case Edge::Left:   x = bar.right + gap; break;
    case Edge::Right:  x = bar.left - w - gap; break;
    }
    HMONITOR mon = MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) }; GetMonitorInfoW(mon, &mi);
    x = std::clamp<int>(x, mi.rcMonitor.left + 4, std::max(mi.rcMonitor.left + 4, mi.rcMonitor.right - w - 4));
    y = std::clamp<int>(y, mi.rcMonitor.top + 4, std::max(mi.rcMonitor.top + 4, mi.rcMonitor.bottom - h - 4));
    return SetWindowPos(ov, HWND_TOPMOST, x, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE) != FALSE;
    });
}

// With the stock taskbar the buttons are invoked directly; the shortcuts are the fallback.
void OpenCalendar() { if (!InvokeButton(Clock)) SendKeyCombo({ VK_LWIN, VK_MENU, 'D' }); }

void OpenVolume(POINT c) {
    // SndVol's flyout mode takes the screen position as MAKELPARAM(x, y) of its centre
    wchar_t arg[32]; swprintf_s(arg, L"-f %ld", (long)MAKELPARAM((WORD)c.x, (WORD)c.y));
    ShellExecuteW(nullptr, L"open", L"SndVol.exe", arg, nullptr, SW_SHOWNORMAL);
}

void OpenQuickSettings() { if (!InvokeButton(Network)) SendKeyCombo({ VK_LWIN, 'A' }); }

}
