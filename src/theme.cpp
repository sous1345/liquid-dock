#include "theme.h"
#include <thread>

namespace theme {

static constexpr const wchar_t* kKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";
static double g_checkedAt = -1;
static bool g_light = false;

static bool ReadLight() {
    DWORD v = 0, sz = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, kKey, L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &sz) != ERROR_SUCCESS) return true;   // Windows default
    return v != 0;
}

bool IsLight() {
    double now = NowSeconds();
    if (g_checkedAt < 0 || now - g_checkedAt > 1.0) { g_light = ReadLight(); g_checkedAt = now; }   // drawn every frame
    return g_light;
}

bool Toggle() {
    DWORD v = ReadLight() ? 0 : 1;
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kKey, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return false;
    bool ok = RegSetValueExW(k, L"AppsUseLightTheme", 0, REG_DWORD, (BYTE*)&v, sizeof(v)) == ERROR_SUCCESS;
    ok = RegSetValueExW(k, L"SystemUsesLightTheme", 0, REG_DWORD, (BYTE*)&v, sizeof(v)) == ERROR_SUCCESS && ok;
    RegCloseKey(k);
    if (!ok) return false;
    g_light = v != 0; g_checkedAt = NowSeconds();
    // tell Explorer and the apps, as Settings does; a hung window must not stall the dock
    std::thread([] {
        DWORD_PTR r = 0;
        SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"ImmersiveColorSet", SMTO_ABORTIFHUNG, 1000, &r);
    }).detach();
    Log(L"theme: %s", v ? L"light" : L"dark");
    return true;
}

}   // namespace theme
