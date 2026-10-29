#include "hooks.hpp"
#include "memory.hpp"
#include "version_check.hpp"

using namespace CTRPluginFramework;

namespace mc3d::hook
{
    namespace
    {
        // ARM-mode instruction that reads pc (or branches relative to it) and
        // so cannot run relocated. Conservative: any use of r15 as Rn/Rm/Rs,
        // b/bl/blx(imm), bx/blx(reg) are rejected.
        bool IsPcDependent(u32 w)
        {
            const u32 cond = w >> 28;
            if (cond == 0xF)
                return true;                                  // blx imm, pld..: unconditional space
            if ((w & 0x0E000000) == 0x0A000000)
                return true;                                  // b / bl
            if ((w & 0x0FFFFFD0) == 0x012FFF10)
                return true;                                  // bx / blx reg
            const u32 rn = (w >> 16) & 0xF, rd = (w >> 12) & 0xF, rs = (w >> 8) & 0xF, rm = w & 0xF;
            const u32 cls = (w >> 25) & 7;
            switch (cls)
            {
                case 0:  // data processing (reg) / multiply / misc / extra load-store
                    return rn == 15 || rd == 15 || rm == 15 || ((w & 0x10) && rs == 15);
                case 1:  // data processing (imm)
                    return rn == 15 || rd == 15;
                case 2:  // load/store imm (incl. ldr rX,[pc,#])
                    return rn == 15 || rd == 15;
                case 3:  // load/store reg
                    return rn == 15 || rd == 15 || rm == 15;
                case 4:  // ldm/stm: base pc, or pc in the list of an ldm
                    return rn == 15 || ((w & 0x00100000) && (w & 0x8000));
                default: // coprocessor / svc: vldr [pc] etc.
                    return (cls == 6) && rn == 15;
            }
        }
    }

    u32 ExpectedFirstWord(u32 addr)
    {
        const game::GameVersion *v = version::Current();
        if (!v || !addr)
            return 0;
        for (u32 i = 0; i < v->sigCount; i++)
        {
            const game::Sig &s = v->sigs[i];
            if (s.addr == addr)
                return s.bytes[0] | (s.bytes[1] << 8) | (s.bytes[2] << 16) | (static_cast<u32>(s.bytes[3]) << 24);
        }
        return 0;
    }

    Result FunctionHook::Install(u32 target, Callback callback, u32 expectedInsn)
    {
        if (_installed)
            return _last = Result::AlreadyInstalled;
        if (!target)
            return _last = Result::Missing;
        if (!mem::PatchingAllowed())
            return _last = Result::Locked;
        if ((target & 3) || mem::RegionOf(target, 8) != mem::Region::Text)
            return _last = Result::BadAddress;

        // Always require the expected first instruction: never hook blind.
        u32 cur;
        if (expectedInsn == 0 || !mem::Read32(target, cur) || cur != expectedInsn)
            return _last = Result::UnexpectedCode;

        // MITM mode: the first instruction is copied verbatim into the CTRPF
        // stub (HookManager.cpp) and OriginalFunction() runs it there, then
        // continues at target + 4. CTRPF does NOT check MITM targets for
        // PC-relative instructions, so we refuse them ourselves.
        if (IsPcDependent(cur))
            return _last = Result::UnexpectedCode;
        _hook.InitializeForMitm(target, reinterpret_cast<u32>(callback));
        _ctrpfResult = _hook.Enable();
        if (_ctrpfResult != HookResult::Success)
            return _last = Result::CtrpfError;

        _target = target;
        _installed = true;
        return _last = Result::Ok;
    }

    Result SubWrapHook::Install(u32 site, u32 expectedBl, void (*before)(void), void (*after)(void))
    {
        if (_installed)
            return _last = Result::AlreadyInstalled;
        if (!site)
            return _last = Result::Missing;
        if (!mem::PatchingAllowed())
            return _last = Result::Locked;
        if ((site & 3) || mem::RegionOf(site, 4) != mem::Region::Text)
            return _last = Result::BadAddress;

        u32 cur;
        if ((expectedBl >> 24) != 0xEB || !mem::Read32(site, cur) || cur != expectedBl)
            return _last = Result::UnexpectedCode;

        _hook.InitializeForSubWrap(site, reinterpret_cast<u32>(before), reinterpret_cast<u32>(after));
        if (_hook.Enable() != HookResult::Success)
            return _last = Result::CtrpfError;

        _target = site;
        _installed = true;
        return _last = Result::Ok;
    }

    Result InstrHook::Install(u32 site, u32 expectedInsn, void (*callback)(void))
    {
        if (_installed)
            return _last = Result::AlreadyInstalled;
        if (!site || !callback)
            return _last = Result::Missing;
        if (!mem::PatchingAllowed())
            return _last = Result::Locked;
        if ((site & 3) || mem::RegionOf(site, 4) != mem::Region::Text)
            return _last = Result::BadAddress;
        u32 cur;
        if (expectedInsn == 0 || !mem::Read32(site, cur) || cur != expectedInsn || IsPcDependent(cur))
            return _last = Result::UnexpectedCode;
        _hook.Initialize(site, reinterpret_cast<u32>(callback));
        _hook.SetFlags(USE_LR_TO_RETURN | EXECUTE_OI_BEFORE_CB);
        if (_hook.Enable() != HookResult::Success)
            return _last = Result::CtrpfError;
        _target = site;
        _installed = true;
        return _last = Result::Ok;
    }

    const char *ResultString(Result r)
    {
        switch (r)
        {
            case Result::Ok:               return "ok";
            case Result::Locked:           return "locked";
            case Result::Missing:          return "n/a for this version";
            case Result::BadAddress:       return "bad address";
            case Result::UnexpectedCode:   return "unexpected code";
            case Result::AlreadyInstalled: return "already installed";
            case Result::CtrpfError:       return "CTRPF error";
        }
        return "?";
    }
}
