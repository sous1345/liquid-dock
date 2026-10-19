#include "common.h"
#include <shlobj.h>
#include <cstdio>
#include <cstdarg>
#include <fstream>
#include <sstream>
#include <mutex>
#include <variant>

double NowSeconds() {
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return double(t.QuadPart) / double(freq.QuadPart);
}

// Settings, pins and logs live in a "data" folder next to the exe (portable; also avoids
// AppData virtualisation when launched from a packaged app).
std::wstring AppDataDir() {
    static std::wstring dir;
    if (dir.empty()) {
        wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring s = exe;
        dir = s.substr(0, s.find_last_of(L'\\')) + L"\\data";
        CreateDirectoryW(dir.c_str(), nullptr);
    }
    return dir;
}

void Log(const wchar_t* fmt, ...) {
    wchar_t buf[2048];
    va_list ap; va_start(ap, fmt);
    _vsnwprintf_s(buf, _TRUNCATE, fmt, ap);
    va_end(ap);
    OutputDebugStringW(buf);
    OutputDebugStringW(L"\n");
    static std::mutex mu;   // Log is called from worker threads too
    std::lock_guard<std::mutex> lk(mu);
    static int lines = 0;   // keep the log small however long the dock runs
    if (++lines > 20000) return;
    static FILE* f = nullptr;
    if (!f) {
        std::wstring path = AppDataDir() + L"\\dock.log";
        f = _wfsopen(path.c_str(), L"w, ccs=UTF-8", _SH_DENYNO);
    }
    if (f) {
        SYSTEMTIME st; GetLocalTime(&st);
        fwprintf(f, L"%02d:%02d:%02d.%03d %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, buf);
        fflush(f);
    }
}

std::wstring SystemExe(const wchar_t* name) {
    wchar_t dir[MAX_PATH]; GetSystemDirectoryW(dir, MAX_PATH);
    return L"\"" + std::wstring(dir) + L"\\" + name + L"\"";
}

std::wstring ToLower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(s.data(), (DWORD)s.size());
    return s;
}

std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring FromUtf8(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

// Every setting is one row: key in settings.ini, member, and the accepted range. Unknown keys and
// malformed values are ignored, so older and newer files load with defaults for what is missing.
namespace {
struct Field {
    const char* key;
    std::variant<float Settings::*, int Settings::*, bool Settings::*, Edge Settings::*, std::string Settings::*> member;
    float lo = 0, hi = 0;   // numeric range (ignored for flags and text)
    bool legacy = false;    // read from older files, no longer written
};
const Field kFields[] = {
    { "edge", &Settings::edge, 0, 3 },
    { "iconSize", &Settings::iconSize, 32, 96 },
    { "magnification", &Settings::magnification, 1, 2.1f },   // above the menu's maximum, crowded icons slide away from the pointer
    { "glassStyle", &Settings::glassStyle, 0, 2 },
    { "trayButtons", &Settings::trayButtons },
    { "showDesktopButton", &Settings::showDesktopButton, 0, 0, true },
    { "showThemeButton", &Settings::showThemeButton, 0, 0, true },
    { "showOledButton", &Settings::showOledButton, 0, 0, true },
    { "oledOneMonitor", &Settings::oledOneMonitor },
    { "clockSize", &Settings::clockSize, 0, 1 },
    { "iconSpacing", &Settings::iconSpacing, 0, 2 },
    { "iconGap", &Settings::iconGap, 0, 60 },
    { "dockThickness", &Settings::dockThickness, 75, 150 },
    { "edgeGap", &Settings::edgeGap, 0, 40 },
    { "edgeOffset", &Settings::edgeOffset, -100, 100 },
    { "autoHide", &Settings::autoHide },
    { "showWithStart", &Settings::showWithStart },
    { "showDelay", &Settings::showDelay, 0, 2 },
    { "hideDelay", &Settings::hideDelay, 0, 3 },
    { "autostart", &Settings::autostart },
    { "startMenuExtras", &Settings::startMenuExtras },
    { "shiftRunsAsAdmin", &Settings::shiftRunsAsAdmin },
    { "revealHotkey", &Settings::revealHotkey, 0, 3 },
    { "cpuTemp", &Settings::cpuTemp },
    { "pinsVersion", &Settings::pinsVersion, 0, 1000000 },
};
}

void Settings::Load() {
    std::ifstream f(AppDataDir() + L"\\settings.ini");
    std::string line;
    bool sawButtons = false;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        for (auto& fd : kFields) {
            if (k != fd.key) continue;
            try {
                std::visit([&](auto m) {
                    using T = std::remove_cvref_t<decltype(this->*m)>;
                    if constexpr (std::is_same_v<T, std::string>) { this->*m = v; sawButtons = true; }
                    else if constexpr (std::is_same_v<T, bool>) this->*m = std::stoi(v) != 0;
                    else if constexpr (std::is_same_v<T, float>) this->*m = std::clamp(std::stof(v), fd.lo, fd.hi);
                    else if constexpr (std::is_same_v<T, int>) this->*m = std::clamp(std::stoi(v), (int)fd.lo, (int)fd.hi);
                    else this->*m = (T)std::clamp(std::stoi(v), (int)fd.lo, (int)fd.hi);
                }, fd.member);
            } catch (...) {}
            break;
        }
    }
    if (!sawButtons) {   // a file from before the button list: carry its three on/off choices over
        trayButtons.clear();
        for (auto [on, key] : { std::pair{ showDesktopButton, "desktop" }, std::pair{ showThemeButton, "theme" }, std::pair{ showOledButton, "oled" } })
            if (on) { if (!trayButtons.empty()) trayButtons += ','; trayButtons += key; }
    }
}

// Written to a temporary file and swapped in, so a crash or power loss never leaves a half file.
void Settings::Save() const {
    std::wstring path = AppDataDir() + L"\\settings.ini", tmp = path + L".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        for (auto& fd : kFields) {
            if (fd.legacy) continue;
            f << fd.key << "=";
            std::visit([&](auto m) {
                using T = std::remove_cvref_t<decltype(this->*m)>;
                if constexpr (std::is_same_v<T, std::string>) f << this->*m;
                else if constexpr (std::is_same_v<T, bool> || std::is_same_v<T, int>) f << (int)(this->*m);
                else if constexpr (std::is_same_v<T, float>) f << this->*m;
                else f << (int)(this->*m);
            }, fd.member);
            f << "\n";
        }
        if (!f.good()) { Log(L"settings: write failed"); return; }
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        Log(L"settings: replace failed %lu", GetLastError());
}
