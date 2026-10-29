#pragma once
#include <3ds/types.h>
#include "game_versions.hpp"

namespace mc3d::version
{
    enum class Status
    {
        NotChecked,
        Ok,
        WrongTitle,     // not Minecraft NN3DS USA
        Unsupported,    // no address table matched this build
    };

    // Try every table in game::kVersions; the first one whose signatures,
    // site words and literal words all match becomes Current() and unlocks
    // mem:: writes. Run once from PatchProcess.
    Status  Select();

    Status                     GetStatus();
    bool                       IsSupported();
    const game::GameVersion   *Current();          // nullptr if unsupported
    const char                *StatusString(Status s);
    const char                *VersionName();      // "v1.0 (base)" or "unknown"

    // Diagnostics for the status box: per table, the first mismatching address.
    u32         TablesTried();
    u32         FirstMismatch(u32 table);
}
