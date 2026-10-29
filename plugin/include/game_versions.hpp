#pragma once
// Per-game-version address tables for Minecraft: New Nintendo 3DS Edition (USA 00040000001B8700).
//
// One GameVersion per supported build. At boot version::Select() picks the
// first table whose every signature (24 bytes at the expected address), site
// instruction word and literal-pool word matches the running code. If none
// matches, the plugin stays idle and patches nothing.
//
// A field left at 0 means "not known for this build": the feature that needs
// it is switched off, the rest still works. So a partially ported table (for
// example only the frame hook + profiler) is valid.
//
// Two stereo designs exist (see the project's design notes (not in this repo)):
//   StereoKind::KillSwitch   v1.0   the game's own right-eye renderer, switched on
//   StereoKind::FlatParallax v9.12  renderer was removed; the plugin copies the
//                                   finished top image into its own right-eye
//                                   buffers and scans them out shifted (flat depth)
#include <3ds/types.h>

namespace mc3d::game
{
    constexpr u64 kTitleId = 0x00040000001B8700ULL;

    struct Sig
    {
        u32 addr;
        u8  bytes[24];
    };

    enum class StereoKind : u32
    {
        None = 0,
        KillSwitch = 1,
        FlatParallax = 2,
    };

    struct GameVersion
    {
        const char *name;
        StereoKind  stereoKind;

        // exheader segments (flat at 0x100000); dataEnd includes .bss
        u32 textStart, textEnd, roStart, roEnd, dataStart, dataEnd;

        // ---- functions (hook targets must each have a Sig) ----
        u32 fnPresent;          // GxDevice::present(this)            frame boundary (+ stereo control on v1.0)
        u32 fnStereoReplay;     // replayCmdlistForRightEye           sig only (v1.0)
        u32 fnWorldRender;      // GameRenderer render-level (r0-r3, s0)
        u32 fnDraw;             // RenderContext::draw
        u32 fnDrawIndexed;      // RenderContext::drawIndexed
        u32 fnBuildImmediate;   // sync chunk build (_buildImmediate / buildChunk)
        u32 fnStartRebuild;     // RenderChunk::startRebuild
        u32 fnEndRebuild;       // RenderChunk::endRebuild
        u32 fnJobBudget;        // WorkerPool::runCoroutines budget (r0, d0)
        u32 fnSetDisplayMode;   // nngxSetDisplayMode(u32 mode)
        u32 fnSwapBuffers;      // v1.0 nngxSwapBuffers / v9.12 nngxSwapBuffersByAddress (sig only / called)
        u32 fnWorldConstants;   // WorldConstants::refresh            sig only
        u32 fnGxInit;           // GxDevice::init(this)               sig only (v1.0) / hooked (v9.12)

        // ---- v1.0 kill-switch sites inside fnPresent ----
        u32 siteFlagClear;      // strb rX,[rFlag]   gStereoThisFrame = 0 (never patched, see stereo.cpp)
        u32 siteCmdlistGate;    // cmp r0,#0 before strbeq/strbne [rFlag,#1]
        u32 insnFlagClear;      // expected words at those sites
        u32 insnCmdlistGate;
        u32 insnCmdlistGatePatched; // cmp r0,r0

        // ---- literal-pool words that must hold the data addresses below ----
        u32 litStereoFlag;
        u32 litNngxState;

        // ---- data ----
        u32 stereoFlag;         // u8 gStereoThisFrame; +1 = u8 gCmdlistSaved (v1.0)
        u32 nngxState;          // nngx internal state
        u32 offDisplayMode;     // nngxState + this = current display mode (0x700 / 0x702)
        u32 offRightRowOffset;  // nngxState + this = s32 right-eye (DISPLAY0_EXT) row offset (v1.0 SwapBuffers)

        // ---- v9.12 flat parallax (all required for StereoKind::FlatParallax) ----
        u32 fnSwapCallback;     // GxDevice cmdlist-done callback (does both swaps)       sig only
        u32 fnCopyB2L;          // gd CopyTexture2DResourceBlockToLinear(res,mip,srcY,dst,w,h,fmt,aa,flip)
        u32 fnActiveDisplay;    // nngxActiveDisplay(0x400/0x401/0x410)
        u32 fnGenDisplaybuffers;// nngxGenDisplaybuffers(n, ids)
        u32 fnBindDisplaybuffer;// nngxBindDisplaybuffer(id)
        u32 fnDisplaybufferStorage; // nngxDisplaybufferStorage(fmt, w, h, area)
        u32 fnGetDisplaybufferParam;// nngxGetDisplaybufferParameteri(pname, &v)
        u32 siteTopB2L;         // bl fnCopyB2L in present (top RT -> left display buffer); r4 = dev, r6 = idx
        u32 insnTopB2L;
        u32 siteTopSwap;        // bl fnSwapBuffers in fnSwapCallback (top screen, r2 = 0)
        u32 insnTopSwap;
        u32 litGxDeviceBase;    // literal in fnSwapCallback holding gxDeviceBase
        u32 gxDeviceBase;       // u32 at +4 = GxDevice singleton
        u32 litSwapFormat;      // literal in fnSwapCallback holding the display format (0x8051 RGB8)
        u32 swapFormat;
        // GxDevice field offsets
        u32 devTopColorRes;     // top colour Texture2DResource*
        u32 devTopWidth;        // top display buffer width  (240)
        u32 devTopHeight;       // top display buffer height (400)
        u32 devTopDispAddr;     // u32[3] top display buffer addresses (left eye)
        u32 devCurList;         // command list being recorded (0/1)
        u32 devRunList;         // command list last run (read by the swap callback)
        u32 devSwapIdx;         // display buffer index of the list last run
        u32 devVramANext;       // nngx VRAM-A bump pointer (diagnostics)
        u32 devVramBNext;       // nngx VRAM-B bump pointer (diagnostics)
        // nngx command-list request queue (v0.1.1 headroom guard). The game
        // never checks the request count against the capacity, so the plugin
        // does before it adds its copies. All 0 = unknown: no 3D is drawn.
        u32 litBoundCmdlist;    // literal in the request-add wrapper; must hold nngxState
        u32 offBoundCmdlist;    // nngxState + this = bound command list object
        u32 clReqMax;           // cmdlist + this = request capacity (nngxCmdlistStorage arg 2)
        u32 clReqUsed;          // cmdlist + this = requests used (reset by nngxClearCmdlist)

        // ---- game's built-in profiler (read only; 0 = not available) ----
        u32 litGameProfiler;    // literal holding gameProfiler
        u32 gameProfiler;       // ctx; section id i = {u32 n; pad; f64 excl; f64 incl; f64 maxIncl} at ctx + 32*i

        // ---- profiler timing sites (v0.1.1; optional, 0 = not hooked) ----
        // `bl` sites wrapped with register-preserving tick stubs (profiler.cpp).
        u32 sitePresentFlush;   // bl cmdlist flush in present
        u32 insnPresentFlush;
        u32 sitePresentWait;    // bl WaitCmdlistDone(previous list) in present (pure GPU wait)
        u32 insnPresentWait;
        u32 sitePresentRun;     // bl RunCmdlist in present (GPU submit)
        u32 insnPresentRun;
        u32 siteJobRun;         // bl run-one-coroutine inside the job budget loop
        u32 insnJobRun;

        // ---- v0.2 true depth (optional, 0 = flat parallax only) ----
        // WorldConstants::refresh (fnWorldConstants) reads three matrix stacks
        // {begin, end, ...; u8 dirty @+0xC}, elements 0x44 B, top = end - 0x44
        // (0x6A6AD4), float[16] column-major at +0. WVP = P*(V*W) (0x725884).
        u32 litProjStack;       // literal in fnWorldConstants holding projStack
        u32 projStack;          // u32* -> projection matrix stack
        u32 viewStack;          // u32* -> view (camera) matrix stack
        u32 fnSetRenderTarget;  // GxDevice::setRenderTarget(dev, n): 0 top, 2 bottom (v0.2 bottom-screen gate)
        u32 cmdCursor;          // u32 gd/nngx command-buffer write pointer (M0 list usage)
        u32 clBufBase;          // cmdlist + this = buffer base (nngxCmdlistStorage)
        u32 clBufSize;          // cmdlist + this = buffer size in bytes

        const Sig *sigs;
        u32 sigCount;

        // ---- v0.3 Comfort 3D: same-frame replay + 30 fps pacer (0 = feature off) ----
        u32 fnGxClear;          // GxDevice::clear(dev, const u8 rgba[4]*, target): hooked (p0 mark) and called (right-eye clear)
        u32 fnSplit;            // nngx split (render request for everything since the last split), called
        u32 litSplitCursor;     // literal in fnSplit that must hold cmdCursor
        u32 litSplitLimit;      // literal in fnSplit that must hold cmdLimit
        u32 cmdLimit;           // u32 = end of the writable command buffer (the split's own bound)
        u32 clPendA, clPendB;   // cmdlist + these: pending sub-list state; both must be 0 to append
        u32 siteLevelRender;    // `bl` LevelRenderer render wrapper inside fnWorldRender (hand starts after it)
        u32 insnLevelRender;
        u32 siteOverlayRender;  // conditional `bl` after it (block outline/overlay, LevelRenderer): the hand starts after this one
        u32 insnOverlayRender;
        u32 offVblankTop;       // nngxState + this = top-screen vblank counter (incremented by the vblank handler)
        u32 litVblankState;     // literal in that handler that must hold nngxState
        // ---- v0.3 render distance / fast leaves / far fog ----
        u32 siteRdClamp;        // `ldr r0,[r6,#0x20]` in LevelRenderer::startFrame (r4 = LevelRenderer, r6 = ctx)
        u32 insnRdClamp;
        u32 offLrRadius;        // LevelRenderer + this = render radius in use (blocks; -1 forces a full rebuild)
        u32 offLrFar;           // LevelRenderer + this = far/fog distance (float)
        u32 offLrClient;        // LevelRenderer + this = client
        u32 offClientOptions;   // client + this = Options*
        u32 offOptRenderDist;   // Options + this = render distance (int, chunks)
        u32 offOptLeaves;       // Options + this = gfx_transparentleaves (u8)
        u32 fnOptionsSetInt;    // Options::set(Options*, const Option*, int): writes + notifies listeners
        u32 optRenderDistance;  // Option object of gfx_renderdistance_new
        u32 litOptRenderDistance; // literal in fnOptionsSetInt that must hold optRenderDistance
        u32 fnGraphicsChanged;  // (client): applies fancy/transparent-leaves to every block
        u32 rdLevels;           // static std::vector<int> of the game's render distance levels
        u32 litRdLevels;        // literal (options load clamp) that must hold rdLevels
        u32 farOverride;        // float debug override of the far distance (-1 = off)
        u32 litFarOverride;     // literal in startFrame that must hold farOverride
        u32 heapTable;          // u32[] of heap wrapper objects (memory guard)
        u32 litHeapTable;
        u32 heapVtable;         // heap wrapper vtable; ExpHeap at object + 0x18
        u32 litHeapVtable;
        u32 fnOptionsSave;      // Options::save(Options*): writes options.txt (+0x64 at 0x634A00, +0x6F at 0x634A3C)
        // v0.3.1 diagnostics (log who queues GPU requests inside the top range)
        u32 fnGdFlush;          // gd: split + clear render-target dirty flags (0x4B5C3C)
        u32 fnGdClearTargets;   // gd ClearTargets(color, depth, rgba*, stencil; s0 = depth)
        u32 fnFbClear;          // mce FrameBuffer::clear(fb, rgba*, attachment)
        // ---- v0.3.3 A/B swap: the game's only pad reads (SDK ExtraPadReader) ----
        u32 fnPadReadLatest;    // ExtraPadReader::ReadLatest(reader, status*) -> bool; status 0x18 B, hold/trig/rel @+8/+0xC/+0x10
        u32 fnPadRead;          // ExtraPadReader::Read(reader, status[], s32 *count, s32 max)
    };

    // All known builds, in the order they are tried.
    extern const GameVersion *const kVersions[];
    extern const u32 kVersionCount;

    constexpr u32 kModeNormal = 0x700;
    constexpr u32 kModeStereo = 0x702;
}
