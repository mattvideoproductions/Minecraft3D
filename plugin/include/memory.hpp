#pragma once
// Memory helpers for the game's .text / .rodata / .data(+bss).
//
// All writers are LOCKED until version::Select() found a matching address
// table, so nothing is ever written into an unknown game build. Writes into
// .text flush the data cache (Process::Patch) and then invalidate the WHOLE
// instruction cache with svcInvalidateEntireInstructionCache. Never use the
// range variant: on hardware it crashed the kernel (Terraria v0.2.0, data
// abort in i2c's context). See LESSONS.md (instruction cache).
#include <3ds/types.h>

namespace mc3d::mem
{
    enum class Region { None, Text, RoData, Data };

    Region  RegionOf(u32 addr, u32 len = 1);   // uses the selected GameVersion
    bool    PatchingAllowed();
    void    SetPatchingAllowed(bool allowed);  // called by version::Select()

    // Make .text writable once (CTRPF ProtectMemory = whole process RWX).
    // Call from PatchProcess, before the game runs; calling it mid-game from
    // the plugin thread crashed Terraria v0.3.0.
    void    PrepareTextWrites();

    // Raw reads (inside the game's segments only)
    bool    Read32(u32 addr, u32 &out);
    bool    Read8(u32 addr, u8 &out);

    bool    Write(u32 addr, const void *src, u32 len, void *original = nullptr);
    bool    Write32(u32 addr, u32 value, u32 *original = nullptr);
    bool    Write8(u32 addr, u8 value, u8 *original = nullptr);

    // Write only if the current word equals `expected` (per-patch guard).
    bool    Replace32(u32 addr, u32 expected, u32 value);
}
