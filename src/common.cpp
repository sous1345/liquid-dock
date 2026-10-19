#include "common.h"
#include <shlobj.h>
#include <cstdio>
#include <cstdarg>
#include <fstream>
#include <sstream>
#include <mutex>

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

void Settings::Load() {
    std::ifstream f(AppDataDir() + L"\\settings.ini");
    std::string line;
    while (std::getline(f, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        try {
            if (k == "edge") edge = (Edge)std::clamp(std::stoi(v), 0, 3);
            else if (k == "iconSize") iconSize = std::clamp(std::stof(v), 32.0f, 96.0f);
            else if (k == "magnification") magnification = std::clamp(std::stof(v), 1.0f, 2.5f);
            else if (k == "glassStyle") glassStyle = std::clamp(std::stoi(v), 0, 2);
            else if (k == "showDesktopButton") showDesktopButton = std::stoi(v) != 0;
            else if (k == "showOledButton") showOledButton = std::stoi(v) != 0;
            else if (k == "oledOneMonitor") oledOneMonitor = std::stoi(v) != 0;
            else if (k == "clockSize") clockSize = std::clamp(std::stoi(v), 0, 1);
            else if (k == "iconSpacing") iconSpacing = std::clamp(std::stoi(v), 0, 2);
            else if (k == "showDelay") showDelay = std::clamp(std::stof(v), 0.0f, 2.0f);
            else if (k == "hideDelay") hideDelay = std::clamp(std::stof(v), 0.0f, 3.0f);
            else if (k == "autostart") autostart = std::stoi(v) != 0;
            else if (k == "cpuTemp") cpuTemp = std::stoi(v) != 0;
            else if (k == "pinsVersion") pinsVersion = std::stoi(v);
        } catch (...) {}
    }
}

void Settings::Save() const {
    std::ofstream f(AppDataDir() + L"\\settings.ini", std::ios::trunc);
    f << "edge=" << (int)edge << "\n"
      << "iconSize=" << iconSize << "\n"
      << "magnification=" << magnification << "\n"
      << "glassStyle=" << glassStyle << "\n"
      << "showDesktopButton=" << (showDesktopButton ? 1 : 0) << "\n"
      << "showOledButton=" << (showOledButton ? 1 : 0) << "\n"
      << "oledOneMonitor=" << (oledOneMonitor ? 1 : 0) << "\n"
      << "clockSize=" << clockSize << "\n"
      << "iconSpacing=" << iconSpacing << "\n"
      << "showDelay=" << showDelay << "\n"
      << "hideDelay=" << hideDelay << "\n"
      << "autostart=" << (autostart ? 1 : 0) << "\n"
      << "cpuTemp=" << (cpuTemp ? 1 : 0) << "\n"
      << "pinsVersion=" << pinsVersion << "\n";
}
