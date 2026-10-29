#include <string>

#include "stereo.hpp"
#include "config.hpp"
#include "stereo_flat.hpp"
#include "hooks.hpp"
#include "memory.hpp"
#include "profiler.hpp"
#include "version_check.hpp"
#include "world_opts.hpp"

#include <3ds.h>
#include <CTRPluginFramework.hpp>

using namespace CTRPluginFramework;

extern "C" float stereo_get_slider(void)
{
    float v = osGet3DSliderState(); // OS shared config page, 0.0 .. 1.0
    if (!(v > 0.0f)) v = 0.0f;      // also catches NaN
    if (v > 1.0f) v = 1.0f;
    return v;
}

namespace mc3d::stereo
{
    namespace
    {
        hook::FunctionHook h_present;

        // options (plugin thread writes, game thread reads)
        volatile bool s_optEnabled = true;
        volatile bool s_optForceSave = false;
        volatile bool s_optFallback = false;
        volatile int  s_optShift = 5;

        // game-thread state
        volatile State s_state = State::Unavailable;
        bool  s_checked = false;     // first-frame sanity check done
        u32   s_mode = game::kModeNormal;   // mode we last set (or found)
        s32   s_rowOff = 0;          // right-eye row offset we last wrote
        bool  s_gatePatched = false;
        u32   s_frames3D = 0;
        u32   s_faultInfo = 0;

        const game::GameVersion *V() { return version::Current(); }

        bool StereoFieldsKnown(const game::GameVersion *v)
        {
            return v && v->stereoFlag && v->nngxState && v->offDisplayMode && v->fnSetDisplayMode;
        }

        volatile u8  *FlagPtr()  { return reinterpret_cast<volatile u8 *>(V()->stereoFlag); }
        volatile u32 *ModePtr()  { return reinterpret_cast<volatile u32 *>(V()->nngxState + V()->offDisplayMode); }
        volatile s32 *RowOffPtr(){ return reinterpret_cast<volatile s32 *>(V()->nngxState + V()->offRightRowOffset); }

        void SetMode(u32 mode)
        {
            // The game's own nngxSetDisplayMode: stores the mode and the
            // scanout width (0x190 for 0x700/0x702). Takes effect at the next
            // nngxSwapBuffers, i.e. together with the next frame.
            reinterpret_cast<void (*)(u32)>(V()->fnSetDisplayMode)(mode);
            s_mode = mode;
        }

        void SetGate(bool patched)
        {
            const game::GameVersion *v = V();
            if (!v->siteCmdlistGate || patched == s_gatePatched)
                return;
            const u32 from = patched ? v->insnCmdlistGate : v->insnCmdlistGatePatched;
            const u32 to = patched ? v->insnCmdlistGatePatched : v->insnCmdlistGate;
            // Same thread that runs present(), and present() has returned, so
            // the gate instruction is not executing. One aligned word + full
            // I-cache invalidate (memory.cpp).
            if (mem::Replace32(v->siteCmdlistGate, from, to))
                s_gatePatched = patched;
        }

        void SetRowOffset(s32 off)
        {
            if (off == s_rowOff || !V()->offRightRowOffset)
                return;
            *RowOffPtr() = off;
            s_rowOff = off;
        }

        // First frame: the state we are about to drive must look like the RE
        // says (mode 0x700, flag 0/1, right-eye offset 0). Otherwise stay out.
        bool SanityCheck()
        {
            const u32 mode = *ModePtr();
            const u8 flag = FlagPtr()[0];
            const u8 saved = FlagPtr()[1];
            const s32 off = V()->offRightRowOffset ? *RowOffPtr() : 0;
            if (mode != game::kModeNormal) { s_faultInfo = 0x10000000 | mode; return false; }
            if (flag > 1 || saved > 1)     { s_faultInfo = 0x20000000 | (flag << 8) | saved; return false; }
            if (off != 0)                  { s_faultInfo = 0x30000000 | (static_cast<u32>(off) & 0xFFFF); return false; }
            return true;
        }

        // Main thread, right after the original present() returned: the frame
        // was swapped, gStereoThisFrame was just cleared by the game, and the
        // command-list save for the NEXT frame has (or has not) been started.
        // Nothing written here is read by the GPU or the replayed command list
        // (LESSONS #8): the byte steers CPU code of the next frame, the mode and
        // row offset are read by the next nngxSwapBuffers.
        void OnFrameEnd()
        {
            const game::GameVersion *v = V();
            if (!StereoFieldsKnown(v))
                return;

            if (!s_checked)
            {
                s_checked = true;
                if (!SanityCheck())
                {
                    s_state = State::Fault;
                    return;
                }
                s_state = State::Off;
            }
            if (s_state == State::Fault)
                return;

            const float slider = stereo_get_slider();
            const bool on = slider > 0.0f;
            const bool fallback = on && s_optFallback;
            const bool want3D = on && s_optEnabled && !s_optFallback && v->fnStereoReplay;

            // Fast path: slider at 0 / everything off and already stock -> no writes.
            if (!want3D && !fallback && s_mode == game::kModeNormal && s_rowOff == 0 && !s_gatePatched)
            {
                s_state = State::Off;
                return;
            }

            SetGate(want3D && s_optForceSave);

            u32 mode = game::kModeNormal;
            s32 rowOff = 0;
            State st = State::Off;

            if (want3D)
            {
                if (FlagPtr()[1])          // gCmdlistSaved: next frame will be replayed
                {
                    FlagPtr()[0] = 1;      // gStereoThisFrame (the game clears it again at 0x3A3110)
                    mode = game::kModeStereo;
                    st = State::Stereo;
                    s_frames3D++;
                }
                else
                {
                    // Without a saved list the replay is skipped, but present()
                    // would still skip RunCmdlist and copy the stale right RT:
                    // never set the flag in that case.
                    st = State::WaitCmdlist;
                }
            }
            else if (fallback)
            {
                // gStereoThisFrame stays 0 (the game cleared it), so present()
                // copies the LEFT render target into the DISPLAY0_EXT buffer;
                // scanning that buffer out a few rows earlier/later gives a
                // flat, uniform depth.
                mode = game::kModeStereo;
                const float rows = slider * static_cast<float>(s_optShift);
                rowOff = static_cast<s32>(rows < 0.0f ? rows - 0.5f : rows + 0.5f);
                st = State::Fallback;
            }

            if (mode == game::kModeNormal)
            {
                SetRowOffset(0);
                if (s_mode != mode)
                    SetMode(mode);
            }
            else
            {
                if (s_mode != mode)
                    SetMode(mode);
                SetRowOffset(rowOff);
            }
            s_state = st;
        }

        bool KillSwitch()
        {
            const game::GameVersion *v = V();
            return v && v->stereoKind == game::StereoKind::KillSwitch;
        }

        u64 Hook_Present(MC3D_HOOK_ARGS)
        {
            const bool prof = prof::Active();
            const u64 t0 = prof ? svcGetSystemTick() : 0;
            const u64 r = hook::FunctionHook::CallOriginal(MC3D_HOOK_PASS);
            if (KillSwitch())
                OnFrameEnd();
            else
                world::Update(ComfortActive(), ComfortSelected());  // v0.3: 3D distance, leaves, fog, setting
            prof::OnPresent(prof, t0, prof ? svcGetSystemTick() : 0);
            return r;
        }
    }

    void InstallHooks()
    {
        const game::GameVersion *v = V();
        if (!v)
            return;
        if (cfg::Hook(cfg::kPresent))
            h_present.Install(v->fnPresent, Hook_Present, hook::ExpectedFirstWord(v->fnPresent));
        if (v->stereoKind == game::StereoKind::FlatParallax)
            flat::Install();
        else
            s_state = (h_present.Installed() && KillSwitch() && StereoFieldsKnown(v)) ? State::Off : State::Unavailable;
    }

    bool FrameHookInstalled() { return h_present.Installed(); }
    bool IsFlatParallax()     { return V() && V()->stereoKind == game::StereoKind::FlatParallax; }

    std::string HookStatus()
    {
        std::string s = Utils::Format("present %08lX %s\n", h_present.Target(), hook::ResultString(h_present.LastResult()));
        if (IsFlatParallax())
            s += flat::HookStatus();
        return s;
    }

    void SetEnabled(bool on)          { s_optEnabled = on; flat::SetEnabled(on); }
    void SetForceCmdlistSave(bool on) { s_optForceSave = on; }
    void SetFallback(bool on)         { s_optFallback = on; }
    void SetFallbackShift(int rows)   { s_optShift = rows < -12 ? -12 : rows > 12 ? 12 : rows; }
    int  FallbackShift()              { return s_optShift; }
    State GetState()                  { return IsFlatParallax() ? flat::GetState() : s_state; }

    const char *StateName(State s)
    {
        switch (s)
        {
            case State::Unavailable: return "n/a";
            case State::Off:         return "off";
            case State::Stereo:      return "stereo";
            case State::WaitCmdlist: return "wait-cmdlist";
            case State::Fallback:    return "fallback";
            case State::Fault:       return "FAULT";
            case State::WaitInit:    return "wait-init";
            case State::NoBuffers:   return "NO-BUFFERS";
            case State::Flat:        return "flat3D";
            case State::Depth:       return "alt3D";
            case State::Comfort:     return "comfort3D";
            case State::Mono3D:      return "comfort-2D";
        }
        return "?";
    }

    std::string DebugText()
    {
        const game::GameVersion *v = V();
        if (IsFlatParallax())
            return flat::DebugText();
        if (!KillSwitch() || !StereoFieldsKnown(v))
            return "Stereo: not available for this game version";
        return Utils::Format("Stereo: %s  slider %d%%\nflag %u saved %u  mode %03lX  rowoff %ld\n"
                             "gate %s  3D frames %lu  fault %08lX",
                             StateName(s_state), (int)(stereo_get_slider() * 100.0f + 0.5f),
                             FlagPtr()[0], FlagPtr()[1], *ModePtr(),
                             v->offRightRowOffset ? (long)*RowOffPtr() : 0L,
                             s_gatePatched ? "patched" : "stock", s_frames3D, s_faultInfo);
    }
}
