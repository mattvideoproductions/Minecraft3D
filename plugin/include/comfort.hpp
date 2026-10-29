#pragma once
// Comfort 3D (v0.3): both eyes rendered in the SAME frame by replaying the
// frame's top-screen command range for the right eye, plus a 30 fps pacer.
// Design: the project's design notes (not in this repo) (sections 1, 7), replay details
// the project's design notes (not in this repo) (sections 1-5).
//
// Per frame (game main thread, inside GxDevice::present, after the game's
// own top-screen B2L at 0x4F8918):
//   1. parse this command list (pica_shadow) with two marks:
//        p0 = cursor right after the stage-B top clear (GxDevice::clear hook)
//        pH = cursor at GxDevice::setRenderTarget(2) (bottom stage starts)
//      and record every WORLDVIEWPROJ (c0-c3) / PROJ (c8-c11) upload in [p0, pH);
//   2. B2L the finished (left) top picture into the left eye buffer, clear
//      the top colour+depth target with the game's own clear (same colour);
//   3. write  prologue | copy of [p0, pH) with right-eye matrices | epilogue
//      at the command cursor, patch the ORIGINAL range to left-eye matrices,
//      split (render request) and B2L the right picture into the right buffer.
//   Anything unexpected: nothing is written and the caller shows the frame
//   flat or 2D (never a half-built frame).
#include <3ds/types.h>
#include "c3_stats.hpp"

namespace mc3d::comfort
{
    // Fallback reasons (log/CSV order is stable). 16+ = pica::Fail + 16.
    enum Reason : u8
    {
        kOk = 0,
        kNotReady,      // a required hook/table field is missing
        kWarmup,        // GPU state shadow (re)starting: one or two frames
        kNoP0,          // no top clear seen this frame
        kNoPH,          // no setRenderTarget(2) after it
        kList,          // marks were taken in a different command list
        kOrder,         // marks out of order / outside the list / misaligned
        kReqInRange,    // too many / unknown GPU requests inside [p0, pH) (v0.3.1: others are replayed)
        kClears,        // more than one top clear this frame
        kWorldCalls,    // world render entered more than once
        kHeadroom,      // command-list request queue too full
        kPendingJump,   // nngx has a pending sub-list (cl+0x30/0x34)
        kCursorMoved,   // cursor moved unexpectedly while committing
        kParse,         // command stream parse error (shadow reset)
        kSplitFailed,   // our split did not add exactly one request
        kNoRoom,        // command buffer too full for the copy
        kSegments,      // v0.3.1: request boundaries in the range do not line up with the stream
        kPicaBase = 17,
    };
    const char *ReasonName(u8 r);

    struct Params
    {
        float a;            // world: slider*px/200 (NDC units, full disparity between the eyes)
        float conv;         // zero-parallax distance (blocks), from the comfort cap
        float hand, pan, ui, hud;   // constant classes, full disparity in NDC (slider*px/200)
    };

    // ---- hook feeds (game main thread) ----
    void OnTopClear(u32 dev, const u8 *rgba, u32 target);   // after GxDevice::clear returned
    void OnSetRenderTarget(u32 target);                      // before GxDevice::setRenderTarget
    void OnWorldEnter();                                     // 0x31DD8C entry
    void OnWorldExit();                                      // 0x31DD8C return
    void OnLevelRenderReturn();                              // `bl 0x221BB8` returned (hand starts)
    bool AfterLevelRender();                                 // inside the world render, after the level (hand/overlays)
    void OnProjRefresh(const float *proj);                   // WorldConstants::refresh, world class: PROJ stack top

    // ---- frame (main thread, present, after the game's top B2L) ----
    // wanted = Comfort mode, 3D on, slider > 0 (keeps the GPU-state shadow warm).
    //   None     nothing was issued; the top target still holds the frame
    //            (the caller may show it flat or 2D)
    //   LeftOnly the left copy and the right-eye clear were issued but the
    //            replay was not (rare); the top target is cleared: show 2D
    //            from the game's own buffer
    //   Both     dstL and dstR receive this frame's two eyes
    enum class Result : u8 { None, LeftOnly, Both };
    Result Frame(u32 dev, bool wanted, u32 dstL, u32 dstR, const Params &p);
    void EndFrame();                                         // reset per-frame marks (always, after Frame)

    // ---- RunCmdlist site (main thread, before RunCmdlist 0x4F8A30) ----
    void BeforeRunCmdlist();
    void SetPacer(bool on);                                  // 30 fps lock wanted this frame
    bool GovernorAllows();                                   // false while a cool-down runs (too many late frames)
    void SetGovernor(bool on);                               // v0.3.1: off by default; cfg gov=on enables it
    bool GovernorEnabled();
    bool PacerOn();

    // ---- swap (GSP cmdlist-done thread) ----
    void OnSwap(u32 left, u32 right);

    // Installed hooks report (status box)
    void Install();                     // PatchProcess: level-render site hook
    bool Ready();                       // every hook/field the replay needs is present
    const char *NotReadyWhy();
    void SetHooksPresent(bool clear, bool rt, bool world, bool proj);

    // ---- stats ----
    void NoteListKind(u32 list, bool replay);               // which kind of list is running (GPU split)
    void OnListGpu(u32 list, u32 ticks);                     // GSP thread: GPU time of a finished list
    void TakeStats(ComfortStats &out);                       // main thread, once per second
    u32  LastReason();
    u32  Replayed();
    // v0.3.1: a frame whose replay failed kept the previous pair (held) or was shown 2D
    void NoteShown(bool held);
    void ReadTotals(ComfortTotals &out);                     // plugin thread: session totals (fallback log)
}
