#include <winsock2.h>
#include <ws2ipdef.h>
#include "system.h"
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <netlistmgr.h>
#include <iphlpapi.h>
#include <shellapi.h>
#include <thread>
#include "native_tray.h"
#include "taskbar_visibility.h"

SysInfo g_sys;
NativeTaskbar g_taskbar;

static ComPtr<IAudioEndpointVolume> DefaultEndpointVolume() {
    ComPtr<IMMDeviceEnumerator> en;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) return nullptr;
    ComPtr<IMMDevice> dev;
    if (FAILED(en->GetDefaultAudioEndpoint(eRender, eMultimedia, &dev))) return nullptr;
    ComPtr<IAudioEndpointVolume> vol;
    if (FAILED(dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, (void**)vol.GetAddressOf()))) return nullptr;
    return vol;
}

static bool HasWifiUp() {
    ULONG len = 16 * 1024;
    std::vector<BYTE> buf(len);
    auto* aa = (IP_ADAPTER_ADDRESSES*)buf.data();
    ULONG r = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, aa, &len);
    if (r == ERROR_BUFFER_OVERFLOW) { buf.resize(len); aa = (IP_ADAPTER_ADDRESSES*)buf.data(); r = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_GATEWAYS, nullptr, aa, &len); }
    if (r != NO_ERROR) return false;
    bool wifi = false, wired = false;
    for (auto* a = aa; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || !a->FirstGatewayAddress) continue;
        if (a->IfType == IF_TYPE_IEEE80211) wifi = true; else if (a->IfType == IF_TYPE_ETHERNET_CSMACD) wired = true;
    }
    return wifi && !wired;
}

// clock text: cheap, on the UI thread
void SysInfo::Tick(double now) {
    if (now - lastFast < 0.5) return;
    lastFast = now;
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t t[64] = {}, d[64] = {}, ts[16];
    GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &st, nullptr, t, 64);
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &st, L"M/d ddd", d, 64, nullptr);
    swprintf_s(ts, L"%02d:%02d", st.wHour, st.wMinute);
    if (timeText != t || dateText != d) { timeText = t; dateText = d; timeShort = ts; version++; }
}

// volume and network: audio / network services can block for seconds (device changes, resume),
// so they are polled on a background thread and published as atomics
void SysInfo::Start() {
    std::thread([this] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        for (int tick = 0;; ++tick) {
            if (auto v = DefaultEndpointVolume()) {
                float lvl = 0; BOOL m = FALSE;
                v->GetMasterVolumeLevelScalar(&lvl); v->GetMute(&m);
                if (std::fabs(lvl - volume) > 0.001f || (m != 0) != muted) { volume = lvl; muted = m != 0; version++; }
            }
            if (tick % 6 == 0) {
                int n = 0;
                ComPtr<INetworkListManager> nlm;
                if (SUCCEEDED(CoCreateInstance(CLSID_NetworkListManager, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&nlm)))) {
                    NLM_CONNECTIVITY c = NLM_CONNECTIVITY_DISCONNECTED;
                    nlm->GetConnectivity(&c);
                    if (c & (NLM_CONNECTIVITY_IPV4_INTERNET | NLM_CONNECTIVITY_IPV6_INTERNET | NLM_CONNECTIVITY_IPV4_LOCALNETWORK | NLM_CONNECTIVITY_IPV6_LOCALNETWORK))
                        n = HasWifiUp() ? 2 : 1;
                } else n = 1;
                if (n != net) { net = n; version++; }
            }
            Sleep(500);
        }
    }).detach();
}

void SysInfo::AddVolume(float delta) {
    if (auto v = DefaultEndpointVolume()) {
        float lvl = 0; v->GetMasterVolumeLevelScalar(&lvl);
        lvl = clampf(lvl + delta, 0, 1);
        v->SetMasterVolumeLevelScalar(lvl, nullptr);
        if (muted && delta > 0) v->SetMute(FALSE, nullptr);
        volume = lvl; version++;
    }
}

void SysInfo::ToggleMute() {
    if (auto v = DefaultEndpointVolume()) {
        BOOL m = FALSE; v->GetMute(&m); v->SetMute(!m, nullptr); muted = !m; version++;
    }
}

void SendKeyCombo(std::initializer_list<WORD> keys) {
    std::vector<INPUT> in;
    for (WORD k : keys) { INPUT i = {}; i.type = INPUT_KEYBOARD; i.ki.wVk = k; in.push_back(i); }
    for (auto it = keys.end(); it != keys.begin();) { --it; INPUT i = {}; i.type = INPUT_KEYBOARD; i.ki.wVk = *it; i.ki.dwFlags = KEYEVENTF_KEYUP; in.push_back(i); }
    SendInput((UINT)in.size(), in.data(), sizeof(INPUT));
}

// ---------------------------------------------------------------- native taskbar

static void ForEachTaskbar(const std::function<void(HWND)>& fn) {
    if (HWND h = FindWindowW(L"Shell_TrayWnd", nullptr)) fn(h);
    HWND s = nullptr;
    while ((s = FindWindowExW(nullptr, s, L"Shell_SecondaryTrayWnd", nullptr)) != nullptr) fn(s);
}

// The taskbar is made invisible with an empty window region instead of being hidden: Explorer
// still treats it as shown, so its own windows (hidden icons, calendar, network...) keep working
// when the dock opens them, while it draws nothing and receives no clicks.
static bool HasEmptyRegion(HWND h) {
    HRGN r = CreateRectRgn(0, 0, 0, 0);
    int type = GetWindowRgn(h, r);
    DeleteObject(r);
    return type == NULLREGION;
}

static void Vanish(HWND h) {
    // Explorer can rebuild the region while activating a native flyout. Keep
    // the entire taskbar transparent as well, independent of that region.
    taskbarvisibility::Protect(h);
    if (!HasEmptyRegion(h) && IsWindowVisible(h) && h == FindWindowW(L"Shell_TrayWnd", nullptr)) nativetray::CacheTrayButtons(h);   // still expanded
    if (!IsWindowVisible(h)) ShowWindow(h, SW_SHOWNA);   // e.g. hidden by an older version
    if (!HasEmptyRegion(h)) {
        HRGN r = CreateRectRgn(0, 0, 0, 0);
        if (!SetWindowRgn(h, r, TRUE)) DeleteObject(r);   // on success the window owns the region
    }
}

extern HWND g_mainWnd;
constexpr UINT WM_APP_ENFORCE = WM_APP + 30;   // handled by the main window: Enforce() on the UI thread

void NativeTaskbar::StartEnforcer() {
    std::thread([this] {
        double start = NowSeconds();
        for (;;) {
            // right after logon explorer keeps (re)building its taskbar: check often for the first minute
            Sleep(NowSeconds() - start < 60 ? 250 : 1000);
            if (hidden) Enforce(false);
        }
    }).detach();
}

bool NativeTaskbar::Hide() {
    std::lock_guard<std::recursive_mutex> lk(mu);
    APPBARDATA abd = { sizeof(abd) };
    abd.hWnd = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!abd.hWnd) return false;
    if (!stateSaved) {   // do not hide anything until the recovery state has been saved
        originalState = SHAppBarMessage(ABM_GETSTATE, &abd);
        // remember across crashes so a later run can restore it
        HKEY k = nullptr;
        LSTATUS saved = RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\LiquidDock", 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr);
        if (saved == ERROR_SUCCESS) {
            DWORD v = (DWORD)originalState;
            DWORD existing = 0, sz = sizeof(existing);
            saved = RegGetValueW(HKEY_CURRENT_USER, L"Software\\LiquidDock", L"TaskbarState", RRF_RT_REG_DWORD, nullptr, &existing, &sz);
            if (saved == ERROR_FILE_NOT_FOUND || saved == ERROR_PATH_NOT_FOUND)
                saved = RegSetValueExW(k, L"TaskbarState", 0, REG_DWORD, (BYTE*)&v, sizeof(v));
            else if (saved == ERROR_SUCCESS) originalState = existing;
            RegCloseKey(k);
        }
        if (saved != ERROR_SUCCESS) { Log(L"taskbar backup failed: %ld; keeping native taskbar", saved); return false; }
        stateSaved = true;
    }
    hidden = true;
    Enforce();
    return true;
}

void NativeTaskbar::Enforce(bool onUiThread) {
    std::lock_guard<std::recursive_mutex> lk(mu);
    if (!hidden) return;
    HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!tray) return;   // explorer not up yet (early logon) - try again shortly
    // a new / re-shown taskbar must have its tray buttons cached (UI Automation, UI thread) before it vanishes
    if (!onUiThread && (!stateSaved || !HasEmptyRegion(tray))) { PostMessageW(g_mainWnd, WM_APP_ENFORCE, 0, 0); return; }
    if (!stateSaved) { Hide(); return; }
    // auto-hide frees the reserved work area so maximised windows use the full screen;
    // explorer may reset it while it starts, so keep checking
    ForEachTaskbar(Vanish);
    APPBARDATA abd = { sizeof(abd) };
    abd.hWnd = tray;
    UINT_PTR st = SHAppBarMessage(ABM_GETSTATE, &abd);
    if (!(st & ABS_AUTOHIDE)) {
        abd.lParam = ABS_AUTOHIDE | (originalState & ABS_ALWAYSONTOP);
        SHAppBarMessage(ABM_SETSTATE, &abd);
    }
    ForEachTaskbar(Vanish);
}

bool NativeTaskbar::WithInvisibleTaskbars(const std::function<bool()>& action) {
    std::lock_guard<std::recursive_mutex> lk(mu);
    if (!hidden || !stateSaved) return false;
    bool protectedAll = true; int count = 0;
    ForEachTaskbar([&](HWND h) { ++count; if (!taskbarvisibility::Protect(h)) protectedAll = false; });
    // Never activate Explorer's controls on the strength of an empty region alone.
    if (!count || !protectedAll) return false;
    bool result = action();
    Enforce();
    ForEachTaskbar([&](HWND h) { if (!taskbarvisibility::IsProtected(h)) result = false; });
    return result;
}

bool NativeTaskbar::Restore() {
    std::lock_guard<std::recursive_mutex> lk(mu);
    hidden = false;
    DWORD st = 0, sz = sizeof(st);
    LSTATUS read = RegGetValueW(HKEY_CURRENT_USER, L"Software\\LiquidDock", L"TaskbarState", RRF_RT_REG_DWORD, nullptr, &st, &sz);
    bool fromReg = read == ERROR_SUCCESS;
    if (!stateSaved && !fromReg && read != ERROR_FILE_NOT_FOUND && read != ERROR_PATH_NOT_FOUND) return false;
    if (!stateSaved && fromReg) { originalState = st; stateSaved = true; }
    HWND primary = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!primary) return false;   // Explorer is restarting: retain the recovery record
    std::vector<HWND> bars;
    ForEachTaskbar([&](HWND h) { bars.push_back(h); });
    bool restored = true;
    for (HWND h : bars) {
        if (!taskbarvisibility::Restore(h)) restored = false;
        if (!SetWindowRgn(h, nullptr, TRUE)) restored = false;
        ShowWindow(h, SW_SHOWNA);
        if (!IsWindow(h) || !IsWindowVisible(h) || HasEmptyRegion(h)) restored = false;
    }
    if (stateSaved) {
        APPBARDATA abd = { sizeof(abd) };
        abd.hWnd = primary;
        abd.lParam = originalState;
        SHAppBarMessage(ABM_SETSTATE, &abd);
        // ABM_SETSTATE always returns TRUE. Read back the setting instead.
        // ABS_ALWAYSONTOP is no longer meaningful on supported modern Windows versions.
        if ((SHAppBarMessage(ABM_GETSTATE, &abd) & ABS_AUTOHIDE) != (originalState & ABS_AUTOHIDE)) restored = false;
    }
    std::vector<HWND> current;
    ForEachTaskbar([&](HWND h) { current.push_back(h); });
    if (!restored || primary != FindWindowW(L"Shell_TrayWnd", nullptr) || current != bars) return false;
    LSTATUS removed = RegDeleteKeyValueW(HKEY_CURRENT_USER, L"Software\\LiquidDock", L"TaskbarState");
    if (removed != ERROR_SUCCESS && removed != ERROR_FILE_NOT_FOUND && removed != ERROR_PATH_NOT_FOUND) return false;
    stateSaved = false;
    return true;
}
