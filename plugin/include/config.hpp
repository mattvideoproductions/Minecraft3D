#pragma once
// Per-hook enable flags, read once in PatchProcess from
//   /luma/plugins/00040000001B8700/mc3d_cfg.txt
// Format: one `key=value` per line, `#` starts a comment. A leading UTF-8 BOM,
// CRLF line ends and spaces around `=` are accepted (v0.1.1). Recognised:
//   hooks=present,init,b2l,swap,proj,prof     (comma or space separated)
// Group "prof" = world,draw,sync,rebuild,jobs (each can also be named alone).
// "all" = every hook, "none" (or an empty list) = no hooks at all.
// A hook that is not listed is not installed. File absent, or no `hooks=`
// line = every hook (the full working set).
//   irrst=on                                   (default off; v0.1.1)
// v0.2 start-up values of the stereo menu options (all changeable at runtime):
//   mode=depth|flat   tdepth=<preset 0..>   flatdepth=<preset 0..>
//   comp=on|off       leftvram=on|off       menupop=on|off
//   log=on (start with "Log stats to SD" on)   osd=on (stats on the bottom screen)
// v0.3: mode=comfort|alternate|flat (alternate = old "depth")   tdepth=<preset 0..>
//   lock=on|off (30 fps lock in Comfort 3D)   rd=<chunks, 0 = game>   rdauto=on|off
//   rd3d=<chunks, 0 = auto>   leaves=auto|on|off (on = fast)   farfog=<blocks, 0 = off>
//   hook names "comfort" (replay hooks) and "world" (render-distance clamp site).
// v0.3.2: hud=0|1|2 (top HUD Normal/Small/Smaller)
// v0.3.3: abswap=on|off (start with "Swap A/B buttons" on; default off),
//   hook name "input" (the two pad-read hooks the swap needs).
// Diagnostic only: lets CTRPF's hidInit() open ir:rst again, as v0.1.0 did.
// That broke the game's own C-stick on hardware (the project's design notes (not in this repo) "C-stick").
#include <3ds/types.h>

namespace mc3d::cfg
{
    enum HookBit : u32
    {
        kPresent = 1u << 0,
        kInit    = 1u << 1,
        kB2L     = 1u << 2,
        kSwap    = 1u << 3,
        kWorld   = 1u << 4,
        kDraw    = 1u << 5,     // draw + drawIndexed
        kSync    = 1u << 6,
        kRebuild = 1u << 7,     // startRebuild + endRebuild
        kJobs    = 1u << 8,
        kSplit   = 1u << 9,     // v0.1.1: present flush/wait/submit, GPU time, max single job
        kProj    = 1u << 10,    // v0.2: WorldConstants::refresh (true-depth projection shear)
        kComfort = 1u << 11,    // v0.3: GxDevice::clear + level-render site (Comfort 3D replay)
        kWorldOpts = 1u << 12,  // v0.3: render-distance clamp site 0x3C5ACC (3D distance, leaves, fog)
        kInput   = 1u << 13,    // v0.3.3: ExtraPadReader ReadLatest + Read (A/B swap)
        kProf    = kWorld | kDraw | kSync | kRebuild | kJobs | kSplit,
        kAll     = 0x3FFFu,
    };

    void        Load();                 // PatchProcess, before any hook is installed
    bool        Hook(u32 bits);         // true if every bit in `bits` is enabled
    u32         Hooks();
    bool        FromFile();             // a cfg file with a hooks= line was found
    const char *Path();
    bool        IrrstAllowed();         // irrst=on in the cfg file (default false)
    // v0.2 stereo start-up options; -1 = not given (keep the built-in default)
    int         StereoOpt(const char *key);
}
