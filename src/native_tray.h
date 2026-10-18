#pragma once
#include "common.h"

// Opens Windows' own tray windows from the dock instead of re-implementing them.
namespace nativetray {
    // The native hidden-icons flyout, anchored next to the dock on Windows 10/11.
    // Fails without exposing the taskbar if the visibility guard cannot be acquired.
    bool OpenOverflow(POINT anchor, Edge edge, RECT dockBar);
    // Stock Windows 11 taskbar: remember its tray buttons (hidden icons, network, clock) while the taskbar is still expanded
    // (call before hiding it); the openers need them afterwards.
    void CacheTrayButtons(HWND tray);
    void OpenCalendar();                    // date/time flyout (Win+Alt+D)
    void OpenVolume(POINT center);          // volume slider at a screen position
    void OpenQuickSettings();               // Wi-Fi, Bluetooth, ... (Win+A)
    bool ToggleDesktop();                  // Explorer owns show/restore state
}
