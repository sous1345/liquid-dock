#include "dock.h"
#include "system.h"
#include "sensors.h"
#include "native_tray.h"
#include "oled.h"
#include "theme.h"
#include <shellapi.h>

// ---------------------------------------------------------------- small tray buttons
// A new button is one row here plus its tooltip / draw / click functions; layout, pairing,
// hit testing, hover highlight and the tooltip position all come from this table.

const Dock::SmallButton Dock::kSmallButtons[] = {
    // kind         group  shown                           icon×  min   tooltip              draw                 click
    { HitNet,       0,     nullptr,                        0.55f, 24,   &Dock::TipNetwork,   &Dock::DrawNetwork,  &Dock::ClickNetwork },
    { HitVol,       0,     nullptr,                        0.55f, 24,   &Dock::TipVolume,    &Dock::DrawVolume,   &Dock::ClickVolume },
    { HitDesktop,   1,     &Settings::showDesktopButton,   0.55f, 24,   &Dock::TipDesktop,   &Dock::DrawDesktop,  &Dock::ClickDesktop },
    { HitTheme,     1,     &Settings::showThemeButton,     0.55f, 24,   &Dock::TipTheme,     &Dock::DrawTheme,    &Dock::ClickTheme },
    { HitOled,      1,     &Settings::showOledButton,      0.55f, 24,   &Dock::TipOled,      &Dock::DrawOled,     &Dock::ClickOled },
    // always available, with a generous cell independent of crowded app icons
    { HitSettings,  1,     nullptr,                        0.65f, 32,   &Dock::TipSettings,  &Dock::DrawSettings, &Dock::ClickSettings },
    { HitNone },
};

const Dock::SmallButton* Dock::FindSmallButton(HitKind kind) {
    for (const SmallButton* b = kSmallButtons; b->kind != HitNone; ++b) if (b->kind == kind) return b;
    return nullptr;
}

std::wstring Dock::TipNetwork() const {
    return g_sys.net == 0 ? L"인터넷 연결 없음" : (g_sys.net == 2 ? L"Wi-Fi 연결됨" : L"이더넷 연결됨");
}
std::wstring Dock::TipVolume() const {
    wchar_t b[64]; swprintf_s(b, g_sys.muted ? L"음소거" : L"볼륨 %d%%", (int)std::round(g_sys.volume * 100));
    return b;
}
std::wstring Dock::TipDesktop() const { return L"바탕화면 보기 / 창 복원"; }
std::wstring Dock::TipTheme() const { return theme::IsLight() ? L"다크 모드로 전환" : L"라이트 모드로 전환"; }
std::wstring Dock::TipOled() const { return oled::Active() ? L"OLED 모드 끄기" : L"OLED 모드 켜기 (검은 배경 · 아이콘 숨김)"; }
std::wstring Dock::TipSettings() const { return L"설정"; }

// Windows' own tray windows rather than re-implementations
void Dock::ClickNetwork() { nativetray::OpenQuickSettings(); }
void Dock::ClickVolume() {
    // volume slider just outside the dock, centred on the icon
    float depth = IsVertical(g_settings.edge) ? 70 * scale : 260 * scale;
    nativetray::OpenVolume(ScreenPointUV(cells[HitVol].Mid(), margin + barThick + 12 * scale + depth));
}
void Dock::ClickDesktop() { nativetray::ToggleDesktop(); }
void Dock::ClickTheme() {
    if (!theme::Toggle()) MessageBoxW(hwnd, L"다크/라이트 모드를 바꾸지 못했습니다.", L"Liquid Dock", MB_OK | MB_ICONINFORMATION);
    dirty = true;
}
void Dock::ClickOled() { ToggleOled(); }

void Dock::ToggleOled() {
    bool ok = oled::Active() ? oled::Disable() : oled::Enable(g_settings.oledOneMonitor ? monitor : nullptr);
    if (!ok) MessageBoxW(hwnd, L"OLED 모드를 바꾸지 못했습니다. 잠시 후 다시 시도해 주세요.", L"Liquid Dock", MB_OK | MB_ICONINFORMATION);
    dirty = true;
}

// ---------------------------------------------------------------- menus

int Dock::PopupMenu(HMENU m) {
    POINT pt; GetCursorPos(&pt);
    menuOpen = true;
    SetForegroundWindow(hwnd);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, nullptr);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    menuOpen = false;
    DestroyMenu(m);
    return cmd;
}

// Builds a popup menu whose items carry their own actions, so no command numbers are needed.
namespace {
struct MenuBuilder {
    std::vector<std::function<void()>> actions;
    HMENU root = CreatePopupMenu();
    HMENU Sub() { return CreatePopupMenu(); }
    void Item(HMENU m, const wchar_t* label, std::function<void()> act, bool checked = false) {
        actions.push_back(std::move(act));
        AppendMenuW(m, MF_STRING | (checked ? MF_CHECKED : 0), actions.size(), label);
    }
    void Popup(HMENU m, HMENU sub, const wchar_t* label, bool checked = false) {
        AppendMenuW(m, MF_POPUP | (checked ? MF_CHECKED : 0), (UINT_PTR)sub, label);
    }
    void Separator(HMENU m) { AppendMenuW(m, MF_SEPARATOR, 0, nullptr); }
    // returns true when an item was chosen
    bool Run(int cmd) {
        if (cmd <= 0 || cmd > (int)actions.size()) return false;
        actions[cmd - 1]();
        return true;
    }
};
void OpenUri(const wchar_t* uri) { ShellExecuteW(nullptr, L"open", uri, nullptr, nullptr, SW_SHOWNORMAL); }
}

void Dock::ShowAppMenu(Slot& s) {
    auto app = s.app;
    if (!app) return;
    MenuBuilder mb;
    int n = 0;
    for (HWND w : app->windows) {
        if (n >= 15) break;
        wchar_t t[128] = {}; GetWindowTextW(w, t, 128);
        std::wstring title = t; if (title.empty()) title = app->name;
        if (title.size() > 60) title = title.substr(0, 57) + L"...";
        mb.Item(mb.root, title.c_str(), [w] { g_apps.ActivateWindow(w); }, w == GetForegroundWindow());
        n++;
    }
    if (n) mb.Separator(mb.root);
    int dock = index;
    mb.Item(mb.root, app->Running() ? L"새 창 열기" : L"열기", [app, dock] { g_apps.Launch(*app, dock); });
    if (app->pinned) mb.Item(mb.root, L"독에서 제거", [app] { g_apps.SetPinned(*app, false); });
    else mb.Item(mb.root, L"독에 고정", [app] { g_apps.SetPinned(*app, true); });
    if (app->Running()) {
        mb.Separator(mb.root);
        mb.Item(mb.root, n > 1 ? L"모든 창 닫기" : L"창 닫기", [app] { g_apps.CloseAll(*app); });
    }
    mb.Run(PopupMenu(mb.root));
}

// Right-click on the tray items: their own menus instead of the dock settings.
void Dock::ShowTrayMenu(HitKind kind) {
    MenuBuilder mb;
    HMENU m = mb.root;
    // same items as the Windows 11 taskbar's own menus (plus mute, which the dock can toggle directly)
    switch (kind) {
    case HitVol: {
        mb.Item(m, L"소리 설정 열기(&E)", [] { OpenUri(L"ms-settings:sound"); });
        mb.Item(m, L"볼륨 믹서 열기(&M)", [] { OpenUri(L"ms-settings:apps-volume"); });
        HMENU spk = mb.Sub();
        mb.Item(spk, L"공간 음향 설정 열기", [] { OpenUri(L"ms-settings:sound-devices"); });
        mb.Item(spk, L"출력 장치 속성", [] { OpenUri(L"ms-settings:sound"); });
        mb.Popup(m, spk, L"스피커 설정");
        mb.Item(m, L"소리(&S)", [] { ShellExecuteW(nullptr, L"open", L"control.exe", L"mmsys.cpl", nullptr, SW_SHOWNORMAL); });
        mb.Item(m, L"소리 문제 해결(&T)", [] { OpenUri(L"ms-settings:troubleshoot"); });
        mb.Separator(m);
        mb.Item(m, L"음소거", [] { g_sys.ToggleMute(); }, g_sys.muted);
        break;
    }
    case HitNet:
        mb.Item(m, L"문제 해결(&T)", [] { OpenUri(L"ms-settings:troubleshoot"); });
        mb.Item(m, L"네트워크 및 인터넷 설정 열기(&N)", [] { OpenUri(L"ms-settings:network"); });
        break;
    case HitClock:
        mb.Item(m, L"날짜 및 시간 조정", [] { OpenUri(L"ms-settings:dateandtime"); });
        mb.Item(m, L"알림 설정", [] { OpenUri(L"ms-settings:notifications"); });
        break;
    default:   // system monitor cell / panel
        mb.Item(m, L"작업 관리자", [] { SendKeyCombo({ VK_CONTROL, VK_SHIFT, VK_ESCAPE }); });
        mb.Separator(m);
        mb.Item(m, L"CPU 온도 표시 (관리자 도우미 + PawnIO)", [this] {
            if (g_settings.cpuTemp) { g_settings.cpuTemp = false; g_settings.Save(); }
            else { SetForegroundWindow(hwnd); g_sensors.EnableCpuTemp(hwnd); }
        }, g_settings.cpuTemp);
        break;
    }
    mb.Run(PopupMenu(m));
}

void Dock::ShowDockMenu() {
    MenuBuilder mb;
    HMENU m = mb.root;
    enum Effect { None, Rebuild, Reblur };
    Effect effect = None;
    // a choice that changes the layout rebuilds every dock; a glass change re-blurs them
    auto choice = [&](HMENU sub, const wchar_t* label, bool checked, std::function<void()> set, Effect e) {
        mb.Item(sub, label, [set, e, &effect] { set(); effect = e; }, checked);
    };
    auto& s = g_settings;

    HMENU pos = mb.Sub();
    const std::pair<Edge, const wchar_t*> edges[] = { { Edge::Bottom, L"아래" }, { Edge::Left, L"왼쪽" }, { Edge::Right, L"오른쪽" }, { Edge::Top, L"위" } };
    for (auto& [e, label] : edges) choice(pos, label, s.edge == e, [e] { g_settings.edge = e; }, Rebuild);

    HMENU size = mb.Sub();
    choice(size, L"작게", s.iconSize < 40, [] { g_settings.iconSize = 36; }, Rebuild);
    choice(size, L"보통", s.iconSize >= 40 && s.iconSize < 49, [] { g_settings.iconSize = 44; }, Rebuild);
    choice(size, L"크게", s.iconSize >= 49, [] { g_settings.iconSize = 54; }, Rebuild);

    HMENU spacing = mb.Sub();
    const wchar_t* spacingLabels[] = { L"좁게", L"보통", L"넓게" };
    for (int i = 0; i < 3; ++i) choice(spacing, spacingLabels[i], s.iconSpacing == i, [i] { g_settings.iconSpacing = i; }, Rebuild);

    HMENU clock = mb.Sub();
    choice(clock, L"보통", s.clockSize == 0, [] { g_settings.clockSize = 0; }, Rebuild);
    choice(clock, L"크게", s.clockSize == 1, [] { g_settings.clockSize = 1; }, Rebuild);

    HMENU om = mb.Sub();
    mb.Item(om, L"OLED 모드 켜기 (검은 배경 · 아이콘 숨김)", [this] { ToggleOled(); }, oled::Active());
    mb.Separator(om);
    choice(om, L"모든 모니터", !s.oledOneMonitor, [] { g_settings.oledOneMonitor = false; }, None);
    choice(om, L"이 모니터만", s.oledOneMonitor, [] { g_settings.oledOneMonitor = true; }, None);
    mb.Separator(om);
    choice(om, L"독에 OLED 버튼 표시", s.showOledButton, [] { g_settings.showOledButton = !g_settings.showOledButton; }, Rebuild);

    HMENU mg = mb.Sub();
    choice(mg, L"끄기", s.magnification < 1.05f, [] { g_settings.magnification = 1.0f; }, Rebuild);
    choice(mg, L"약하게", s.magnification >= 1.05f && s.magnification < 1.55f, [] { g_settings.magnification = 1.4f; }, Rebuild);
    choice(mg, L"보통", s.magnification >= 1.55f && s.magnification < 1.9f, [] { g_settings.magnification = 1.7f; }, Rebuild);
    choice(mg, L"강하게", s.magnification >= 1.9f, [] { g_settings.magnification = 2.1f; }, Rebuild);

    HMENU gl = mb.Sub();
    const wchar_t* glassLabels[] = { L"맑게", L"보통", L"뿌옇게" };
    for (int i = 0; i < 3; ++i) choice(gl, glassLabels[i], s.glassStyle == i, [i] { g_settings.glassStyle = i; }, Reblur);

    HMENU dl = mb.Sub();
    choice(dl, L"즉시", s.showDelay < 0.1f, [] { g_settings.showDelay = 0.0f; }, None);
    choice(dl, L"짧게", s.showDelay >= 0.1f && s.showDelay < 0.3f, [] { g_settings.showDelay = 0.15f; }, None);
    choice(dl, L"길게", s.showDelay >= 0.3f, [] { g_settings.showDelay = 0.45f; }, None);

    mb.Popup(m, pos, L"위치");
    mb.Popup(m, size, L"아이콘 크기");
    mb.Popup(m, spacing, L"아이콘 간격");
    mb.Popup(m, clock, L"시계 글자 크기");
    choice(m, L"바탕화면 보기 버튼", s.showDesktopButton, [] { g_settings.showDesktopButton = !g_settings.showDesktopButton; }, Rebuild);
    choice(m, L"다크/라이트 전환 버튼", s.showThemeButton, [] { g_settings.showThemeButton = !g_settings.showThemeButton; }, Rebuild);
    mb.Popup(m, om, L"OLED 모드", oled::Active());
    mb.Popup(m, mg, L"확대 효과");
    mb.Popup(m, gl, L"유리 질감");
    mb.Popup(m, dl, L"나타나는 속도");
    mb.Separator(m);
    mb.Item(m, L"Windows 시작 시 자동 실행", [] {
        g_settings.autostart = !g_settings.autostart; PostMessageW(g_mainWnd, WM_APP_AUTOSTART, 0, 0);
    }, s.autostart);
    mb.Item(m, L"CPU 온도 표시 (관리자 도우미 + PawnIO)", [this] {
        if (g_settings.cpuTemp) g_settings.cpuTemp = false;   // the helper's readings are ignored from now on
        else { SetForegroundWindow(hwnd); g_sensors.EnableCpuTemp(hwnd); }
    }, s.cpuTemp);
    mb.Separator(m);
    mb.Item(m, L"Liquid Dock 종료 (기본 작업표시줄 복원)", [] { PostMessageW(g_mainWnd, WM_CLOSE, 0, 0); });

    if (!mb.Run(PopupMenu(m))) return;
    g_settings.Save();
    if (effect == Reblur) { backdropDirty = true; dirty = true; PostMessageW(g_mainWnd, WM_APP_REBUILD, 1, 0); }   // other docks redo their blur too
    if (effect == Rebuild) PostMessageW(g_mainWnd, WM_APP_REBUILD, 0, 0);
}
