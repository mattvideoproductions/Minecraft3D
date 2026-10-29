#pragma once
// Internal interface of the v9.12 flat-parallax stereo (stereo_flat.cpp).
// Public API: stereo.hpp.
#include <3ds/types.h>
#include <string>
#include "stereo.hpp"

namespace mc3d::stereo::flat
{
    void        Install();                  // PatchProcess, after version::Select()
    std::string HookStatus();
    void        SetEnabled(bool on);
    State       GetState();
    std::string DebugText();
}
