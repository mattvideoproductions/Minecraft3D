// Comfort 3D (v0.3): same-frame right-eye replay + 30 fps pacer.
// See comfort.hpp for the frame flow and the project's design notes (not in this repo) "v0.3.0" for the
// evidence behind every address used here.
#include "comfort.hpp"
#include "pica_shadow.hpp"
#include "game_versions.hpp"
#include "version_check.hpp"
#include "hooks.hpp"
#include "config.hpp"
#include "profiler.hpp"

#include <3ds.h>
#include <CTRPluginFramework.hpp>
#include <cmath>
#include <cstring>

extern "C" void mc3d_stub_after_level_render(void);

namespace mc3d::comfort
{
    namespace
    {
        const game::GameVersion *V() { return version::Current(); }
        inline volatile u32 &W32(u32 a) { return *reinterpret_cast<volatile u32 *>(a); }
        inline u32 Tick() { return static_cast<u32>(svcGetSystemTick()); }
        // u32 ticks wrap every 16 s: wall-clock milliseconds come from the 64-bit tick
        inline u32 NowMs() { return static_cast<u32>(svcGetSystemTick() / (prof::kTicksPerSec / 1000)); }

        using FnB2L = s32 (*)(u32, u32, u32, u32, u32, u32, u32, u32, u32);
        using FnClear = void (*)(u32, const u8 *, u32);
        using FnSplit = void (*)(void);

        constexpr int kPadRows = 16, kRowBytes = 720;
        constexpr u32 kTicksMs = prof::kTicksPerSec / 1000;

        // ---- installed pieces ----
        bool s_hClear = false, s_hRt = false, s_hWorld = false, s_hProj = false;
        hook::FunctionHook h_clear;
        hook::SubWrapHook  h_level, h_overlay;
        const char *s_notReady = "not installed";

        // ---- per-frame marks (main thread) ----
        struct Marks
        {
            u32  list;
            u32  p0, reqP0, topClears;
            u8   rgba[4];
            bool haveP0;
            u32  pH, reqH;
            bool havePH;
            u32  worldCalls, pW0, pW1, pLevel;
            bool haveLevel, inWorld, afterLevel;
        };
        Marks s_m = {};
        bool  s_ourClear = false;           // our own right-eye clear is running

        // GPU requests inside [p0, pH): nngx request entries (0x7C777C:
        // cl+0x18 array, 0x1C bytes each; byte 0 = type 0 DMA, 1 render,
        // 2 fill, 3 transfer, 4 texture copy; render: +4 address, +8 size).
        constexpr u32 kMaxReq = 12, kReqSize = 0x1C;
        u32 s_req[kMaxReq][kReqSize / 4];
        pica::Piece s_pieces[kMaxReq + 1];
        // Prologue / epilogue emitted ONCE per frame into scratch (D-S1: the
        // emitter scans every slot; v0.3.0 ran it 5 times per frame), then
        // copied into the command buffer at commit.
        constexpr u32 kScratchWords = 16384;
        u32 s_proBuf[kScratchWords], s_epiBuf[kScratchWords];
        u32 s_proN = 0, s_epiN = 0;

        bool InRamPtr(u32 a) { return a >= 0x08000000 && a < 0x40000000; }

        // Diagnostics: game call sites of gd split / clear inside the range
        hook::FunctionHook h_dbgSplit, h_dbgClear, h_dbgFbClear;
        u32 s_callers[3][2];                // [split, ClearTargets, FrameBuffer::clear][first, last] return address
        u32 s_callerN[3];

        constexpr u32 kMaxProj = 8;
        u32 s_proj[kMaxProj][4][4];         // PROJ rows in upload order (w z y x)
        u32 s_projN = 0;

        // ---- GPU state shadow ----
        pica::State  s_cur, s_p0, s_pH;
        constexpr u32 kSiteCap = 2048;
        pica::Site   s_sites[kSiteCap];
        u32          s_leftW[kSiteCap][4];
        u32          s_rightW[kSiteCap][4];
        bool         s_patch[kSiteCap];
        u8           s_force[pica::kSlotCount];
        pica::Fail EmitScratch()
        {
            pica::Emitter a;
            a.p = s_proBuf;
            a.end = s_proBuf + kScratchWords;
            pica::Fail f = pica::EmitTransition(s_cur, s_p0, nullptr, a);
            if (f != pica::Fail::None)
                return f;
            s_proN = static_cast<u32>(a.p - s_proBuf);
            pica::Emitter b;
            b.p = s_epiBuf;
            b.end = s_epiBuf + kScratchWords;
            f = pica::EmitTransition(s_pH, s_cur, s_force, b);
            if (f != pica::Fail::None)
                return f == pica::Fail::UnknownAtP0 ? pica::Fail::EmitOverflow : f;
            s_epiN = static_cast<u32>(b.p - s_epiBuf);
            return pica::Fail::None;
        }
        pica::Parser s_parser;
        bool s_live = false;                // the shadow follows the GPU
        bool s_tailPending = false;         // parse this list's tail before RunCmdlist
        u32  s_tailFrom = 0, s_tailCl = 0;
        u32  s_warm = 0;                    // lists parsed in a row since the last reset
        u32  s_lastReason = kNotReady;
        u32  s_replayedTotal = 0;

        // ---- pacer (main thread) ----
        volatile bool s_paceWanted = false;
        bool s_haveLastRun = false;
        u32  s_lastRunV = 0;
        u32  s_paceTimeouts = 0;            // consecutive
        u32  s_paceOffUntil = 0;            // ms (NowMs); pacer suspended (vblank counter not moving)
        bool s_paceSuspended = false;
        // D-S1 governor: 6 late frames (picture held 3+ vblanks) out of the
        // last 60 -> Comfort off (flat plane, unpaced) for 10 s, then retry.
        // v0.3.1: OFF by default (opt-in `gov=on`). On hardware at 20-30 fps
        // almost every paced frame is late, so it tripped within a few
        // frames of every retry: 10 s of flat plane, a moment of 3D, again
        // (Matt: "swapping back and forth ... is not something I like").
        // Hardware testing found Comfort fine even at low fps (the project's design notes (not in this repo)).
        pica::Governor s_gov;
        volatile bool s_govAllows = true;
        bool s_govEnabled = false;

        // ---- stats ----
        ComfortStats s_st = {};             // main-thread part
        ComfortTotals s_tot = {};           // v0.3.1: session totals (always-on fallback log)
        // GSP-thread part (atomics)
        u32 s_hist[2][4], s_ivSum[2], s_ivSq[2], s_split;
        u32 s_lastAddr[2], s_lastV[2];
        bool s_haveV[2];
        u32 s_gpuCN, s_gpuCT, s_gpuMN, s_gpuMT;
        volatile u8 s_listKind[2];
        u32 s_vbAtWindow = 0;

        inline void Add(u32 &x, u32 v) { __atomic_fetch_add(&x, v, __ATOMIC_RELAXED); }
        inline u32 Take(u32 &x) { return __atomic_exchange_n(&x, 0u, __ATOMIC_RELAXED); }

        void NoteRequests(u32 n);

        // Memory-fill request (0x7C7A24 layout: +4/+8/+0xC buffer 0 start,
        // end, value; +0x10/+0x14/+0x18 buffer 1) whose buffers are exactly
        // the top colour (dev+0x88) / top depth (dev+0xB8) targets, or empty.
        bool FillOfTopTarget(u32 dev, const u32 *r)
        {
            if ((r[0] & 0xFF) != 2)
                return false;
            // GxDevice::clear 0x4F91A4 target 0: ClearTargets(dev+0x88, dev+0xB8)
            // and ClearTargets reads the buffer address at object +0x1C (param[7]).
            const u32 colRes = W32(dev + 0x88), depRes = W32(dev + 0xB8);
            if (!InRamPtr(colRes) || !InRamPtr(depRes) || (colRes & 3) || (depRes & 3))
                return false;
            const u32 col = W32(colRes + 0x1C), dep = W32(depRes + 0x1C);
            auto ok = [&](u32 start) { return start == 0 || start == col || start == dep; };
            return ok(r[1]) && ok(r[4]) && (r[1] || r[4]);
        }

        void Fallback(u8 r)
        {
            s_lastReason = r;
            if (r < kReasonCount && s_st.reason[r] < 0xFFFF)
                s_st.reason[r]++;
            if (r < kReasonCount)
                s_tot.reason[r]++;
        }

        u32 Cursor() { return W32(V()->cmdCursor); }
        u32 BoundCl() { return W32(V()->nngxState + V()->offBoundCmdlist); }
        u32 ReqUsed(u32 cl) { return W32(cl + V()->clReqUsed); }
        u32 VCount() { return W32(V()->nngxState + V()->offVblankTop); }

        bool ClOk(u32 cl)
        {
            return cl && !(cl & 3) && cl >= 0x08000000 && cl < 0x40000000;
        }

        // world/hand/hud/menu classification of one upload, as the FULL
        // disparity (right - left) in NDC units: k (x' = x + k*w) and kc.
        bool Classify(const pica::Site &s, const Params &p, float &K, float &KC)
        {
            K = KC = 0.0f;
            auto f = [](u32 w) { float x; std::memcpy(&x, &w, 4); return x; };
            if (s.base == 8)
            {
                // PROJ only if it is bit-identical to a projection matrix
                // WorldConstants::refresh uploaded for the world this frame
                // (renderchunk* PROJ = c8-c11; rain_snow, cloud, uv_scale,
                // uv_repeat use c8 for other values).
                bool match = false;
                for (u32 i = 0; i < s_projN && !match; i++)
                    match = std::memcmp(s_proj[i], s.raw, sizeof(s.raw)) == 0;
                if (!match)
                {
                    s_st.proj8Miss++;
                    return false;
                }
                s_st.proj8Match++;
                // audit S4: the match only says "this is PROJ" (perspective);
                // the class (world / hand / HUD / menu) comes from the
                // position, exactly as for c0, so a renderchunk-material draw
                // in the hand range gets the hand constant, not world depth.
            }
            bool ortho = false;
            if (s.base != 8)
            {
                const float x3 = f(s.raw[3][3]), y3 = f(s.raw[3][2]), z3 = f(s.raw[3][1]), w3 = f(s.raw[3][0]);
                if (!(std::isfinite(x3) && std::isfinite(y3) && std::isfinite(z3) && std::isfinite(w3)))
                    return false;
                ortho = std::fabs(x3) < 1e-5f && std::fabs(y3) < 1e-5f && std::fabs(z3) < 1e-5f &&
                        std::fabs(w3 - 1.0f) < 1e-5f;
            }
            const bool menu = s_m.worldCalls == 0;
            if (menu)
            {
                K = ortho ? p.ui : p.pan;
                s_st.sitesMenu++;
                return true;
            }
            const bool inWorld = s.pos >= s_m.pW0 && s.pos < s_m.pW1;
            if (ortho || !inWorld)
            {
                K = p.hud;          // in-world HUD / GUI: at the screen
                s_st.sitesHud++;
                return true;
            }
            const bool hand = s_m.haveLevel && s.pos >= s_m.pLevel;
            if (hand)
            {
                K = p.hand;
                s_st.sitesHand++;
                return true;
            }
            K = p.a;
            KC = p.a * p.conv;
            s_st.sitesWorld++;
            return true;
        }

        void ResetShadow()
        {
            s_parser.cur = &s_cur;
            s_parser.ResetShadow();
            s_warm = 0;
            s_live = true;
        }

        // Record who queued a request between p0 and pH (lr at entry).
        inline bool InWindow() { return s_m.haveP0 && !s_m.havePH && !s_ourClear; }
        void NoteCaller(int k, u32 lr)
        {
            if (!InWindow())
                return;
            if (s_callerN[k]++ == 0)
                s_callers[k][0] = lr;
            s_callers[k][1] = lr;
        }
        __attribute__((noinline)) u64 Hook_DbgSplit(MC3D_HOOK_ARGS)
        {
            NoteCaller(0, reinterpret_cast<u32>(__builtin_return_address(0)));
            return hook::FunctionHook::CallOriginal(MC3D_HOOK_PASS);
        }
        __attribute__((noinline)) u64 Hook_DbgClear(MC3D_HOOK_ARGS)
        {
            NoteCaller(1, reinterpret_cast<u32>(__builtin_return_address(0)));
            return hook::FunctionHook::CallOriginal(MC3D_HOOK_PASS);
        }
        __attribute__((noinline)) u64 Hook_DbgFbClear(MC3D_HOOK_ARGS)
        {
            NoteCaller(2, reinterpret_cast<u32>(__builtin_return_address(0)));
            return hook::FunctionHook::CallOriginal(MC3D_HOOK_PASS);
        }

        void NoteRequests(u32 n)
        {
            if (!n)
                return;
            s_st.reqFrames++;
            for (u32 i = 0; i < n && i < kMaxReq; i++)
            {
                const u32 t = s_req[i][0] & 0xFF;
                if (t < 5)
                    s_st.reqTypes[t]++;
                if (s_st.reqDumpN < 4)
                {
                    for (u32 j = 0; j < 7; j++)
                        s_st.reqDump[s_st.reqDumpN][j] = s_req[i][j];
                    s_st.reqDumpN++;
                }
            }
            for (int k = 0; k < 3; k++)
            {
                s_st.caller[k][0] = s_callers[k][0];
                s_st.caller[k][1] = s_callers[k][1];
                s_st.callerN[k] += s_callerN[k];
            }
        }

        u64 Hook_GxClear(MC3D_HOOK_ARGS)
        {
            const u64 r = hook::FunctionHook::CallOriginal(MC3D_HOOK_PASS);
            if (!s_ourClear)
                OnTopClear(a0, reinterpret_cast<const u8 *>(a1), a2);
            return r;
        }
    }

    // ---------------------------------------------------------------------
    const char *ReasonName(u8 r)
    {
        static const char *const names[] = {
            "ok", "not-ready", "warmup", "no-p0", "no-pH", "list", "order", "req-in-range", "clears",
            "world-calls", "headroom", "pending-jump", "cursor-moved", "parse", "split", "no-room", "segments",
        };
        if (r < sizeof(names) / sizeof(names[0]))
            return names[r];
        if (r >= kPicaBase && r < kPicaBase + static_cast<u32>(pica::Fail::Count))
            return pica::FailName(static_cast<pica::Fail>(r - kPicaBase));
        return "?";
    }

    void SetHooksPresent(bool clear, bool rt, bool world, bool proj)
    {
        s_hClear = clear || s_hClear;
        s_hRt = rt;
        s_hWorld = world;
        s_hProj = proj;
    }

    void Install()
    {
        const game::GameVersion *v = V();
        if (!v || !v->fnGxClear)
            return;
        if (cfg::Hook(cfg::kComfort))
        {
            h_clear.Install(v->fnGxClear, Hook_GxClear, hook::ExpectedFirstWord(v->fnGxClear));
            s_hClear = h_clear.Installed();
            if (v->siteLevelRender && prof::WorldHookInstalled())
                h_level.Install(v->siteLevelRender, v->insnLevelRender, nullptr, mc3d_stub_after_level_render);
            // The block outline / overlay after the level is world-space:
            // when it is drawn, the hand starts after it instead.
            if (v->siteOverlayRender && h_level.Installed())
                h_overlay.Install(v->siteOverlayRender, v->insnOverlayRender, nullptr, mc3d_stub_after_level_render);
            // Diagnostics (log only): who queues GPU requests inside the range.
            if (v->fnGdFlush)
                h_dbgSplit.Install(v->fnGdFlush, Hook_DbgSplit, hook::ExpectedFirstWord(v->fnGdFlush));
            if (v->fnGdClearTargets)
                h_dbgClear.Install(v->fnGdClearTargets, Hook_DbgClear, hook::ExpectedFirstWord(v->fnGdClearTargets));
            if (v->fnFbClear)
                h_dbgFbClear.Install(v->fnFbClear, Hook_DbgFbClear, hook::ExpectedFirstWord(v->fnFbClear));
        }
        s_parser.cur = &s_cur;
        s_parser.ResetShadow();
    }

    bool Ready()
    {
        const game::GameVersion *v = V();
        if (!v || !v->fnSplit || !v->cmdCursor || !v->cmdLimit || !v->clBufBase || !v->clBufSize ||
            !v->offBoundCmdlist || !v->clReqUsed || !v->clReqMax || !v->fnCopyB2L || !v->fnGxClear ||
            !v->clPendA || !v->clPendB || !v->offVblankTop)
            return (s_notReady = "table"), false;
        if (!s_hClear) return (s_notReady = "clear hook"), false;
        if (!s_hRt) return (s_notReady = "setRenderTarget hook"), false;
        if (!s_hWorld) return (s_notReady = "world hook"), false;
        if (!s_hProj) return (s_notReady = "projection hook"), false;
        if (!prof::RunHookInstalled()) return (s_notReady = "RunCmdlist site"), false;
        s_notReady = "ok";
        return true;
    }
    const char *NotReadyWhy() { return s_notReady; }

    // ---- hook feeds (main thread) ----
    void OnTopClear(u32 dev, const u8 *rgba, u32 target)
    {
        const game::GameVersion *v = V();
        if (!v || (target != 0 && target != 3) || !dev)
            return;
        s_m.topClears++;
        s_m.p0 = Cursor();
        const u32 cl = BoundCl();
        s_m.reqP0 = ClOk(cl) ? ReqUsed(cl) : 0xFFFFFFFF;
        s_m.list = W32(dev + v->devCurList) & 1;
        if (rgba)
            std::memcpy(s_m.rgba, rgba, 4);
        s_m.haveP0 = rgba != nullptr;
        s_m.havePH = false;
    }

    void OnSetRenderTarget(u32 target)
    {
        if (target != 2 || !s_m.haveP0 || s_m.havePH || !V())
            return;
        s_m.pH = Cursor();
        const u32 cl = BoundCl();
        s_m.reqH = ClOk(cl) ? ReqUsed(cl) : 0xFFFFFFFE;
        s_m.havePH = true;
    }

    void OnWorldEnter()
    {
        if (!V() || !V()->cmdCursor)
            return;
        if (++s_m.worldCalls == 1)
            s_m.pW0 = Cursor();
        s_m.inWorld = true;
        s_m.afterLevel = false;
    }

    void OnWorldExit()
    {
        if (!V() || !V()->cmdCursor)
            return;
        s_m.pW1 = Cursor();
        s_m.inWorld = false;
        s_m.afterLevel = false;
    }

    void OnLevelRenderReturn()
    {
        if (!V() || !V()->cmdCursor || !s_m.inWorld)
            return;
        s_m.pLevel = Cursor();          // the last one wins: level, then the overlay if drawn
        s_m.haveLevel = true;
        s_m.afterLevel = true;
    }

    bool AfterLevelRender() { return s_m.inWorld && s_m.afterLevel; }

    void OnProjRefresh(const float *m)
    {
        if (!m || s_projN >= kMaxProj)
            return;
        u32 rows[4][4];
        for (u32 i = 0; i < 4; i++)
        {
            std::memcpy(&rows[i][0], &m[12 + i], 4);   // w
            std::memcpy(&rows[i][1], &m[8 + i], 4);    // z
            std::memcpy(&rows[i][2], &m[4 + i], 4);    // y
            std::memcpy(&rows[i][3], &m[i], 4);        // x
        }
        for (u32 i = 0; i < s_projN; i++)
            if (std::memcmp(s_proj[i], rows, sizeof(rows)) == 0)
                return;
        std::memcpy(s_proj[s_projN++], rows, sizeof(rows));
    }

    // ---------------------------------------------------------------------
    // The frame. Main thread, inside present, after the game's top B2L.
    Result Frame(u32 dev, bool wanted, u32 dstL, u32 dstR, const Params &p)
    {
        if (!wanted)
        {
            s_live = false;
            s_tailPending = false;
            return Result::None;
        }
        s_st.tried++;
        s_tot.tried++;
        const game::GameVersion *v = V();
        if (!Ready() || !dev)
        {
            s_live = false;
            s_tailPending = false;
            Fallback(kNotReady);
            return Result::None;
        }
        const u32 cl = BoundCl();
        if (!ClOk(cl))
        {
            s_live = false;
            s_tailPending = false;
            Fallback(kOrder);
            return Result::None;
        }
        const u32 base = W32(cl + v->clBufBase), size = W32(cl + v->clBufSize);
        const u32 cur = Cursor(), limit = W32(v->cmdLimit);
        if (!base || (base & 7) || (cur & 7) || size > 0x400000 || cur < base || limit < cur ||
            limit > base + size)
        {
            s_live = false;
            s_tailPending = false;
            Fallback(kOrder);
            return Result::None;
        }

        // ---- continuity of the shadow: the previous list must have been
        // parsed up to its RunCmdlist (BeforeRunCmdlist), else start over.
        if (!s_live || s_tailPending)
            ResetShadow();
        s_parser.BeginList();

        // ---- marks ----
        const u32 list = W32(dev + v->devCurList) & 1;
        u8 markWhy = kOk;
        if (!s_m.haveP0) markWhy = kNoP0;
        else if (!s_m.havePH) markWhy = kNoPH;
        else if (s_m.list != list) markWhy = kList;
        else if (s_m.topClears != 1) markWhy = kClears;
        else if (s_m.worldCalls > 1) markWhy = kWorldCalls;
        else if (s_m.p0 < base || s_m.pH < s_m.p0 || s_m.pH > cur || (s_m.p0 & 7) || (s_m.pH & 7))
            markWhy = kOrder;
        else if (s_m.reqH < s_m.reqP0 || s_m.reqH - s_m.reqP0 > kMaxReq)
            markWhy = kReqInRange;
        else if (s_m.worldCalls == 1 &&
                 (s_m.pW0 < s_m.p0 || s_m.pW1 < s_m.pW0 || s_m.pW1 > s_m.pH ||
                  (s_m.haveLevel && (s_m.pLevel < s_m.pW0 || s_m.pLevel > s_m.pW1))))
            markWhy = kOrder;

        // ---- GPU requests the game queued inside [p0, pH) (v0.3.1) ----
        // Azahar v0.3.0: every in-world frame had at least one (`req-in-range`).
        // They are replayed in queue order: a render request (type 1, a
        // split) cuts the replay into pieces at the same command boundary;
        // any other request (0 DMA, 2 fill, 3 transfer, 4 texture copy) is
        // queued again at the same place, so the right eye sees the same
        // sequence (e.g. a depth clear before the hand).
        u32 nReq = 0;
        if (markWhy == kOk)
        {
            const u32 arr = W32(cl + 0x18);
            nReq = s_m.reqH - s_m.reqP0;
            if (nReq && (!InRamPtr(arr) || (arr & 3)))
                markWhy = kReqInRange;
            u32 lastB = s_m.p0;
            for (u32 i = 0; i < nReq && markWhy == kOk; i++)
            {
                std::memcpy(s_req[i], reinterpret_cast<const void *>(arr + (s_m.reqP0 + i) * kReqSize), kReqSize);
                const u8 type = static_cast<u8>(s_req[i][0] & 0xFF);
                if (type == 1)
                {
                    const u32 b = s_req[i][1] + s_req[i][2];        // segment end in the buffer
                    // the split aligns relative to the list base (0x7C5530/48)
                    if (b <= lastB || b > s_m.pH || ((b - base) & 15) || (s_req[i][3] & 4))
                        markWhy = kSegments;
                    lastB = b;
                }
                else if (!FillOfTopTarget(dev, s_req[i]))
                {
                    // delta audit D-S2: only a memory fill of the top colour or
                    // depth buffer is proven safe to repeat (seen in Azahar:
                    // depth 0x1F300000 from 0x39B154). Anything else: 2D, logged.
                    markWhy = kReqInRange;
                    s_st.reqRefused++;
                }
            }
            NoteRequests(nReq);
        }

        s_parser.markP0 = markWhy == kOk ? reinterpret_cast<const u32 *>(s_m.p0) : nullptr;
        s_parser.markPH = markWhy == kOk ? reinterpret_cast<const u32 *>(s_m.pH) : nullptr;
        s_parser.snapP0 = &s_p0;
        s_parser.snapPH = &s_pH;
        s_parser.sites = s_sites;
        s_parser.siteCap = kSiteCap;
        s_parser.siteCount = 0;
        s_parser.finCount = 0;
        s_parser.sawP0 = s_parser.sawPH = s_parser.afterP0 = false;
        s_parser.fail = pica::Fail::None;
        s_parser.wordsParsed = 0;

        // Continue the shadow where the last call stopped (the earlier
        // part of this list was parsed by previous calls only if we are
        // mid-list; normally this is the whole list once per frame).
        const u32 t0 = Tick();
        const bool parsed = s_parser.Run(reinterpret_cast<const u32 *>(base), reinterpret_cast<const u32 *>(cur));
        const u32 dt = Tick() - t0;
        s_st.parseTicks += dt;
        if (dt > s_st.parseMaxTicks)
            s_st.parseMaxTicks = dt;
        if (!parsed || s_parser.jump)
        {
            s_live = false;
            s_tailPending = false;
            Fallback(parsed ? static_cast<u8>(kPicaBase + static_cast<u8>(pica::Fail::Jump)) : static_cast<u8>(kParse));
            return Result::None;
        }
        s_tailPending = true;           // continue from here before RunCmdlist
        s_tailFrom = cur;
        s_tailCl = cl;
        s_warm++;
        if (s_m.worldCalls == 0)
            s_st.menuFrames++;

        if (markWhy != kOk)
        {
            Fallback(markWhy);
            return Result::None;
        }
        if (s_parser.fail != pica::Fail::None)
        {
            Fallback(static_cast<u8>(kPicaBase + static_cast<u8>(s_parser.fail)));
            return Result::None;
        }
        if (!s_parser.sawP0 || !s_parser.sawPH)
        {
            Fallback(kOrder);
            return Result::None;
        }
        // Every finalize in the range must be the last command of a split
        // trailer (segment end - 8 bytes); it is neutralised in the copy.
        u32 nBound = 0;
        for (u32 i = 0; i < nReq; i++)
            if ((s_req[i][0] & 0xFF) == 1)
                nBound++;
        if (s_parser.finCount != nBound)
        {
            Fallback(kSegments);
            return Result::None;
        }
        for (u32 f = 0; f < s_parser.finCount; f++)
        {
            bool ok = false;
            for (u32 i = 0; i < nReq && !ok; i++)
                ok = (s_req[i][0] & 0xFF) == 1 &&
                     reinterpret_cast<u32>(s_parser.finAt[f]) + 8 == s_req[i][1] + s_req[i][2];
            if (!ok)
            {
                Fallback(kSegments);
                return Result::None;
            }
        }
        if (W32(cl + v->clPendA) || W32(cl + v->clPendB))
        {
            Fallback(kPendingJump);
            return Result::None;
        }
        // Request headroom: left copy, fill, one split per piece, the
        // replicated requests, right copy; the game adds up to 2; 2 spare.
        const u32 nPieces = nBound + 1;
        const u32 nRepl = nReq - nBound;
        {
            const u32 used = ReqUsed(cl), max = W32(cl + v->clReqMax);
            if (!max || max > 0x10000 || used > max || max - used < 2 + nPieces + nRepl + 1 + 2 + 2)
            {
                prof::NoteReqSkip();
                Fallback(kHeadroom);
                return Result::None;
            }
        }

        // ---- right/left matrices for every upload in the range ----
        std::memset(s_force, 0, sizeof(s_force));
        const u32 nSites = s_parser.siteCount;
        for (u32 i = 0; i < nSites; i++)
        {
            const pica::Site &s = s_sites[i];
            float K, KC;
            s_patch[i] = false;
            if (!Classify(s, p, K, KC) || (K == 0.0f && KC == 0.0f) || !std::isfinite(K) || !std::isfinite(KC))
                continue;
            pica::ShearRow0(s.raw[0], s.raw[3], -0.5f * K, -0.5f * KC, s_leftW[i]);
            pica::ShearRow0(s.raw[0], s.raw[3], 0.5f * K, 0.5f * KC, s_rightW[i]);
            s_patch[i] = true;
            s_force[pica::kVsfBase + s.base] = 1;      // row 0 register (c0 or c8) differs after the copy
        }

        // ---- dry run: sizes + "can the transition be expressed" ----
        const u32 te = Tick();
        pica::Fail f = EmitScratch();
        s_st.emitTicks += Tick() - te;
        if (f != pica::Fail::None)
        {
            Fallback(s_warm < 3 && f == pica::Fail::UnknownAtP0 ? static_cast<u8>(kWarmup)
                                                                 : static_cast<u8>(kPicaBase + static_cast<u8>(f)));
            return Result::None;
        }
        const u32 proWords = s_proN;
        const u32 epiWords = s_epiN;
        const u32 copyBytes = s_m.pH - s_m.p0;
        constexpr u32 kReserve = 512;   // final split trailer + present's pad/final split + margin
        const u32 needBytes = (proWords + epiWords) * 4 + copyBytes + nPieces * 32 + kReserve;
        if (limit - cur < needBytes)
        {
            Fallback(kNoRoom);
            return Result::None;
        }

        // ---------------- commit ----------------
        const auto b2l = reinterpret_cast<FnB2L>(v->fnCopyB2L);
        const u32 res = W32(dev + v->devTopColorRes);
        const u32 w = W32(dev + v->devTopWidth), h = W32(dev + v->devTopHeight);
        const u32 tb = Tick();
        // 1. left eye: the finished picture (game's own B2L arguments)
        b2l(res, 0, 0, dstL + kPadRows * kRowBytes, w, h, 1, 0, 0);
        // 2. clear the top target exactly like the stage-B clear did
        u8 rgba[4];
        std::memcpy(rgba, s_m.rgba, 4);
        s_ourClear = true;
        reinterpret_cast<FnClear>(v->fnGxClear)(dev, rgba, 0);
        s_ourClear = false;
        // 3. our words go at the (normally unchanged) cursor
        u32 cur2 = Cursor();
        if (cur2 != cur)
        {
            // audit S6: the split pads on bit 3 only, so the cursor must stay
            // 8-byte aligned. audit S5: a failed parse leaves the shadow
            // half-updated: drop it (restart next frame).
            const bool sane = cur2 > cur && cur2 <= limit && !(cur2 & 7);
            const bool parsedNew = sane &&
                s_parser.Run(reinterpret_cast<const u32 *>(cur), reinterpret_cast<const u32 *>(cur2)) &&
                !s_parser.jump;
            if (!parsedNew)
            {
                s_live = false;
                s_tailPending = false;
                Fallback(kCursorMoved);
                return Result::LeftOnly;    // caller: left copy exists, shows it 2D
            }
            if (limit - cur2 < needBytes + 256)
            {
                s_tailFrom = cur2;          // shadow is consistent up to cur2
                Fallback(kCursorMoved);
                return Result::LeftOnly;
            }
            // delta audit D-S3: the new words changed the end state: the
            // transitions must be re-proven (B1 on this path).
            s_tailFrom = cur2;
            if (s_parser.fail != pica::Fail::None || EmitScratch() != pica::Fail::None ||
                limit - cur2 < ((s_proN + s_epiN) * 4 + copyBytes + nPieces * 32 + kReserve + 256))
            {
                Fallback(kCursorMoved);
                return Result::LeftOnly;
            }
        }
        // D-B1: plan the pieces (the game's finalize is never copied).
        u32 bounds[kMaxReq];
        u32 nb = 0;
        for (u32 i = 0; i < nReq; i++)
            if ((s_req[i][0] & 0xFF) == 1)
                bounds[nb++] = s_req[i][1] + s_req[i][2];
        if (pica::PlanPieces(s_m.p0, s_m.pH, bounds, nb, s_pieces) != nb + 1)
        {
            Fallback(kSegments);
            return Result::LeftOnly;
        }
        if (W32(v->nngxState + 0xA0) == cl)
        {
            // N2: the bound list must not be the one the GPU is running
            Fallback(kPendingJump);
            return Result::LeftOnly;
        }
        // (the prologue/epilogue in scratch are proven for the final end state)
        // From here on everything is written: once the prologue runs on
        // the GPU the epilogue must follow, whatever happens.
        bool splitsOk = true;
        u32 req = 0, site = 0, pi = 0;
        const u32 arr = W32(cl + 0x18);
        u32 *at = reinterpret_cast<u32 *>(cur2);
        bool first = true;
        for (;;)
        {
            // replicate the non-render requests that come next in the queue
            while (req < nReq && (s_req[req][0] & 0xFF) != 1)
            {
                const u32 used = ReqUsed(cl);
                std::memcpy(reinterpret_cast<void *>(arr + used * kReqSize), s_req[req], kReqSize);
                W32(cl + v->clReqUsed) = used + 1;
                req++;
            }
            const bool last = req >= nReq;
            const pica::Piece &pc = s_pieces[pi];
            pica::Emitter e;
            e.p = at;
            e.end = reinterpret_cast<u32 *>(limit - kReserve);
            if (first && e.Room(s_proN))
            {
                std::memcpy(e.p, s_proBuf, s_proN * 4);             // prologue (proven above)
                e.p += s_proN;
            }
            first = false;
            u32 *copy = e.p;
            const u32 bytes = pc.copyEnd - pc.start;
            if (e.Room(bytes / 4))
            {
                std::memcpy(copy, reinterpret_cast<const void *>(pc.start), bytes);
                e.p += bytes / 4;
                // right-eye matrices inside this piece (every word checked: N1b)
                for (; site < nSites && reinterpret_cast<u32>(s_sites[site].row0[0]) < pc.end; site++)
                    if (s_patch[site])
                        for (u32 j = 0; j < 4; j++)
                        {
                            const u32 a = reinterpret_cast<u32>(s_sites[site].row0[j]);
                            if (a >= pc.start && a + 4 <= pc.copyEnd)
                                copy[(a - pc.start) / 4] = s_rightW[site][j];
                            else
                                splitsOk = false;       // cannot happen (bursts never span a split): show 2D
                        }
            }
            if (last && e.Room(s_epiN))
            {
                std::memcpy(e.p, s_epiBuf, s_epiN * 4);             // epilogue
                e.p += s_epiN;
            }
            const u32 end = reinterpret_cast<u32>(e.p);
            W32(v->cmdCursor) = end;
            svcFlushProcessDataCache(CUR_PROCESS_HANDLE, reinterpret_cast<u32>(at), end - reinterpret_cast<u32>(at));
            const u32 before = ReqUsed(cl);
            reinterpret_cast<FnSplit>(v->fnSplit)();
            if (ReqUsed(cl) != before + 1 || e.overflow)
                splitsOk = false;
            at = reinterpret_cast<u32 *>(Cursor());
            if (last)
                break;
            pi++;
            req++;                                                  // past this render request
        }
        // 4. left-eye matrices in the original range (the GPU has not read it yet)
        bool anyPatch = false;
        for (u32 i = 0; i < nSites; i++)
            if (s_patch[i])
            {
                for (u32 j = 0; j < 4; j++)
                    *s_sites[i].row0[j] = s_leftW[i][j];
                anyPatch = true;
            }
        if (anyPatch)
            svcFlushProcessDataCache(CUR_PROCESS_HANDLE, s_m.p0, copyBytes);
        s_tailFrom = Cursor();          // after our last split trailer
        if (!splitsOk)
        {
            // D-S3: if anything went wrong while writing, the GPU state after
            // our words is not proven: restart the shadow next frame.
            s_live = false;
            s_tailPending = false;
            Fallback(kSplitFailed);
            return Result::LeftOnly;
        }
        // 5. right eye
        b2l(res, 0, 0, dstR + kPadRows * kRowBytes, w, h, 1, 0, 0);
        s_st.buildTicks += Tick() - tb;

        s_st.replayed++;
        s_tot.replayed++;
        s_replayedTotal++;
        s_st.copyBytes += copyBytes;
        s_st.proWords += proWords;
        s_st.epiWords += epiWords;
        if (nPieces > 1)
            s_st.segmented++;
        s_lastReason = kOk;
        return Result::Both;
    }

    void EndFrame()
    {
        s_m = Marks{};
        for (int k = 0; k < 3; k++)
            s_callerN[k] = 0;
        s_projN = 0;
    }

    // ---------------------------------------------------------------------
    void SetPacer(bool on) { s_paceWanted = on; }
    bool GovernorAllows() { return s_govAllows; }
    void SetGovernor(bool on) { s_govEnabled = on; if (!on) s_govAllows = true; }
    bool PacerOn() { return s_paceWanted && !s_paceSuspended; }

    void BeforeRunCmdlist()
    {
        const game::GameVersion *v = V();
        if (!v || !v->nngxState)
            return;
        // 1. tail of this list (after our hook: split trailer, bottom B2L,
        //    present's padding) so the shadow ends exactly where the GPU will.
        if (s_tailPending)
        {
            s_tailPending = false;
            const u32 cl = BoundCl();
            const u32 cur = v->cmdCursor ? Cursor() : 0;
            s_parser.markP0 = s_parser.markPH = nullptr;
            s_parser.sites = nullptr;
            s_parser.siteCap = 0;
            if (cl != s_tailCl || cur < s_tailFrom ||
                !s_parser.Run(reinterpret_cast<const u32 *>(s_tailFrom), reinterpret_cast<const u32 *>(cur)) ||
                s_parser.jump)
                s_live = false;
        }
        else
            s_live = false;

        // 2. 30 fps pacer: list N starts at least 2 vblanks after list N-1.
        if (!v->offVblankTop)
            return;
        const u32 now = NowMs();
        if (s_paceSuspended && static_cast<s32>(now - s_paceOffUntil) >= 0)
            s_paceSuspended = false;
        if (!s_paceWanted || s_paceSuspended)
        {
            s_haveLastRun = false;
            // let a cool-down run out while Comfort is off
            s_govAllows = !s_govEnabled || pica::GovernorStep(s_gov, false, false, NowMs());
            if (!s_govAllows)
            {
                s_st.govCoolFrames++;
                s_tot.govCoolFrames++;
            }
            return;
        }
        u32 vc = VCount();
        if (!s_haveLastRun)
        {
            s_haveLastRun = true;
            s_lastRunV = vc;
            return;
        }
        s32 d = static_cast<s32>(vc - s_lastRunV);
        if (d < 0 || d > 120)
        {
            s_lastRunV = vc;            // wrap (0x7FFFFFFF) or a long pause: resync
            return;
        }
        const u32 t0 = Tick();
        bool timeout = false;
        while (d < 2)
        {
            if (Tick() - t0 > 40 * kTicksMs)
            {
                timeout = true;
                break;
            }
            svcSleepThread(500000);     // 0.5 ms; no GSP events consumed (we only read the counter)
            vc = VCount();
            d = static_cast<s32>(vc - s_lastRunV);
        }
        const u32 waited = Tick() - t0;
        s_st.paceRuns++;
        s_tot.paceRuns++;
        s_st.paceWaitTicks += waited;
        if (waited > s_st.paceMaxWaitTicks)
            s_st.paceMaxWaitTicks = waited;
        if (d > 2)
        {
            s_st.paceLate++;
            s_tot.paceLate++;
        }
        if (s_govEnabled)
        {
            const u32 trips = s_gov.trips;
            s_govAllows = pica::GovernorStep(s_gov, true, d > 2, NowMs());
            if (s_gov.trips != trips)
            {
                s_st.govTrips++;
                s_tot.govTrips++;
            }
        }
        if (timeout)
        {
            s_st.paceTimeout++;
            // Self-check: the counter is not moving. Three in a row: pacer
            // off for 10 s (logged as timeouts), the game runs unpaced.
            if (++s_paceTimeouts >= 3)
            {
                s_paceSuspended = true;
                s_paceOffUntil = NowMs() + 10000;
                s_paceTimeouts = 0;
            }
        }
        else
            s_paceTimeouts = 0;
        s_lastRunV = vc;
    }

    // ---------------------------------------------------------------------
    // GSP cmdlist-done thread: the addresses about to be shown.
    void OnSwap(u32 left, u32 right)
    {
        const game::GameVersion *v = V();
        if (!v || !v->offVblankTop)
            return;
        const u32 vc = VCount();
        bool changed[2] = { false, false };
        const u32 addr[2] = { left, right };
        for (int e = 0; e < 2; e++)
        {
            if (!addr[e])
            {
                s_haveV[e] = false;
                continue;
            }
            if (addr[e] == s_lastAddr[e] && s_haveV[e])
                continue;
            changed[e] = true;
            if (s_haveV[e])
            {
                const u32 iv = vc - s_lastV[e];
                if (iv < 120)
                {
                    Add(s_hist[e][iv <= 1 ? 0 : iv == 2 ? 1 : iv == 3 ? 2 : 3], 1);
                    Add(s_ivSum[e], iv);
                    Add(s_ivSq[e], iv * iv);
                }
            }
            s_lastV[e] = vc;
            s_lastAddr[e] = addr[e];
            s_haveV[e] = true;
        }
        if (left && right && changed[0] != changed[1])
            Add(s_split, 1);
    }

    void NoteListKind(u32 list, bool replay) { s_listKind[list & 1] = replay ? 1 : 0; }

    void OnListGpu(u32 list, u32 ticks)
    {
        if (s_listKind[list & 1])
        {
            Add(s_gpuCN, 1);
            Add(s_gpuCT, ticks);
        }
        else
        {
            Add(s_gpuMN, 1);
            Add(s_gpuMT, ticks);
        }
    }

    void TakeStats(ComfortStats &out)
    {
        out = s_st;
        s_st = ComfortStats{};
        for (int e = 0; e < 2; e++)
        {
            for (int b = 0; b < 4; b++)
                out.hist[e][b] = Take(s_hist[e][b]);
            out.ivSum[e] = Take(s_ivSum[e]);
            out.ivSq[e] = Take(s_ivSq[e]);
        }
        out.eyeSplit = Take(s_split);
        out.gpuCN = Take(s_gpuCN);
        out.gpuCTicks = Take(s_gpuCT);
        out.gpuMN = Take(s_gpuMN);
        out.gpuMTicks = Take(s_gpuMT);
        const game::GameVersion *v = V();
        const u32 vc = (v && v->offVblankTop) ? VCount() : 0;
        out.vblanks = vc - s_vbAtWindow;
        s_vbAtWindow = vc;
        out.paceOn = PacerOn() ? 1 : 0;
    }

    u32 LastReason() { return s_lastReason; }
    u32 Replayed() { return s_replayedTotal; }

    // v0.3.1: what a failed Comfort frame showed (stereo_flat.cpp).
    void NoteShown(bool held)
    {
        if (held)
        {
            s_st.held++;
            s_tot.held++;
        }
        else
        {
            s_st.monoFallback++;
            s_tot.mono++;
        }
    }
    bool GovernorEnabled() { return s_govEnabled; }
    void ReadTotals(ComfortTotals &out) { std::memcpy(&out, &s_tot, sizeof(out)); }
}

extern "C" void mc3d_level_render_returned(void) { mc3d::comfort::OnLevelRenderReturn(); }
