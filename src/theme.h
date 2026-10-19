#pragma once
#include "common.h"

// Windows light/dark mode, the same switch as Settings > Personalization > Colors.
namespace theme {
    bool IsLight();       // app mode
    bool Toggle();        // flips both the Windows and the app mode together
}
