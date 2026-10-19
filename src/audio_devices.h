#pragma once
#include "common.h"

// Audio output devices: the list Windows shows under "Output", and switching the default one.
namespace audio {
    struct Device { std::wstring id, name; bool isDefault = false; };
    std::vector<Device> Outputs();                 // active playback devices
    bool SetDefault(const std::wstring& id);       // false: the caller opens the sound settings instead
}
