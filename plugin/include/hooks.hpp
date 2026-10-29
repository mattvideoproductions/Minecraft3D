#pragma once
// Function hooking on top of CTRPluginFramework's Hook (MITM / trampoline mode).
//
// All hooks in this plugin use the same pass-through signature (HOOK_ARGS):
// r0-r3 and d0-d3 (= s0-s7) are received and forwarded to the original, and
// r0:r1 is returned. That way float arguments (the world render takes s0-s3,
// the job budget takes d0) survive, whatever the callback does in between.
// None of the hooked functions takes stack arguments (checked in
// the project's design notes (not in this repo)); a function that does must not be hooked this way.
//
// The callback must call CallOriginal() BEFORE calling anything that could
// itself be hooked: CTRPF keeps the current hook context in TLS slot 15 and a
// nested hook overwrites it.
//
// Install() refuses if the version check failed, the target is outside game
// .text, or the first instruction is not the expected one.
#include <3ds/types.h>
#include <CTRPluginFramework.hpp>

#define MC3D_HOOK_ARGS   u32 a0, u32 a1, u32 a2, u32 a3, double f0, double f1, double f2, double f3
#define MC3D_HOOK_PASS   a0, a1, a2, a3, f0, f1, f2, f3

namespace mc3d::hook
{
    enum class Result
    {
        Ok,
        Locked,             // version check not passed
        Missing,            // address not known for this game version (0)
        BadAddress,         // target outside game .text
        UnexpectedCode,     // first instruction != expected
        AlreadyInstalled,
        CtrpfError,         // see LastCtrpfResult()
    };

    using Callback = u64 (*)(MC3D_HOOK_ARGS);

    class FunctionHook
    {
    public:
        Result  Install(u32 target, Callback callback, u32 expectedInsn);
        bool    Installed() const { return _installed; }
        u32     Target() const { return _target; }
        Result  LastResult() const { return _last; }
        CTRPluginFramework::HookResult LastCtrpfResult() const { return _ctrpfResult; }

        static inline u64 CallOriginal(MC3D_HOOK_ARGS)
        {
            return CTRPluginFramework::HookContext::GetCurrent().OriginalFunction<u64>(MC3D_HOOK_PASS);
        }

    private:
        CTRPluginFramework::Hook       _hook;
        CTRPluginFramework::HookResult _ctrpfResult = CTRPluginFramework::HookResult::Success;
        Result  _last = Result::Missing;
        u32     _target = 0;
        bool    _installed = false;
    };

    // Wraps one `bl` instruction inside a game function (CTRPF WRAP_SUB):
    //   [before(r0-r3 as the game set them)] ; original sub(r0-r3, stack args) ; [after]
    // The original call is made with sp and the stack arguments untouched, so
    // subs with stack arguments are fine. `before` must preserve r0-r3 unless
    // it wants to change the sub's arguments (asm stub); `after` sees the
    // game's r4-r11 (asm stub) and may clobber r0-r3/r12 only where the game
    // reloads them before use (checked per site in the project's design notes (not in this repo)).
    class SubWrapHook
    {
    public:
        Result  Install(u32 site, u32 expectedBl, void (*before)(void), void (*after)(void));
        bool    Installed() const { return _installed; }
        u32     Target() const { return _target; }
        Result  LastResult() const { return _last; }

    private:
        CTRPluginFramework::Hook       _hook;
        Result  _last = Result::Missing;
        u32     _target = 0;
        bool    _installed = false;
    };

    // v0.3: replaces ONE ordinary instruction inside a function (CTRPF
    // default hook: USE_LR_TO_RETURN | EXECUTE_OI_BEFORE_CB). The original
    // instruction runs first in the CTRPF wrapper, then `callback` is
    // `blx`ed with the game's r0-r3 (r4-r11 untouched), then execution
    // continues at site + 4 with whatever r0-r3 the callback returns.
    // The callback must be an asm stub that preserves everything it does
    // not mean to change (stereo_stubs.s). Requirements checked here: the
    // expected word, not PC-relative, game .text, and (by the caller, in the
    // RE notes) that lr and the flags are dead at the site.
    class InstrHook
    {
    public:
        Result  Install(u32 site, u32 expectedInsn, void (*callback)(void));
        bool    Installed() const { return _installed; }
        u32     Target() const { return _target; }
        Result  LastResult() const { return _last; }

    private:
        CTRPluginFramework::Hook       _hook;
        Result  _last = Result::Missing;
        u32     _target = 0;
        bool    _installed = false;
    };

    // First instruction word at `addr` as recorded in the version table's
    // signature (0 if the table has no signature there).
    u32         ExpectedFirstWord(u32 addr);
    const char *ResultString(Result r);
}
