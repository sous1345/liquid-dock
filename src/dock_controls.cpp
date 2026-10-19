#include "dock.h"
#include "system.h"
#include "sensors.h"
#include "native_tray.h"
#include "oled.h"
#include "theme.h"
#include "audio_devices.h"
#include <shellapi.h>
#include "settings_window.h"

// ---------------------------------------------------------------- small tray buttons
// One row per button, in ButtonId order: its cell size and what it does. Layout, pairing, hit
// testing, hover highlight and tooltip placement are shared; the icon is drawn in buttons.cpp.

const Dock::SmallButton Dock::kSmallButtons[kButtonCount] = {
    // id                     icon×  min  tooltip               click                  right click
    { ButtonId::Network,      0.55f, 24,  &Dock::TipNetwork,    &Dock::ClickNetwork,    &Dock::MenuNetwork },
    { ButtonId::Volume,       0.55f, 24,  &Dock::TipVolume,     &Dock::ClickVolume,     &Dock::MenuVolume },
    { ButtonId::Desktop,      0.55f, 24,  &Dock::TipDesktop,    &Dock::ClickDesktop,    nullptr },
    { ButtonId::Theme,        0.55f, 24,  &Dock::TipTheme,      &Dock::ClickTheme,      nullptr },
    { ButtonId::Oled,         0.55f, 24,  &Dock::TipOled,       &Dock::ClickOled,       nullptr },
    { ButtonId::Awake,        0.55f, 24,  &Dock::TipAwake,      &Dock::ClickAwake,      nullptr },
    { ButtonId::Screenshot,   0.55f, 24,  &Dock::TipScreenshot, &Dock::ClickScreenshot, nullptr },
    { ButtonId::Quiet,        0.55f, 24,  &Dock::TipQuiet,      &Dock::ClickQuiet,      &Dock::MenuQuiet },
    { ButtonId::Trash,        0.55f, 24,  &Dock::TipTrash,      &Dock::ClickTrash,      &Dock::MenuTrash },
    { ButtonId::Downloads,    0.55f, 24,  &Dock::TipDownloads,  &Dock::ClickDownloads,  &Dock::MenuDownloads },
    { ButtonId::Audio,        0.55f, 24,  &Dock::TipAudio,      &Dock::ClickAudio,      &Dock::MenuAudio },
    // always available, with a generous cell independent of crowded app icons
    { ButtonId::Settings,     0.65f, 32,  &Dock::TipSettings,   &Dock::ClickSettings,   nullptr },
};

namespace {
void OpenUri(const wchar_t* uri) { ShellExecuteW(nullptr, L"open", uri, nullptr, nullptr, SW_SHOWNORMAL); }
void MenuDownloadsFolder() {
    std::wstring dir = features::DownloadsFolder();
    if (!dir.empty()) ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}
}
// by its path, not with a key combination
void OpenTaskManager() {
    wchar_t sys[MAX_PATH]; GetSystemDirectoryW(sys, MAX_PATH);
    ShellExecuteW(nullptr, L"open", (std::wstring(sys) + L"\\Taskmgr.exe").c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}
namespace {
constexpr double kForever = 1e300;
double SecondsToMidnight() {
    SYSTEMTIME st; GetLocalTime(&st);
    return 24.0 * 3600 - (st.wHour * 3600.0 + st.wMinute * 60.0 + st.wSecond);
}
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
std::wstring Dock::TipAwake() const { return features::Awake() ? L"화면 꺼짐 방지 끄기" : L"화면 꺼짐 방지 켜기"; }
std::wstring Dock::TipScreenshot() const { return L"스크린샷 (캡처 도구)"; }
std::wstring Dock::TipQuiet() const { return features::Quiet(NowSeconds()) ? L"알림 튐 다시 켜기" : L"알림 튐 끄기 (우클릭: 시간 선택)"; }
std::wstring Dock::TipTrash() const { return features::TrashFull() ? L"휴지통 (우클릭: 비우기)" : L"휴지통 (비어 있음)"; }
std::wstring Dock::TipDownloads() const { return L"최근 다운로드"; }
std::wstring Dock::TipAudio() const {
    for (auto& d : audio::Outputs()) if (d.isDefault) return L"출력: " + d.name;
    return L"오디오 장치";
}
std::wstring Dock::TipSettings() const { return L"설정"; }

// Windows' own tray windows rather than re-implementations
void Dock::ClickNetwork() { nativetray::OpenQuickSettings(); }
void Dock::ClickVolume() {
    // volume slider just outside the dock, centred on the icon
    float depth = IsVertical(g_settings.edge) ? 70 * scale : 260 * scale;
    nativetray::OpenVolume(ScreenPointUV(Cell_(ButtonId::Volume).Mid(), margin + barThick + 12 * scale + depth));
}
void Dock::ClickDesktop() { nativetray::ToggleDesktop(); }
void Dock::ClickTheme() {
    if (!theme::Toggle()) MessageBoxW(hwnd, L"다크/라이트 모드를 바꾸지 못했습니다.", L"Liquid Dock", MB_OK | MB_ICONINFORMATION);
    dirty = true;
}
void Dock::ClickOled() { ToggleOled(); }
void Dock::ClickAwake() { features::SetAwake(!features::Awake()); dirty = true; }
void Dock::ClickScreenshot() { OpenUri(L"ms-screenclip:"); }
void Dock::ClickQuiet() { features::SetQuiet(features::Quiet(NowSeconds()) ? 0 : kForever); dirty = true; }
void Dock::ClickTrash() { OpenUri(L"shell:RecycleBinFolder"); }
void Dock::ClickSettings() { settingswindow::Show(monitor); }

void Dock::ToggleOled() {
    bool ok = oled::Active() ? oled::Disable() : oled::Enable(g_settings.oledOneMonitor ? monitor : nullptr);
    if (!ok) MessageBoxW(hwnd, L"OLED 모드를 바꾸지 못했습니다. 잠시 후 다시 시도해 주세요.", L"Liquid Dock", MB_OK | MB_ICONINFORMATION);
    dirty = true;
}

// Files dropped on the trash button go to the Recycle Bin (Windows asks, as it does for Delete).
bool Dock::DropOnTrash(HDROP drop) {
    std::wstring list;
    UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    for (UINT i = 0; i < n; ++i) {
        wchar_t path[MAX_PATH * 2];
        if (DragQueryFileW(drop, i, path, ARRAYSIZE(path))) { list += path; list.push_back(L'\0'); }
    }
    if (list.empty()) return false;
    list.push_back(L'\0');
    SHFILEOPSTRUCTW op = {};
    op.hwnd = hwnd; op.wFunc = FO_DELETE; op.pFrom = list.c_str(); op.fFlags = FOF_ALLOWUNDO;
    int rc = SHFileOperationW(&op);
    Log(L"trash: %u file(s), rc %d", n, rc);
    return rc == 0;
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

// The output devices as menu items; choosing one makes it the default (or opens the sound settings
// when Windows refuses).
static void AddOutputDevices(MenuBuilder& mb, HMENU m) {
    auto devices = audio::Outputs();
    if (devices.empty()) { AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"출력 장치 없음"); return; }
    for (auto& d : devices) {
        std::wstring id = d.id;
        mb.Item(m, d.name.c_str(), [id] { if (!audio::SetDefault(id)) OpenUri(L"ms-settings:sound"); }, d.isDefault);
    }
}

// Right-click on the tray items: their own menus instead of the dock settings.
void Dock::ShowTrayMenu(const Hit& h) {
    MenuBuilder mb;
    HMENU m = mb.root;
    // same items as the Windows 11 taskbar's own menus (plus mute and the output list)
    if (h.Is(ButtonId::Volume)) {
        HMENU out = mb.Sub();
        AddOutputDevices(mb, out);
        mb.Popup(m, out, L"출력 장치");
        mb.Separator(m);
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
    } else if (h.Is(ButtonId::Network)) {
        mb.Item(m, L"문제 해결(&T)", [] { OpenUri(L"ms-settings:troubleshoot"); });
        mb.Item(m, L"네트워크 및 인터넷 설정 열기(&N)", [] { OpenUri(L"ms-settings:network"); });
    } else if (h.kind == HitClock) {
        mb.Item(m, L"날짜 및 시간 조정", [] { OpenUri(L"ms-settings:dateandtime"); });
        mb.Item(m, L"알림 설정", [] { OpenUri(L"ms-settings:notifications"); });
    } else {   // system monitor cell / panel
        mb.Item(m, L"작업 관리자", [] { OpenTaskManager(); });
        mb.Separator(m);
        mb.Item(m, L"CPU 온도 표시 (관리자 도우미 + PawnIO)", [this] {
            if (g_settings.cpuTemp) { g_settings.cpuTemp = false; g_settings.Save(); }
            else { SetForegroundWindow(hwnd); g_sensors.EnableCpuTemp(hwnd); }
        }, g_settings.cpuTemp);
    }
    mb.Run(PopupMenu(m));
}

void Dock::MenuQuiet() {
    MenuBuilder mb;
    double now = NowSeconds();
    bool on = features::Quiet(now);
    mb.Item(mb.root, L"1시간 동안", [now] { features::SetQuiet(now + 3600); });
    mb.Item(mb.root, L"오늘 밤 12시까지", [now] { features::SetQuiet(now + SecondsToMidnight()); });
    mb.Item(mb.root, L"다시 켤 때까지", [] { features::SetQuiet(kForever); });
    if (on) { mb.Separator(mb.root); mb.Item(mb.root, L"알림 튐 다시 켜기", [] { features::SetQuiet(0); }); }
    mb.Run(PopupMenu(mb.root));
    dirty = true;
}

void Dock::MenuTrash() {
    MenuBuilder mb;
    mb.Item(mb.root, L"휴지통 열기", [] { OpenUri(L"shell:RecycleBinFolder"); });
    if (features::TrashFull()) {
        HWND owner = hwnd;
        mb.Item(mb.root, L"휴지통 비우기", [owner] { SHEmptyRecycleBinW(owner, nullptr, 0); });   // Windows asks first
    } else AppendMenuW(mb.root, MF_STRING | MF_GRAYED, 0, L"휴지통 비우기");
    mb.Run(PopupMenu(mb.root));
}

void Dock::ClickDownloads() {
    MenuBuilder mb;
    auto files = features::RecentDownloads(15);
    if (files.empty()) AppendMenuW(mb.root, MF_STRING | MF_GRAYED, 0, L"최근 받은 파일이 없습니다");
    for (auto& f : files) {
        std::wstring name = f.name.size() > 48 ? f.name.substr(0, 45) + L"..." : f.name;
        std::wstring label = name + L"\t" + f.ago, path = f.path;
        for (size_t p = 0; (p = label.find(L'&', p)) != std::wstring::npos; p += 2) label.insert(p, 1, L'&');   // no accelerators
        mb.Item(mb.root, label.c_str(), [path] { ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL); });
    }
    mb.Separator(mb.root);
    mb.Item(mb.root, L"다운로드 폴더 열기", [] { MenuDownloadsFolder(); });
    mb.Run(PopupMenu(mb.root));
}

void Dock::MenuDownloads() { MenuDownloadsFolder(); }

void Dock::ClickAudio() {
    MenuBuilder mb;
    AddOutputDevices(mb, mb.root);
    mb.Separator(mb.root);
    mb.Item(mb.root, L"소리 설정 열기", [] { OpenUri(L"ms-settings:sound"); });
    mb.Run(PopupMenu(mb.root));
}

void Dock::MenuAudio() { OpenUri(L"ms-settings:sound"); }

// The quick menu: what people switch often. Everything else is in the settings window.
void Dock::ShowDockMenu() {
    MenuBuilder mb;
    HMENU m = mb.root;
    bool rebuild = false;
    HMONITOR mon = monitor;
    mb.Item(m, L"설정...", [mon] { settingswindow::Show(mon); });
    mb.Separator(m);

    HMENU pos = mb.Sub();
    const std::pair<Edge, const wchar_t*> edges[] = { { Edge::Bottom, L"아래" }, { Edge::Left, L"왼쪽" }, { Edge::Right, L"오른쪽" }, { Edge::Top, L"위" } };
    for (auto& [e, label] : edges) mb.Item(pos, label, [e, &rebuild] { g_settings.edge = e; rebuild = true; }, g_settings.edge == e);
    mb.Popup(m, pos, L"위치");

    HMENU size = mb.Sub();
    const std::pair<float, const wchar_t*> sizes[] = { { 36.f, L"작게" }, { 44.f, L"보통" }, { 54.f, L"크게" } };
    for (auto& [v, label] : sizes)
        mb.Item(size, label, [v, &rebuild] { g_settings.iconSize = v; rebuild = true; }, std::fabs(g_settings.iconSize - v) < 1);
    mb.Popup(m, size, L"아이콘 크기");

    mb.Item(m, L"자동 숨김", [] { g_settings.autoHide = !g_settings.autoHide; }, g_settings.autoHide);
    mb.Item(m, L"OLED 모드", [this] { ToggleOled(); }, oled::Active());
    mb.Separator(m);
    mb.Item(m, L"Liquid Dock 종료 (기본 작업표시줄 복원)", [] { PostMessageW(g_mainWnd, WM_CLOSE, 0, 0); });

    if (!mb.Run(PopupMenu(m))) return;
    g_settings.Save();
    if (rebuild) PostMessageW(g_mainWnd, WM_APP_REBUILD, 0, 0);
}
