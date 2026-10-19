#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <windowsx.h>
#include <wrl/client.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <d2d1_3.h>
#include <dwrite_3.h>
#include <dcomp.h>
#include <wincodec.h>
#include <string>
#include <vector>
#include <memory>
#include <algorithm>
#include <cmath>
#include <functional>
#include <unordered_map>

using Microsoft::WRL::ComPtr;

constexpr float kPi = 3.14159265358979f;

inline float clampf(float x, float a, float b) { return x < a ? a : (x > b ? b : x); }
inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
inline float smooth01(float t) { t = clampf(t, 0, 1); return t * t * (3 - 2 * t); }

double NowSeconds();
void Log(const wchar_t* fmt, ...);
std::wstring AppDataDir();          // %APPDATA%\LiquidDock (created)
std::wstring ToLower(std::wstring s);
std::string  ToUtf8(const std::wstring& w);
std::wstring FromUtf8(const std::string& s);
std::wstring SystemExe(const wchar_t* name);   // "C:\Windows\System32\<name>", quoted

// Critically-damped-ish spring used for every animation in the dock.
struct Spring {
    float x = 0, v = 0, target = 0;
    void step(float dt, float stiffness, float dampingRatio) {
        float c = 2.0f * dampingRatio * std::sqrt(stiffness);
        while (dt > 0) {
            float h = dt > (1.0f / 240.0f) ? (1.0f / 240.0f) : dt;
            float a = -stiffness * (x - target) - c * v;
            v += a * h;
            x += v * h;
            dt -= h;
        }
    }
    bool settled(float eps = 0.0015f) const { return std::fabs(x - target) < eps && std::fabs(v) < eps * 20; }
    void snap(float t) { x = target = t; v = 0; }
};

enum class Edge { Bottom = 0, Left = 1, Right = 2, Top = 3 };
inline bool IsVertical(Edge e) { return e == Edge::Left || e == Edge::Right; }

struct Settings {
    Edge  edge = Edge::Bottom;
    float iconSize = 44;        // at a 1440 px short screen side; scaled with the screen
    float magnification = 1.7f; // 1 = off
    int   glassStyle = 1;       // 0 clear, 1 normal, 2 frosted
    bool  showDesktopButton = true;
    bool  showThemeButton = true;   // Windows light/dark switch in the dock
    bool  showOledButton = false;   // OLED burn-in guard toggle in the dock
    bool  oledOneMonitor = false;   // black wallpaper only on the monitor whose dock was clicked
    int   clockSize = 0;        // 0 normal, 1 large; independent of icon size
    int   iconSpacing = 1;      // 0 compact, 1 normal, 2 wide
    float showDelay = 0.05f;    // seconds at edge before reveal
    float hideDelay = 0.35f;    // seconds outside before hide
    bool  autostart = false;
    bool  cpuTemp = false;      // opt-in: elevated helper + PawnIO driver
    int   pinsVersion = 0;      // 2 = store-app pins imported
    void Load();
    void Save() const;
};
