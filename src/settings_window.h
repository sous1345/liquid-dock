#pragma once
#include "common.h"

// The settings window: every option on four tabs (shape, behaviour, buttons, system), drawn with
// Direct2D over the Windows 11 acrylic backdrop (the taskbar's translucent look; a solid colour on
// Windows 10). Changes reach the docks while sliders move. One instance at a time.
namespace settingswindow {
    void Show(HMONITOR monitor);
    void Close();                  // e.g. before a GPU reset: its swap chain belongs to the old device
    bool IsOpen();
    bool PreTranslate(MSG* msg);   // Esc, from the main loop
    void PlaceBottomRight();       // developer (recordings): into the lower right corner of its monitor
}
