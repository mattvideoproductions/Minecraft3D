#pragma once
// v0.3 world options (the project's design notes (not in this repo) sections 2 and 3):
//   - 3D render distance: a render-only clamp of the radius LevelRenderer::
//     startFrame uses (instruction hook at 0x3C5ACC). Loading and ticking keep
//     the game's radius. Auto-lowered while 3D is on, with hysteresis:
//     shrink at once (free: the outer ring is dropped), grow back only after
//     3D has been off for 5 s and at most once per 20 s (a grow rebuilds the ring).
//   - Render distance (normal play): the game's own Options::set on
//     gfx_renderdistance_new (+ its listener: player chunk radius). Values
//     above the game's own maximum are "experimental" and need the memory guard.
//   - Fast leaves: Options+0x6F + the game's graphics-mode refresh 0x222684 +
//     a full chunk rebuild (LevelRenderer+0x200 = -1).
//   - Far fog override: the float at 0xA3510C (experimental, off by default).
//   - Memory guard: walks the game's nn ExpHeap free lists (read only).
// All game calls/writes happen on the game main thread (Update, from the
// present hook after the frame is done); the menu only sets choices.
#include <3ds/types.h>
#include <string>
#include "c3_stats.hpp"

namespace mc3d::world
{
    void Install();                 // PatchProcess
    bool ClampInstalled();

    // ---- choices (plugin thread) ----
    void SetRenderDistance(int chunks);     // 0 = keep the game's setting
    int  RenderDistance();
    void SetAutoLower(bool on);             // auto-lower in 3D (default on)
    bool AutoLower();
    void SetDistance3D(int chunks);         // 0 = automatic rule, else chunks (2 = experimental)
    int  Distance3D();
    enum class Leaves : u8 { Auto, Fast, Fancy };
    void SetLeaves(Leaves m);
    Leaves GetLeaves();
    void SetFarFog(int blocks);             // 0 = off
    int  FarFog();

    int  LevelsFront();                     // game's render distance list (chunks), -1 unknown
    int  LevelsBack();
    bool IsExperimental(int chunks);        // above the game's own maximum
    const char *TakeNotice();               // plugin thread: one pending message or nullptr
    // v0.3.2: menu text for the render distance entry, from what the game
    // actually uses: "6 -> 4 (3D auto)", "6 refused: low memory, 4 used"...
    // (plugin thread; built from values the main thread publishes).
    std::string RdStatusName();
    int  DrawnChunks();                     // main thread: chunks drawn now (after the 3D clamp), -1 unknown

    // ---- game main thread, once per frame after present ----
    // comfortActive: Comfort 3D drawing this frame (slider > 0) - drives the
    //   free, render-only distance clamp (with hysteresis).
    // comfortSelected: Comfort mode chosen and 3D switched on in the menu,
    //   independent of the slider - drives Auto fast leaves (a rebuild), so
    //   slider moves never rebuild the world (audit S3).
    void Update(bool comfortActive, bool comfortSelected);
    void TakeStats(WorldStats &out);        // main thread, once per second
    void SetHeapStatsWanted(bool on);       // logging on: walk the heaps every second

    // Pure helpers (host-tested in plugin/tests/)
    int  Auto3DChunks(int normalChunks, int choice);  // the 3D distance for a normal distance
    struct Hyst
    {
        bool applied;       // 3D state in effect
        bool haveOff;
        u32  offSince;      // tick 3D went off
        bool haveGrow;
        u32  lastGrow;      // tick of the last grow-back
    };
    // Shrink at once; grow back after `offTicks` of 3D off, at most once per `gapTicks`.
    bool HystStep(Hyst &h, bool want, u32 now, u32 offTicks, u32 gapTicks);
}
