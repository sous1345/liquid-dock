#pragma once
#include "common.h"

// OLED burn-in guard: black wallpaper (all monitors or one) and hidden desktop icons.
// The dock already stays hidden until the pointer reaches the edge. The original
// wallpaper, background colour and icon state are kept in data\oled.ini so the mode
// survives restarts and "--restore" (uninstall, restore shortcut) can undo it.
namespace oled {
    bool Active();
    // monitor == nullptr: every monitor
    bool Enable(HMONITOR monitor);
    bool Disable();
    // after an Explorer restart or at startup: hide the desktop icons again if the mode is on
    void Reapply();
}
