#include "native_tray.h"
#include "system.h"
#include <shellapi.h>
#include <uiautomation.h>

// The native taskbar stays "shown" (just invisible, see NativeTaskbar), so Explorer's own tray
// windows open normally when their buttons are pressed.
namespace nativetray {

// Stock Windows 11 taskbar (XAML): its tray buttons are only reachable through UI Automation.
// The overflow window opens where Windows puts it (it can't be moved).
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
        // Nothing usable cached (explorer restarted, or the taskbar was already hidden when the dock
        // started): expand the taskbar for a moment - it stays invisible, the dock keeps its empty
        // region - so its content loads, then pick the buttons up again.
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
    HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!tray) return false;
    HWND notify = FindWindowExW(tray, nullptr, L"TrayNotifyWnd", nullptr);
    HWND chevron = notify ? GetDlgItem(notify, 1502) : nullptr;   // classic (Windows 10 style) taskbar
    if (!chevron) return OpenOverflowWin11(tray);

    DWORD_PTR r = 0;
    SendMessageTimeoutW(chevron, BM_CLICK, 0, 0, SMTO_ABORTIFHUNG, 500, &r);

    // classic overflow: move it next to the dock, centred on the dock's chevron
    HWND ov = nullptr;
    for (int i = 0; i < 25 && !(ov = FindWindowW(L"NotifyIconOverflowWindow", nullptr)); ++i) Sleep(10);
    for (int i = 0; i < 25 && ov && !IsWindowVisible(ov); ++i) Sleep(10);
    if (!ov) return true;
    RECT wr; GetWindowRect(ov, &wr);
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
    x = std::clamp<int>(x, mi.rcMonitor.left + 4, mi.rcMonitor.right - w - 4);
    y = std::clamp<int>(y, mi.rcMonitor.top + 4, mi.rcMonitor.bottom - h - 4);
    SetWindowPos(ov, HWND_TOPMOST, x, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
    return true;
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
