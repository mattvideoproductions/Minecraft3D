#include "version_check.hpp"
#include "memory.hpp"

#include <CTRPluginFramework.hpp>

using CTRPluginFramework::Process;

namespace mc3d::version
{
    static Status                   s_status = Status::NotChecked;
    static const game::GameVersion *s_current = nullptr;
    static constexpr u32            kMaxTables = 8;
    static u32                      s_mismatch[kMaxTables];
    static u32                      s_tried = 0;

    // Only read addresses inside the running process's .text (or .rodata/.data
    // for literals): a table for a bigger build must not fault on a smaller one.
    static bool Readable(u32 addr, u32 len)
    {
        const u32 textEnd = 0x00100000 + Process::GetTextSize();
        if (addr >= 0x00100000 && addr + len <= textEnd)
            return true;
        return Process::CheckAddress(addr, MEMPERM_READ) && Process::CheckAddress(addr + len - 1, MEMPERM_READ);
    }

    static u32 Word(u32 addr) { return *reinterpret_cast<const volatile u32 *>(addr); }

    static bool HasSig(const game::GameVersion &v, u32 fn)
    {
        for (u32 i = 0; i < v.sigCount; i++)
            if (v.sigs[i].addr == fn)
                return true;
        return false;
    }

    // Returns 0 if `v` matches, else the first mismatching address.
    static u32 Check(const game::GameVersion &v)
    {
        if (v.sigCount == 0)
            return 1;
        if (v.textEnd > 0x00100000 + Process::GetTextSize() + 0x1000)
            return v.textEnd;

        for (u32 i = 0; i < v.sigCount; i++)
        {
            const game::Sig &s = v.sigs[i];
            if ((s.addr & 3) || !Readable(s.addr, sizeof(s.bytes)))
                return s.addr ? s.addr : 2;
            const volatile u8 *p = reinterpret_cast<const volatile u8 *>(s.addr);
            for (u32 k = 0; k < sizeof(s.bytes); k++)
                if (p[k] != s.bytes[k])
                    return s.addr + k;
        }

        // Every hook target / called function must be covered by a signature.
        const u32 fns[] = { v.fnPresent, v.fnWorldRender, v.fnDraw, v.fnDrawIndexed, v.fnBuildImmediate,
                            v.fnStartRebuild, v.fnEndRebuild, v.fnJobBudget, v.fnSetDisplayMode,
                            v.fnSwapBuffers, v.fnGxInit, v.fnSwapCallback, v.fnCopyB2L, v.fnActiveDisplay,
                            v.fnGenDisplaybuffers, v.fnBindDisplaybuffer, v.fnDisplaybufferStorage,
                            v.fnGetDisplaybufferParam, v.fnWorldConstants, v.fnSetRenderTarget,
                            // v0.3
                            v.fnGxClear, v.fnSplit, v.fnOptionsSetInt, v.fnGraphicsChanged, v.fnOptionsSave,
                            v.fnGdFlush, v.fnGdClearTargets, v.fnFbClear };
        for (u32 fn : fns)
            if (fn && !HasSig(v, fn))
                return fn;

        // Site words (only checked when the site is known)
        if (v.siteFlagClear && (!Readable(v.siteFlagClear, 4) || Word(v.siteFlagClear) != v.insnFlagClear))
            return v.siteFlagClear;
        if (v.siteCmdlistGate && (!Readable(v.siteCmdlistGate, 4) || Word(v.siteCmdlistGate) != v.insnCmdlistGate))
            return v.siteCmdlistGate;

        // Data addresses must be what the code actually loads.
        if (v.stereoFlag && (!v.litStereoFlag || !Readable(v.litStereoFlag, 4) || Word(v.litStereoFlag) != v.stereoFlag))
            return v.litStereoFlag ? v.litStereoFlag : 3;
        if (v.nngxState && (!v.litNngxState || !Readable(v.litNngxState, 4) || Word(v.litNngxState) != v.nngxState))
            return v.litNngxState ? v.litNngxState : 4;

        // v9.12 flat parallax: call sites we wrap, literals the swap callback loads.
        if (v.stereoKind == game::StereoKind::FlatParallax)
        {
            if (!v.siteTopB2L || !Readable(v.siteTopB2L, 4) || Word(v.siteTopB2L) != v.insnTopB2L)
                return v.siteTopB2L ? v.siteTopB2L : 5;
            if (!v.siteTopSwap || !Readable(v.siteTopSwap, 4) || Word(v.siteTopSwap) != v.insnTopSwap)
                return v.siteTopSwap ? v.siteTopSwap : 6;
            if (!v.litGxDeviceBase || !Readable(v.litGxDeviceBase, 4) || Word(v.litGxDeviceBase) != v.gxDeviceBase)
                return v.litGxDeviceBase ? v.litGxDeviceBase : 7;
            if (!v.litSwapFormat || !Readable(v.litSwapFormat, 4) || Word(v.litSwapFormat) != v.swapFormat)
                return v.litSwapFormat ? v.litSwapFormat : 8;
            // Request-queue guard: the code must load the bound list from nngxState.
            if (v.litBoundCmdlist && (!Readable(v.litBoundCmdlist, 4) || Word(v.litBoundCmdlist) != v.nngxState))
                return v.litBoundCmdlist;
        }
        if (v.gameProfiler && (!v.litGameProfiler || !Readable(v.litGameProfiler, 4) ||
                               Word(v.litGameProfiler) != v.gameProfiler))
            return v.litGameProfiler ? v.litGameProfiler : 9;

        // v0.3: every data address must be what the code loads, every
        // patched/wrapped site must hold the expected instruction. A field
        // left 0 switches only its own feature off.
        struct Lit { u32 lit, value; };
        const Lit lits[] = {
            { v.litSplitCursor, v.cmdCursor }, { v.litSplitLimit, v.cmdLimit },
            { v.litVblankState, v.nngxState }, { v.litOptRenderDistance, v.optRenderDistance },
            { v.litRdLevels, v.rdLevels }, { v.litFarOverride, v.farOverride },
            { v.litHeapTable, v.heapTable }, { v.litHeapVtable, v.heapVtable },
        };
        for (const Lit &l : lits)
            if (l.value && (!l.lit || !Readable(l.lit, 4) || Word(l.lit) != l.value))
                return l.lit ? l.lit : 10;
        struct Site { u32 site, insn; };
        const Site sites[] = { { v.siteLevelRender, v.insnLevelRender }, { v.siteOverlayRender, v.insnOverlayRender },
                               { v.siteRdClamp, v.insnRdClamp } };
        for (const Site &s : sites)
            if (s.site && (!Readable(s.site, 4) || Word(s.site) != s.insn))
                return s.site;
        return 0;
    }

    Status Select()
    {
        mem::SetPatchingAllowed(false);
        s_current = nullptr;
        s_tried = 0;

        if (Process::GetTitleID() != game::kTitleId)
            return s_status = Status::WrongTitle;

        for (u32 i = 0; i < game::kVersionCount && i < kMaxTables; i++)
        {
            s_mismatch[i] = Check(*game::kVersions[i]);
            s_tried = i + 1;
            if (s_mismatch[i] == 0)
            {
                s_current = game::kVersions[i];
                mem::SetPatchingAllowed(true);
                return s_status = Status::Ok;
            }
        }
        return s_status = Status::Unsupported;
    }

    Status                    GetStatus()   { return s_status; }
    bool                      IsSupported() { return s_status == Status::Ok; }
    const game::GameVersion  *Current()     { return s_current; }
    const char               *VersionName() { return s_current ? s_current->name : "unknown"; }
    u32                       TablesTried() { return s_tried; }
    u32                       FirstMismatch(u32 t) { return t < s_tried ? s_mismatch[t] : 0; }

    const char *StatusString(Status s)
    {
        switch (s)
        {
            case Status::NotChecked:  return "not checked";
            case Status::Ok:          return "OK";
            case Status::WrongTitle:  return "wrong title (not Minecraft NN3DS USA)";
            case Status::Unsupported: return "unsupported game version";
        }
        return "?";
    }
}
