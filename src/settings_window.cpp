#include "settings_window.h"
#include "dock.h"
#include "theme.h"
#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace shapewindow {
namespace {

enum Id { IdBottom = 100, IdLeft, IdRight, IdTop, IdIcon = 200, IdThick, IdGap, IdOffset, IdReset = 300, IdClose };
enum Apply { Live, Rebuild };   // Live: laid out again in place; Rebuild: the dock windows change size

struct Slider { Id id; const wchar_t* label; int lo, hi, step; int* value; Apply apply; HWND track = nullptr, text = nullptr; };

HWND g_wnd = nullptr;
HFONT g_font = nullptr, g_bold = nullptr;
HBRUSH g_bg = nullptr;
bool g_dark = false;
int g_iconSize = 44;   // the setting is a float; the slider works in whole steps
Slider g_sliders[] = {
    { IdIcon,   L"아이콘 크기",              32, 64, 2,  &g_iconSize,              Rebuild },
    { IdThick,  L"독 두께",                  75, 150, 5, &g_settings.dockThickness, Live },
    { IdGap,    L"화면 가장자리와 거리",      0, 40, 2,  &g_settings.edgeGap,       Live },
    { IdOffset, L"가장자리 따라 위치",       -100, 100, 5, &g_settings.edgeOffset, Live },
};
const struct { Id id; Edge edge; const wchar_t* label; } kEdges[] = {
    { IdBottom, Edge::Bottom, L"아래" }, { IdLeft, Edge::Left, L"왼쪽" }, { IdRight, Edge::Right, L"오른쪽" }, { IdTop, Edge::Top, L"위" } };

std::wstring ValueText(const Slider& s) {
    int v = *s.value;
    switch (s.id) {
    case IdIcon: return std::to_wstring(v) + (v == 44 ? L" (기본)" : L"");
    case IdThick: return std::to_wstring(v) + L"%" + (v == 100 ? L" (기본)" : L"");
    case IdGap: return v == 0 ? L"붙임" : std::to_wstring(v) + L" px";
    default: {
        if (v == 0) return L"가운데";
        bool vert = IsVertical(g_settings.edge);
        const wchar_t* dir = v < 0 ? (vert ? L"위로" : L"왼쪽으로") : (vert ? L"아래로" : L"오른쪽으로");
        return std::wstring(dir) + L" " + std::to_wstring(std::abs(v)) + L"%";
    }
    }
}

void Notify(Apply a) { PostMessageW(g_mainWnd, a == Rebuild ? WM_APP_REBUILD : WM_APP_RELAYOUT, 0, 0); }

void Refresh() {
    for (auto& s : g_sliders) {
        SendMessageW(s.track, TBM_SETPOS, TRUE, *s.value);
        SetWindowTextW(s.text, ValueText(s).c_str());
    }
    for (auto& e : kEdges) SendMessageW(GetDlgItem(g_wnd, e.id), BM_SETCHECK, g_settings.edge == e.edge ? BST_CHECKED : BST_UNCHECKED, 0);
}

HWND Make(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id, HFONT f) {
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, g_wnd, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE);
    if (g_dark) SetWindowTheme(c, L"DarkMode_Explorer", nullptr);
    return c;
}

void Build(HWND h) {
    UINT dpi = GetDpiForWindow(h);
    auto px = [&](int v) { return MulDiv(v, dpi, 96); };
    g_font = CreateFontW(-px(14), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_bold = CreateFontW(-px(17), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    int x = px(20), w = px(400), y = px(16);
    Make(L"STATIC", L"독 크기와 위치", 0, x, y, w, px(26), 0, g_bold); y += px(30);
    Make(L"STATIC", L"움직이면 독에 바로 반영됩니다.", 0, x, y, w, px(20), 0, g_font); y += px(32);

    Make(L"STATIC", L"위치", 0, x, y, w, px(20), 0, g_font); y += px(24);
    int bw = (w - px(18)) / 4;
    for (int i = 0; i < 4; ++i)
        Make(L"BUTTON", kEdges[i].label, BS_AUTORADIOBUTTON | BS_PUSHLIKE | (i == 0 ? WS_GROUP : 0), x + i * (bw + px(6)), y, bw, px(30), kEdges[i].id, g_font);
    y += px(46);

    for (auto& s : g_sliders) {
        Make(L"STATIC", s.label, 0, x, y, px(250), px(20), 0, g_font);
        s.text = Make(L"STATIC", L"", SS_RIGHT, x + px(250), y, w - px(250), px(20), 0, g_font);
        y += px(22);
        s.track = Make(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, x - px(4), y, w + px(8), px(30), s.id, g_font);
        SendMessageW(s.track, TBM_SETRANGEMIN, FALSE, s.lo);
        SendMessageW(s.track, TBM_SETRANGEMAX, FALSE, s.hi);
        SendMessageW(s.track, TBM_SETLINESIZE, 0, s.step);
        SendMessageW(s.track, TBM_SETPAGESIZE, 0, s.step * 4);
        y += px(40);
    }
    y += px(6);
    Make(L"BUTTON", L"기본값으로", WS_TABSTOP, x, y, px(120), px(32), IdReset, g_font);
    Make(L"BUTTON", L"닫기", WS_TABSTOP | BS_DEFPUSHBUTTON, x + w - px(100), y, px(100), px(32), IdClose, g_font);
    y += px(32) + px(20);

    RECT rc = { 0, 0, w + 2 * x, y };
    AdjustWindowRectExForDpi(&rc, GetWindowLongW(h, GWL_STYLE), FALSE, GetWindowLongW(h, GWL_EXSTYLE), dpi);
    SetWindowPos(h, nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    Refresh();
}

Slider* Find(HWND track) {
    for (auto& s : g_sliders) if (s.track == track) return &s;
    return nullptr;
}

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_HSCROLL: {
        Slider* s = Find((HWND)l);
        if (!s) break;
        int v = (int)SendMessageW(s->track, TBM_GETPOS, 0, 0);
        v = s->lo + (v - s->lo + s->step / 2) / s->step * s->step;   // snap to the step
        bool done = LOWORD(w) != TB_THUMBTRACK;
        if (v != *s->value) {
            *s->value = v;
            if (s->id == IdIcon) g_settings.iconSize = (float)v;
            SetWindowTextW(s->text, ValueText(*s).c_str());
            if (s->apply == Live) Notify(Live);   // live while dragging
        }
        if (done) {
            if (s->apply == Rebuild) Notify(Rebuild);   // a new window size: once, on release
            g_settings.Save();
        }
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(w);
        for (auto& e : kEdges) if (e.id == id && g_settings.edge != e.edge) {
            g_settings.edge = e.edge; g_settings.Save(); Notify(Rebuild); Refresh();
        }
        if (id == IdReset) {
            Settings d;
            g_settings.iconSize = d.iconSize; g_iconSize = (int)d.iconSize;
            g_settings.dockThickness = d.dockThickness; g_settings.edgeGap = d.edgeGap; g_settings.edgeOffset = d.edgeOffset;
            g_settings.Save(); Refresh(); Notify(Rebuild);
        }
        if (id == IdClose || id == IDCANCEL) DestroyWindow(h);
        return 0;
    }
    case WM_CTLCOLORSTATIC: case WM_CTLCOLORBTN: case WM_CTLCOLORDLG:
        if (g_dark) {
            SetTextColor((HDC)w, RGB(240, 240, 240));
            SetBkColor((HDC)w, RGB(32, 32, 32));
            return (LRESULT)g_bg;
        }
        break;
    case WM_ERASEBKGND: {
        RECT r; GetClientRect(h, &r);
        FillRect((HDC)w, &r, g_bg);
        return 1;
    }
    case WM_CLOSE: DestroyWindow(h); return 0;
    case WM_DESTROY:
        g_settings.Save();
        Dock::settingsOpen = false;
        g_wnd = nullptr;
        if (g_font) DeleteObject(g_font);
        if (g_bold) DeleteObject(g_bold);
        if (g_bg) DeleteObject(g_bg);
        g_font = g_bold = nullptr; g_bg = nullptr;
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

}   // namespace

bool IsOpen() { return g_wnd != nullptr; }

bool PreTranslate(MSG* msg) { return g_wnd && IsDialogMessageW(g_wnd, msg); }

void Show(HMONITOR monitor) {
    if (g_wnd) { SetForegroundWindow(g_wnd); return; }
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc = Proc; wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.lpszClassName = L"LiquidDockShape";
        wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));
        RegisterClassExW(&wc);
        registered = true;
    }
    g_dark = !theme::IsLight();
    g_bg = CreateSolidBrush(g_dark ? RGB(32, 32, 32) : GetSysColor(COLOR_WINDOW));
    g_iconSize = (int)std::round(g_settings.iconSize);
    MONITORINFO mi = { sizeof(mi) }; GetMonitorInfoW(monitor, &mi);
    // topmost: the docks stay shown (and topmost) while this is open
    g_wnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_DLGMODALFRAME, L"LiquidDockShape", L"Liquid Dock - 독 크기와 위치",
                            WS_CAPTION | WS_SYSMENU | WS_POPUP, mi.rcWork.left + 100, mi.rcWork.top + 100, 100, 100,
                            nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!g_wnd) return;
    BOOL dark = g_dark;
    DwmSetWindowAttribute(g_wnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    Build(g_wnd);
    // centred on the monitor whose dock opened it
    RECT r; GetWindowRect(g_wnd, &r);
    int w = r.right - r.left, hgt = r.bottom - r.top;
    SetWindowPos(g_wnd, nullptr, (mi.rcWork.left + mi.rcWork.right - w) / 2, (mi.rcWork.top + mi.rcWork.bottom - hgt) / 2, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    Dock::settingsOpen = true;
    ShowWindow(g_wnd, SW_SHOW);
    SetForegroundWindow(g_wnd);
}

}   // namespace shapewindow
