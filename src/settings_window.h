#pragma once
#include "common.h"

// "Dock shape" window: position, icon size, glass thickness, distance from the edge and the
// position along the edge, with sliders that change the docks live. One instance at a time.
namespace shapewindow {
    void Show(HMONITOR monitor);
    bool IsOpen();
    bool PreTranslate(MSG* msg);   // Tab / Esc / Enter for the window, from the main loop
}
