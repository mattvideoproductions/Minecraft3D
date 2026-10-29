// v0.3.3 A/B button swap: see input_swap.hpp and the project's design notes (not in this repo).
#include "input_swap.hpp"
#include "config.hpp"
#include "game_versions.hpp"
#include "hooks.hpp"
#include "pica_shadow.hpp"
#include "version_check.hpp"

#include <CTRPluginFramework.hpp>

using namespace CTRPluginFramework;

namespace mc3d::input
{
    namespace
    {
        // nn::hid ExtraPadStatus (0x18 B): s16 stick x/y, s16 C-stick x/y,
        // u32 hold @+8, trigger @+0xC, release @+0x10, u8 battery, u8 connected.
        constexpr u32 kStatusSize = 0x18, kOffHold = 0x8, kOffTrig = 0xC, kOffRel = 0x10;
        constexpr s32 kMaxSamples = 0x20;      // the game asks for 0x20 (0x10B69C)

        hook::FunctionHook h_latest, h_read;
        volatile bool s_want = false;           // menu (plugin thread)
        bool s_both = false;                    // both hooks in: only then swap anything
        bool s_cfgOff = false;                  // hooks= in the cfg leaves out "input"
        pica::AbLatch s_latch;                  // game main thread only

        u32 Touched(const u8 *st)
        {
            return *reinterpret_cast<const u32 *>(st + kOffHold) | *reinterpret_cast<const u32 *>(st + kOffTrig) |
                   *reinterpret_cast<const u32 *>(st + kOffRel);
        }

        void Swap(u8 *st)
        {
            for (u32 off : { kOffHold, kOffTrig, kOffRel })
            {
                u32 *w = reinterpret_cast<u32 *>(st + off);
                *w = pica::SwapAB(*w);
            }
        }

        // ExtraPadReader::ReadLatest(reader = a0, status* = a1) -> bool
        u64 Hook_PadReadLatest(MC3D_HOOK_ARGS)
        {
            const u64 r = hook::FunctionHook::CallOriginal(MC3D_HOOK_PASS);
            u8 *st = reinterpret_cast<u8 *>(a1);
            if (s_both && static_cast<u32>(r) != 0 && st && pica::AbLatchStep(s_latch, s_want, Touched(st)))
                Swap(st);
            return r;
        }

        // ExtraPadReader::Read(reader = a0, status[] = a1, s32 *count = a2, s32 max = a3)
        u64 Hook_PadRead(MC3D_HOOK_ARGS)
        {
            const u64 r = hook::FunctionHook::CallOriginal(MC3D_HOOK_PASS);
            u8 *buf = reinterpret_cast<u8 *>(a1);
            const s32 *pc = reinterpret_cast<const s32 *>(a2);
            if (!s_both || !buf || !pc)
                return r;
            s32 n = *pc;
            const s32 cap = static_cast<s32>(a3) < kMaxSamples ? static_cast<s32>(a3) : kMaxSamples;
            if (n > cap)
                n = cap;
            if (n <= 0)
                return r;                       // nothing returned: the latch learns nothing
            u32 touched = 0;
            for (s32 i = 0; i < n; i++)
                touched |= Touched(buf + i * kStatusSize);
            if (pica::AbLatchStep(s_latch, s_want, touched))
                for (s32 i = 0; i < n; i++)
                    Swap(buf + i * kStatusSize);
            return r;
        }
    }

    void Install()
    {
        int o;
        if ((o = cfg::StereoOpt("abswap")) >= 0)
            s_want = o != 0;
        const game::GameVersion *v = version::Current();
        s_cfgOff = !cfg::Hook(cfg::kInput);
        if (!v || !v->fnPadReadLatest || !v->fnPadRead || s_cfgOff)
            return;
        // Both or nothing (s_both): one reader swapped and the other not
        // would give the game two different answers in the same frame.
        if (h_latest.Install(v->fnPadReadLatest, Hook_PadReadLatest, hook::ExpectedFirstWord(v->fnPadReadLatest)) !=
            hook::Result::Ok)
            return;
        h_read.Install(v->fnPadRead, Hook_PadRead, hook::ExpectedFirstWord(v->fnPadRead));
        s_both = h_latest.Installed() && h_read.Installed();
    }

    bool Available()      { return s_both; }
    void SetSwap(bool on) { s_want = on; }
    bool SwapWanted()     { return s_want; }
    bool SwapActive()     { return Available() && s_latch.active; }

    std::string HookStatus()
    {
        if (s_cfgOff)
            return "A/B swap: hook off (cfg hooks= without input)\n";
        if (!h_latest.Installed() && !h_read.Installed() && h_latest.LastResult() == hook::Result::Missing)
            return "A/B swap: n/a for this version\n";
        return Utils::Format("A/B swap: latest %s, read %s; menu %s, in effect %s\n",
                             hook::ResultString(h_latest.LastResult()), hook::ResultString(h_read.LastResult()),
                             s_want ? "on" : "off", SwapActive() ? "on" : "off");
    }
}
