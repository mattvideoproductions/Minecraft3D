// v0.3 world options: 3D render-distance clamp, render distance setting,
// fast leaves, far fog override, memory guard. See world_opts.hpp.
#include "world_opts.hpp"
#include "game_versions.hpp"
#include "version_check.hpp"
#include "hooks.hpp"
#include "config.hpp"
#include "profiler.hpp"

#include <3ds.h>
#include <CTRPluginFramework.hpp>
#include <cmath>
#include <cstring>

using namespace CTRPluginFramework;

// Shared with the asm callback mc3d_rd_clamp_cb (stereo_stubs.s); main thread only.
extern "C"
{
    struct Mc3dRd { s32 clamp; u32 lastCtx; u32 lr; u32 ctx; u32 frames; };
    Mc3dRd mc3d_rd = { 0, 0, 0, 0, 0 };
    void mc3d_rd_clamp_cb(void);
}

// MITM pass-through signature (hooks.hpp)
#define MC3D_W_HOOK_ARGS u32 a0, u32 a1, u32 a2, u32 a3, double f0, double f1, double f2, double f3

namespace mc3d::world
{
    namespace
    {
        const game::GameVersion *V() { return version::Current(); }
        inline volatile u32 &W32(u32 a) { return *reinterpret_cast<volatile u32 *>(a); }
        inline u32 NowMs() { return static_cast<u32>(svcGetSystemTick() / (prof::kTicksPerSec / 1000)); }

        hook::InstrHook h_clamp;
        hook::FunctionHook h_save;

        // choices (plugin thread writes, main thread reads)
        volatile int    s_rdChoice = 0;         // 0 = game setting
        volatile bool   s_autoLower = true;
        volatile int    s_d3Choice = 0;         // 0 = automatic rule
        volatile Leaves s_leaves = Leaves::Auto;
        volatile int    s_far = 0;
        volatile bool   s_heapWanted = false;

        // main-thread state
        u32  s_lastFrames = 0;
        bool s_inWorld = false;
        volatile u32 s_optPtr = 0;          // Options* last validated (for the save hook)
        volatile int s_leavesWritten = -1;  // value we last wrote to +0x6F (-1 none)
        volatile int s_rdWritten = -1;      // value we last set through Options::set (-1 none)
        volatile u32 s_saveSubst = 0;       // saves where a forced value was replaced (log)
        int  s_guardSlot = -1;              // cfg guardheap=<slot>: guard only this heap
        Hyst s_rdHyst = {};
        Hyst s_leafHyst = {};
        bool s_leafOnSeen = false;
        u32  s_leafOnSince = 0;
        bool s_leavesOrigKnown = false;
        u8   s_leavesOrig = 1;
        int  s_leavesLast = -1;             // value we last applied (-1 = none yet)
        u32  s_leavesTryMs = 0;
        int  s_rdApplied = 0;
        bool s_rdOrigKnown = false;
        int  s_rdOrig = 0;
        u32  s_guardMs = 0;
        u32  s_heapMs = 0;
        float s_farWritten = 0.0f;
        bool s_farMismatchNoted = false;
        int  s_front = -1, s_back = -1;
        bool s_levelsRead = false;

        WorldStats s_st = {};
        HeapStat   s_heap[kHeapSlots] = {};
        u8         s_heapCount = 0, s_heapWalkOk = 0;
        bool       s_guardOk = false;

        // v0.3.2: what the game really uses, published for the menu text
        // (plugin thread) and the event log. Main thread writes.
        volatile int  s_pubNormal = -1;     // the game's distance (chunks) the renderer asked for
        volatile int  s_pubDrawn = -1;      // after our 3D clamp
        volatile bool s_pubInWorld = false;
        volatile int  s_refusedVal = 0;     // last refused choice (0 none); a new choice clears it
        volatile u8   s_refusedWhy = 0;     // prof::kWhyRdLowMem / kWhyRdNotAllowed
        int  s_evNormal = -1, s_evDrawn = -1;   // last values logged
        u8   s_setWhy = 0;                  // why our last Options::set happened
        u32  s_setMs = 0;

        // notices for the plugin thread
        const char *volatile s_notice = nullptr;
        void Notice(const char *m) { s_notice = m; }

        bool InRam(u32 a) { return a >= 0x08000000 && a < 0x40000000 && !(a & 3); }
        bool Mapped(u32 a, u32 len, bool write)
        {
            const u32 perm = MEMPERM_READ | (write ? MEMPERM_WRITE : 0);
            return InRam(a) && Process::CheckAddress(a, perm) && Process::CheckAddress(a + len - 1, perm);
        }

        // LevelRenderer (from the clamp callback) -> client -> Options.
        // Only valid in a frame in which startFrame ran (audit S1: after
        // leaving a world the LevelRenderer is freed; mc3d_rd.lr is cleared
        // the first frame without startFrame).
        bool Pointers(u32 &lr, u32 &client, u32 &options)
        {
            const game::GameVersion *v = V();
            lr = client = options = 0;
            if (!v || !v->offLrClient || !v->offClientOptions || !s_inWorld || !mc3d_rd.lr)
                return false;
            lr = mc3d_rd.lr;
            if (!Mapped(lr, v->offLrClient + 4, true))
                return false;
            client = W32(lr + v->offLrClient);
            if (!Mapped(client, v->offClientOptions + 4, false))
                return false;
            options = W32(client + v->offClientOptions);
            if (!Mapped(options, 0x200, true))
                return false;
            s_optPtr = options;
            return true;
        }

        void ReadLevels()
        {
            const game::GameVersion *v = V();
            if (s_levelsRead || !v || !v->rdLevels)
                return;
            const u32 b = W32(v->rdLevels), e = W32(v->rdLevels + 4);
            if (!InRam(b) || e <= b || e - b > 64 || ((e - b) & 3) || !Mapped(b, e - b, false))
                return;     // not built yet (static initialiser) or unexpected: try later
            int prev = 0;
            for (u32 a = b; a < e; a += 4)
            {
                const int x = static_cast<int>(W32(a));
                if (x < 1 || x > 32 || x <= prev)
                    return;
                prev = x;
            }
            s_front = static_cast<int>(W32(b));
            s_back = prev;
            s_levelsRead = true;
        }

        // ---- memory guard: nn ExpHeap free lists (read only, bounded) ----
        bool WalkHeaps()
        {
            const game::GameVersion *v = V();
            s_heapCount = 0;
            s_heapWalkOk = 0;
            if (!v || !v->heapTable || !v->heapVtable)
                return false;
            bool allOk = true;
            u32 rs[kHeapSlots] = {}, re[kHeapSlots] = {};
            for (u32 i = 0; i < 10 && s_heapCount < kHeapSlots; i++)
            {
                const u32 obj = W32(v->heapTable + 4 * i);
                if (!InRam(obj) || !Mapped(obj, 0x60, false) || W32(obj) != v->heapVtable)
                    continue;
                const u32 heap = obj + 0x18;
                if (W32(heap) != 0x45585048u)               // 'EXPH'
                    continue;
                const u32 start = W32(heap + 0x18), end = W32(heap + 0x1C);
                rs[s_heapCount] = start;
                re[s_heapCount] = end;
                HeapStat &hs = s_heap[s_heapCount++];
                hs = HeapStat{};
                hs.slot = static_cast<u8>(i);
                if (!InRam(start) || end <= start || end - start > 0x8000000u ||
                    !Mapped(start, 4, false) || !Mapped(end - 4, 4, false))
                {
                    allOk = false;
                    continue;
                }
                hs.capKB = (end - start) >> 10;
                u32 blk = W32(heap + 0x24), total = 0, largest = 0, n = 0;
                bool ok = true;
                while (blk)
                {
                    // Every step is checked: inside the heap, aligned, a
                    // free-block signature 'FR', a size that fits. Another
                    // thread may be changing the list: any doubt = unknown.
                    if (blk < start || blk + 0x10 > end || (blk & 3) || (W32(blk) & 0xFFFF) != 0x4652 ||
                        ++n > 4096)
                    {
                        ok = false;
                        break;
                    }
                    const u32 sz = W32(blk + 4);
                    if (sz > end - blk)
                    {
                        ok = false;
                        break;
                    }
                    total += sz;
                    if (sz > largest)
                        largest = sz;
                    blk = W32(blk + 0xC);
                }
                hs.freeKB = total >> 10;
                hs.largestKB = largest >> 10;
                hs.ok = ok ? 1 : 0;
                if (!ok)
                    allOk = false;
            }
            // A heap whose range contains another listed heap is a parent
            // (boot code 0x1055CC carves the 52 MiB heap out of the main
            // one): its own free list cannot describe the memory chunks use.
            for (u32 a = 0; a < s_heapCount; a++)
                for (u32 b = 0; b < s_heapCount; b++)
                    if (a != b && rs[b] >= rs[a] && re[b] <= re[a] && re[a] > rs[a])
                        s_heap[a].parent = 1;
            s_heapWalkOk = (allOk && s_heapCount) ? 1 : 0;
            return s_heapWalkOk != 0;
        }

        // Design 3.2 thresholds [L]: largest free block >= 3 MiB and total
        // free >= 6 MiB. Audit S7: which heap holds chunk data is not known
        // yet (the log lists every heap by slot to find out), so the guard
        // applies to the LEAF heaps of 8 MiB or more (not a parent whose
        // free list excludes its carved-out child), or to the one slot set
        // with guardheap=<slot> in mc3d_cfg.txt. Fails closed.
        bool GuardOk()
        {
            if (!WalkHeaps())
                return s_guardOk = false;
            bool any = false, ok = true;
            for (u32 i = 0; i < s_heapCount; i++)
            {
                HeapStat &h = s_heap[i];
                const bool use = s_guardSlot >= 0 ? h.slot == s_guardSlot : (!h.parent && h.capKB >= 8 * 1024);
                if (!use)
                    continue;
                h.guarded = 1;
                any = true;
                if (h.largestKB < 3 * 1024 || h.freeKB < 6 * 1024)
                    ok = false;
            }
            return s_guardOk = any && ok;
        }

        bool SetOptionRd(int chunks)
        {
            const game::GameVersion *v = V();
            u32 lr, client, options;
            if (!v || !v->fnOptionsSetInt || !v->optRenderDistance || !Pointers(lr, client, options))
                return false;
            const int cur = static_cast<int>(W32(options + v->offOptRenderDist));
            if (cur < 1 || cur > 32)
                return false;           // not the field we expect: do nothing
            // Audit S2: the player's own value is whatever is there when it
            // is not the value we wrote (re-read, never stale).
            if (!s_rdOrigKnown || cur != s_rdWritten)
            {
                s_rdOrig = cur;
                s_rdOrigKnown = true;
            }
            if (cur != chunks)
                reinterpret_cast<void (*)(u32, u32, int)>(v->fnOptionsSetInt)(options, v->optRenderDistance, chunks);
            s_rdWritten = chunks;
            s_setMs = NowMs();
            s_st.rdChanges++;
            return true;
        }

        constexpr u8 kLeavesOriginal = 2;   // "whatever the game had" (slider 0 = stock)

        bool SetLeavesValue(u8 val)
        {
            const game::GameVersion *v = V();
            u32 lr, client, options;
            if (!v || !v->fnGraphicsChanged || !v->offOptLeaves || !v->offLrRadius || !Pointers(lr, client, options))
                return false;
            volatile u8 *p = reinterpret_cast<volatile u8 *>(options + v->offOptLeaves);
            const u8 cur = *p;
            if (cur > 1)
                return false;           // unexpected content: leave it alone
            // Audit S2: re-read the player's value whenever +0x6F holds
            // something we did not write (e.g. changed in the game menu).
            if (!s_leavesOrigKnown || static_cast<int>(cur) != s_leavesWritten)
            {
                s_leavesOrig = cur;
                s_leavesOrigKnown = true;
            }
            if (val == kLeavesOriginal)
                val = s_leavesOrig;     // the player's own setting, as found
            if (cur == val)
                return true;
            const s32 radius = static_cast<s32>(W32(lr + v->offLrRadius));
            if (radius < 8 || radius > 1024)
                return false;           // renderer not in its normal state: try later
            *p = val;
            s_leavesWritten = val;
            reinterpret_cast<void (*)(u32)>(v->fnGraphicsChanged)(client);
            // Existing meshes keep their old leaves: -1 makes the next
            // startFrame take 0x3C7164(this, 1) = full reset + rebuild.
            W32(lr + v->offLrRadius) = 0xFFFFFFFFu;
            s_st.rebuilds++;
            return true;
        }
    }

    // ---------------------------------------------------------------------
    int Auto3DChunks(int normal, int choice)
    {
        if (normal <= 0)
            return 0;
        int t = choice > 0 ? choice : (normal >= 5 ? 4 : (normal < 3 ? normal : 3));
        if (t > normal)
            t = normal;
        if (t < 2)
            t = normal < 2 ? normal : 2;
        return t;
    }

    bool HystStep(Hyst &h, bool want, u32 now, u32 offTicks, u32 gapTicks)
    {
        if (want)
        {
            h.haveOff = false;
            h.applied = true;               // shrink / apply at once
            return true;
        }
        if (!h.applied)
            return false;
        if (!h.haveOff)
        {
            h.haveOff = true;
            h.offSince = now;
        }
        if (now - h.offSince >= offTicks && (!h.haveGrow || now - h.lastGrow >= gapTicks))
        {
            h.applied = false;
            h.haveGrow = true;
            h.lastGrow = now;
            h.haveOff = false;
        }
        return h.applied;
    }

    // Audit S2: Options::save (0x634774) - while the plugin has forced fast
    // leaves or an experimental render distance, write the PLAYER's values
    // to options.txt, then put ours back. Runs on the thread that saves
    // (the game main thread in practice); reads/writes two Options fields.
    namespace
    {
        u64 Hook_OptionsSave(MC3D_W_HOOK_ARGS)
        {
            const game::GameVersion *v = V();
            const u32 opt = s_optPtr;
            u8 *leaves = nullptr;
            u8 leavesKeep = 0;
            u32 *rd = nullptr;
            u32 rdKeep = 0;
            if (v && opt && a0 == opt)
            {
                leaves = reinterpret_cast<u8 *>(opt + v->offOptLeaves);
                if (s_leavesWritten >= 0 && s_leavesOrigKnown && *leaves == static_cast<u8>(s_leavesWritten) &&
                    *leaves != s_leavesOrig)
                {
                    leavesKeep = *leaves;
                    *leaves = s_leavesOrig;
                    s_saveSubst = s_saveSubst + 1;
                }
                else
                    leaves = nullptr;
                rd = reinterpret_cast<u32 *>(opt + v->offOptRenderDist);
                if (s_rdWritten > 0 && s_rdOrigKnown && static_cast<int>(*rd) == s_rdWritten &&
                    IsExperimental(s_rdWritten) && s_rdOrig != s_rdWritten)
                {
                    rdKeep = *rd;
                    *rd = static_cast<u32>(s_rdOrig);
                    s_saveSubst = s_saveSubst + 1;
                }
                else
                    rd = nullptr;
            }
            const u64 r = hook::FunctionHook::CallOriginal(a0, a1, a2, a3, f0, f1, f2, f3);
            if (leaves)
                *leaves = leavesKeep;
            if (rd)
                *rd = rdKeep;
            return r;
        }
    }

    void Install()
    {
        const game::GameVersion *v = V();
        if (!v || !v->siteRdClamp || !cfg::Hook(cfg::kWorldOpts))
            return;
        h_clamp.Install(v->siteRdClamp, v->insnRdClamp, mc3d_rd_clamp_cb);
        if (v->fnOptionsSave)
            h_save.Install(v->fnOptionsSave, Hook_OptionsSave, hook::ExpectedFirstWord(v->fnOptionsSave));
        int g;
        if ((g = cfg::StereoOpt("guardheap")) >= 0 && g < 10) s_guardSlot = g;
        int o;
        if ((o = cfg::StereoOpt("rd")) >= 0) s_rdChoice = o;
        if ((o = cfg::StereoOpt("rdauto")) >= 0) s_autoLower = o != 0;
        if ((o = cfg::StereoOpt("rd3d")) >= 0) s_d3Choice = o;
        if ((o = cfg::StereoOpt("leaves")) >= 0) s_leaves = o == 0 ? Leaves::Fancy : o == 1 ? Leaves::Fast : Leaves::Auto;
        if ((o = cfg::StereoOpt("farfog")) >= 0) s_far = o;
    }

    bool ClampInstalled() { return h_clamp.Installed(); }

    void SetRenderDistance(int c)
    {
        s_rdChoice = c < 0 ? 0 : c;
        s_refusedVal = 0;               // a new choice: forget the last refusal
    }
    int  RenderDistance() { return s_rdChoice; }
    void SetAutoLower(bool on) { s_autoLower = on; }
    bool AutoLower() { return s_autoLower; }
    void SetDistance3D(int c) { s_d3Choice = c < 0 ? 0 : c; }
    int  Distance3D() { return s_d3Choice; }
    void SetLeaves(Leaves m) { s_leaves = m; }
    Leaves GetLeaves() { return s_leaves; }
    void SetFarFog(int b) { s_far = b < 0 ? 0 : b; }
    int  FarFog() { return s_far; }
    int  LevelsFront() { return s_front; }
    int  LevelsBack() { return s_back; }
    bool IsExperimental(int c) { return s_back > 0 ? c > s_back : true; }
    void SetHeapStatsWanted(bool on) { s_heapWanted = on; }
    const char *TakeNotice()
    {
        const char *m = s_notice;
        s_notice = nullptr;
        return m;
    }

    // ---------------------------------------------------------------------
    void Update(bool comfortActive, bool comfortSelected)
    {
        const game::GameVersion *v = V();
        if (!v)
            return;
        const u32 now = NowMs();
        ReadLevels();
        // In a world = startFrame ran in THIS frame (audit S1). Otherwise
        // the LevelRenderer may already be freed: forget it.
        if (h_clamp.Installed() && mc3d_rd.frames != s_lastFrames)
        {
            s_lastFrames = mc3d_rd.frames;
            s_inWorld = true;
        }
        else
        {
            s_inWorld = false;
            mc3d_rd.lr = 0;
        }

        // ---- 1. 3D render distance (render-only clamp) ----
        if (h_clamp.Installed())
        {
            // Comfort 3D only (audit S3): Flat / alternate eyes need no GPU savings.
            const bool applied = HystStep(s_rdHyst, comfortActive && s_autoLower, now, 5000, 20000);
            const s32 ctx = static_cast<s32>(mc3d_rd.lastCtx);
            const int normal = (ctx >= 16 && ctx <= 32 * 16) ? ctx / 16 : 0;
            const int t = applied ? Auto3DChunks(normal, s_d3Choice) : 0;
            const s32 clamp = (t > 0 && t < normal) ? t * 16 : 0;
            const bool clampChanged = clamp != mc3d_rd.clamp;
            if (clampChanged)
            {
                mc3d_rd.clamp = clamp;
                s_st.rdChanges++;
            }
            // v0.3.2: publish what is really used, and log every change.
            // A change of the game's distance comes from our setting if we
            // called Options::set in the last 5 s, else from the game.
            if (s_inWorld && normal > 0)
            {
                const int drawn = clamp > 0 ? clamp / 16 : normal;
                if (s_evNormal > 0 && normal != s_evNormal)
                    prof::PushEvent(prof::EvKind::Distance, static_cast<u8>(s_evNormal), static_cast<u8>(normal),
                                    s_setWhy && now - s_setMs < 5000 ? s_setWhy : static_cast<u8>(prof::kWhyRdGame));
                if (clampChanged && s_evDrawn > 0 && drawn != s_evDrawn)
                    prof::PushEvent(prof::EvKind::Distance, static_cast<u8>(s_evDrawn), static_cast<u8>(drawn),
                                    clamp > 0 ? prof::kWhyRd3DAuto : prof::kWhyRdRestore);
                s_evNormal = normal;
                s_evDrawn = drawn;
                s_pubNormal = normal;
                s_pubDrawn = drawn;
            }
            s_pubInWorld = s_inWorld;
        }

        // ---- 2. fast leaves (each change = full chunk rebuild) ----
        if (s_inWorld)
        {
            bool fast;
            if (s_leaves == Leaves::Auto)
            {
                // Audit S3 / design 3.4: a rebuild is never tied to the slider.
                // Auto = fast while Comfort 3D is SELECTED (mode + "3D on"),
                // whatever the slider; applied 1 s after selecting or on
                // entering a world, reverted 20 s after deselecting.
                if (comfortSelected && !s_leafOnSeen)
                {
                    s_leafOnSeen = true;
                    s_leafOnSince = now;
                }
                if (!comfortSelected)
                    s_leafOnSeen = false;
                const bool want = comfortSelected && now - s_leafOnSince >= 1000;
                fast = HystStep(s_leafHyst, want, now, 20000, 20000);
            }
            else
                fast = s_leaves == Leaves::Fast;
            const u8 val = fast ? 0 : (s_leaves == Leaves::Fancy ? 1 : kLeavesOriginal);
            // Only on a change of what we want (the pointer checks are
            // syscalls); a failed attempt is retried once per second.
            if (static_cast<int>(val) != s_leavesLast && (s_leavesLast == -1 || now - s_leavesTryMs >= 1000))
            {
                s_leavesTryMs = now;
                s_leavesLast = SetLeavesValue(val) ? static_cast<int>(val) : -2;
            }
        }

        // ---- 3. far fog override (experimental) ----
        if (v->farOverride)
        {
            volatile float *f = reinterpret_cast<volatile float *>(v->farOverride);
            const float cur = *f;
            const int far = s_far;
            if (far > 0)
            {
                if (cur == -1.0f || cur == s_farWritten)
                {
                    *f = static_cast<float>(far);
                    s_farWritten = static_cast<float>(far);
                }
                else if (!s_farMismatchNoted)
                {
                    s_farMismatchNoted = true;
                    Notice("Far fog: game uses the value, not changed");
                }
            }
            else if (s_farWritten != 0.0f)
            {
                if (cur == s_farWritten)
                    *f = -1.0f;
                s_farWritten = 0.0f;
            }
        }

        // ---- 4. render distance setting (game Options) ----
        const int choice = s_rdChoice;
        if (s_inWorld && choice != s_rdApplied)
        {
            if (choice == 0)
            {
                // Back to the game's setting: restore the player's value
                // only if the field still holds what we wrote (audit S2).
                u32 lr, client, options;
                const bool ours = Pointers(lr, client, options) &&
                                  static_cast<int>(W32(options + v->offOptRenderDist)) == s_rdWritten;
                s_setWhy = prof::kWhyRdSetting;
                if (!s_rdOrigKnown || !ours || SetOptionRd(s_rdOrig))
                {
                    s_rdApplied = 0;
                    s_rdWritten = -1;
                }
            }
            else if (!s_levelsRead || choice < s_front || choice > s_back + 2 || choice < 2)
            {
                s_rdChoice = s_rdApplied;
                s_refusedVal = choice;
                s_refusedWhy = prof::kWhyRdNotAllowed;
                prof::PushEvent(prof::EvKind::DistanceRefused, static_cast<u8>(choice),
                                static_cast<u8>(s_evNormal > 0 ? s_evNormal : 0), prof::kWhyRdNotAllowed);
                Notice("Render distance: value not allowed");
            }
            else if (IsExperimental(choice) && !GuardOk())
            {
                // v0.3.2: say so (menu text + log), the game keeps its value.
                s_rdChoice = s_rdApplied;
                s_refusedVal = choice;
                s_refusedWhy = prof::kWhyRdLowMem;
                s_st.guardRefusals++;
                prof::PushEvent(prof::EvKind::DistanceRefused, static_cast<u8>(choice),
                                static_cast<u8>(s_evNormal > 0 ? s_evNormal : 0), prof::kWhyRdLowMem);
                Notice("Render distance: refused, game memory is full");
            }
            else
            {
                s_setWhy = prof::kWhyRdSetting;
                if (SetOptionRd(choice))
                    s_rdApplied = choice;
            }
        }
        // experimental values: re-check every 2 s, step down on low memory
        if (s_inWorld && s_rdApplied > 0 && IsExperimental(s_rdApplied) && now - s_guardMs >= 2000)
        {
            s_guardMs = now;
            if (!GuardOk())
            {
                const int step = s_rdApplied - 1 > s_back ? s_rdApplied - 1 : s_back;
                s_setWhy = prof::kWhyRdStepDown;
                if (SetOptionRd(step))
                {
                    s_rdApplied = step;
                    s_rdChoice = step;
                    s_st.stepDowns++;
                    Notice("Low memory: render distance lowered");
                }
            }
        }

        // ---- 5. heap statistics for the log ----
        if (s_heapWanted && now - s_heapMs >= 1000)
        {
            s_heapMs = now;
            GuardOk();
        }
    }

    int DrawnChunks()
    {
        return s_pubInWorld ? s_pubDrawn : -1;
    }

    std::string RdStatusName()
    {
        const int choice = s_rdChoice, refused = s_refusedVal, normal = s_pubNormal, drawn = s_pubDrawn;
        if (refused > 0)
        {
            const char *why = s_refusedWhy == prof::kWhyRdLowMem ? "low memory" : "not allowed";
            return normal > 0 ? Utils::Format("Render dist.: %d refused (%s), %d used", refused, why, normal)
                              : Utils::Format("Render dist.: %d refused (%s)", refused, why);
        }
        if (!s_pubInWorld || normal <= 0)
            return choice == 0 ? std::string("Render distance: game setting")
                               : Utils::Format("Render distance: %d%s (in a world)", choice,
                                               IsExperimental(choice) ? " exp." : "");
        if (drawn > 0 && drawn < normal)
            return Utils::Format("Render distance: %d -> %d in 3D (auto)", normal, drawn);
        return Utils::Format("Render distance: %d%s", normal, choice == 0 ? " (game setting)" : "");
    }

    void TakeStats(WorldStats &out)
    {
        const game::GameVersion *v = V();
        out = s_st;
        s_st = WorldStats{};
        out.optRd = -1;
        out.leaves = -1;
        out.lrBlocks = -1;
        out.lrFar = 0.0f;
        out.ctxBlocks = static_cast<s32>(mc3d_rd.lastCtx);
        out.clampBlocks = mc3d_rd.clamp;
        u32 lr, client, options;
        if (v && Pointers(lr, client, options))
        {
            out.optRd = static_cast<s32>(W32(options + v->offOptRenderDist));
            out.leaves = static_cast<s8>(*reinterpret_cast<volatile u8 *>(options + v->offOptLeaves));
            out.lrBlocks = static_cast<s32>(W32(lr + v->offLrRadius));
            const u32 fw = W32(lr + v->offLrFar);
            std::memcpy(&out.lrFar, &fw, 4);
        }
        out.farOverride = v && v->farOverride ? *reinterpret_cast<volatile float *>(v->farOverride) : 0.0f;
        out.rdAuto = s_rdHyst.applied ? 1 : 0;
        out.leavesMode = static_cast<u8>(s_leaves);
        out.guardOk = s_guardOk ? 1 : 0;
        out.saveSubst = static_cast<u16>(s_saveSubst);
        for (u32 i = 0; i < kHeapSlots; i++)
            out.heap[i] = i < s_heapCount ? s_heap[i] : HeapStat{};
        out.heapCount = s_heapCount;
        out.heapWalkOk = s_heapWalkOk;
    }
}
