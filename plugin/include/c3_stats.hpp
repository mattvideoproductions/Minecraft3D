#pragma once
// v0.3 per-second statistics shared by comfort.cpp, world_opts.cpp and the
// profiler roll-up (profiler.cpp). Plain data only.
#include <3ds/types.h>

namespace mc3d
{
    constexpr u32 kReasonCount = 32;

    struct ComfortStats
    {
        // replay (main thread)
        u32 tried, replayed, flatFallback, monoFallback;
        u16 reason[kReasonCount];       // fallback count per reason (comfort::ReasonName)
        u32 parseTicks, parseMaxTicks;  // stream parse
        u32 buildTicks;                 // emit + copy + patch + flush
        u32 copyBytes, proWords, epiWords;  // sums (per replayed frame)
        u32 sitesWorld, sitesHand, sitesHud, sitesMenu, proj8Match, proj8Miss;
        u32 menuFrames;                 // frames with no world render (title, chest, furnace...)
        // pacer (main thread)
        u32 paceRuns, paceWaitTicks, paceMaxWaitTicks, paceLate, paceTimeout;
        // presentation (GSP thread): per eye, intervals in vblanks between image changes
        u32 hist[2][4];                 // 1, 2, 3, 4+ vblanks
        u32 ivSum[2], ivSq[2];          // sum / sum of squares of intervals (vblanks)
        u32 eyeSplit;                   // swaps where exactly one eye changed
        u32 vblanks;                    // vblank counter advance during the window (self-check)
        // GPU per list, split by frame kind (RunCmdlist -> done)
        u32 gpuCN, gpuCTicks;           // lists with the replay (both eyes)
        u32 gpuMN, gpuMTicks;           // other lists (one eye / 2D)
        u8  paceOn, active, mode, preset;
        // v0.3.1: GPU requests the game queued inside the top range
        u32 reqFrames;                  // frames with any
        u32 reqTypes[5];                // by type: 0 DMA, 1 render (split), 2 fill, 3 transfer, 4 texture copy
        u32 reqDump[4][7];              // first entries of the window, raw 0x1C bytes
        u32 reqDumpN;
        u32 caller[3][2];               // return addresses: gd split 0x4B5C3C, ClearTargets 0x4B5270, FrameBuffer::clear 0x504158
        u32 callerN[3];
        u32 segmented;                  // replayed frames that needed more than one piece
        u32 reqRefused;                 // frames shown 2D: an in-range request that is not a top-target fill
        u32 govTrips, govCoolFrames;    // governor: suspensions, frames spent cooling down
        u32 emitTicks;                  // prologue + epilogue emission (scratch), summed over tried frames
        u32 held;                       // v0.3.1: failed frames that kept the previous stereo pair on screen
    };

    // v0.3.1: session totals for the always-on fallback log (never reset;
    // main thread writes, the plugin thread copies them every ~10 s, a torn
    // read only skews one line by a count).
    struct ComfortTotals
    {
        u32 tried, replayed, held, mono;    // Comfort frames attempted / both eyes / previous pair held / shown 2D
        u32 govTrips, govCoolFrames;        // governor (opt-in since v0.3.1)
        u32 paceRuns, paceLate;             // paced frames / frames that missed their 2-vblank slot
        u32 reason[kReasonCount];           // fallback count per reason (comfort::ReasonName)
    };

    // slot = index in the game's heap table (0xA30D08 + 4*slot); parent = it
    // contains another listed heap (its free list excludes that child).
    struct HeapStat { u32 capKB, freeKB, largestKB; u8 ok, slot, parent, guarded; };
    constexpr u32 kHeapSlots = 6;

    struct WorldStats
    {
        s32 optRd;          // Options+0x64 (chunks), -1 unknown
        s32 ctxBlocks;      // render radius the renderer asked for (ctx+0x20, blocks, before the clamp)
        s32 lrBlocks;       // LevelRenderer+0x200 (blocks, after the clamp)
        s32 clampBlocks;    // our clamp (0 = none)
        float lrFar;        // LevelRenderer+0x208 (far / fog distance)
        float farOverride;  // 0xA3510C
        s8  leaves;         // Options+0x6F (1 fancy/transparent, 0 fast), -1 unknown
        u8  rdAuto, leavesMode, guardOk;
        u16 rebuilds, rdChanges, guardRefusals, stepDowns;
        HeapStat heap[kHeapSlots];
        u8  heapCount, heapWalkOk;
        u16 saveSubst;      // options.txt saves where a forced value was replaced by the player's (total)
    };
}
