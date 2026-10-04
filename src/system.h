#pragma once
#include "common.h"
#include <atomic>
#include <mutex>

struct SysInfo {
    std::atomic<float> volume{ 0.5f };
    std::atomic<bool>  muted{ false };
    std::atomic<int>   net{ 1 };              // 0 offline, 1 ethernet, 2 wifi
    std::wstring timeText, dateText, timeShort;   // UI thread only
    std::atomic<int>   version{ 0 };          // bumps whenever something visible changed

    void Start();               // background polling of volume / network
    void Tick(double now);      // clock text (UI thread)
    void AddVolume(float delta);
    void ToggleMute();

private:
    double lastFast = 0;
};
extern SysInfo g_sys;

// Hides the native Windows taskbar while the dock runs and restores it afterwards.
class NativeTaskbar {
public:
    void Hide();
    void Restore();
    // Re-hide if explorer showed it again. The periodic check runs on its own thread
    // (StartEnforcer) because explorer calls can block for seconds after logon or resume.
    void Enforce(bool onUiThread = true);
    void StartEnforcer();
    std::atomic<bool> hidden{ false };
private:
    std::recursive_mutex mu;
    UINT_PTR originalState = 0;
    bool stateSaved = false;
};
extern NativeTaskbar g_taskbar;

void SendKeyCombo(std::initializer_list<WORD> keys);
