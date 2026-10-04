#pragma once
#include "common.h"

// Opens Windows' own tray windows from the dock instead of re-implementing them.
namespace nativetray {
    // The real "hidden icons" overflow window, moved next to the dock. Returns false when the
    // taskbar has no classic overflow button (stock Windows 11 taskbar).
    bool OpenOverflow(POINT anchor, Edge edge, RECT dockBar);
    // Stock Windows 11 taskbar: remember its tray buttons (hidden icons, network, clock) while the taskbar is still expanded
    // (call before hiding it); the openers need them afterwards.
    void CacheTrayButtons(HWND tray);
    void OpenCalendar();                    // date/time flyout (Win+Alt+D)
    void OpenVolume(POINT center);          // volume slider at a screen position
    void OpenQuickSettings();               // Wi-Fi, Bluetooth, ... (Win+A)
}
