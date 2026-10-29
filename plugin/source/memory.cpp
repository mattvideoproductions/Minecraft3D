#include "memory.hpp"
#include "version_check.hpp"

#include <CTRPluginFramework.hpp>

// Luma3DS custom SVC, exported by libctrpf (csvc.s). The ONLY i-cache
// maintenance this plugin uses: the range variant crashed the kernel on
// hardware (LESSONS.md (instruction cache)).
extern "C" void svcInvalidateEntireInstructionCache(void);

using CTRPluginFramework::Process;

namespace mc3d::mem
{
    static bool s_patchingAllowed = false;
    static bool s_rwx = false;

    bool PatchingAllowed() { return s_patchingAllowed; }
    void SetPatchingAllowed(bool allowed) { s_patchingAllowed = allowed; }

    static bool InRange(u32 addr, u32 len, u32 start, u32 end)
    {
        return start < end && addr >= start && len <= end - start && addr - start <= (end - start) - len;
    }

    Region RegionOf(u32 addr, u32 len)
    {
        const game::GameVersion *v = version::Current();
        if (!v)
            return Region::None;
        if (InRange(addr, len, v->textStart, v->textEnd)) return Region::Text;
        if (InRange(addr, len, v->roStart, v->roEnd))     return Region::RoData;
        if (InRange(addr, len, v->dataStart, v->dataEnd)) return Region::Data;
        return Region::None;
    }

    void PrepareTextWrites()
    {
        const game::GameVersion *v = version::Current();
        if (!s_patchingAllowed || !v || s_rwx)
            return;
        Process::ProtectMemory(v->textStart, v->textEnd - v->textStart);
        s_rwx = true;
    }

    bool Read32(u32 addr, u32 &out)
    {
        if ((addr & 3) || RegionOf(addr, 4) == Region::None)
            return false;
        out = *reinterpret_cast<const volatile u32 *>(addr);
        return true;
    }

    bool Read8(u32 addr, u8 &out)
    {
        if (RegionOf(addr, 1) == Region::None)
            return false;
        out = *reinterpret_cast<const volatile u8 *>(addr);
        return true;
    }

    bool Write(u32 addr, const void *src, u32 len, void *original)
    {
        if (!s_patchingAllowed || len == 0 || src == nullptr)
            return false;

        Region r = RegionOf(addr, len);
        if (r == Region::None)
            return false;

        if (r == Region::Data)
        {
            // Plain data: no cache maintenance needed (same CPU, cached RAM;
            // the GPU never reads these game variables).
            if (original)
                __builtin_memcpy(original, reinterpret_cast<const void *>(addr), len);
            __builtin_memcpy(reinterpret_cast<void *>(addr), src, len);
            return true;
        }

        if (!s_rwx)
        {
            // Normally done in PatchProcess; this is the fallback.
            Process::ProtectMemory(addr, len);
            s_rwx = true;
        }

        if (!Process::Patch(addr, const_cast<void *>(src), len, original))
            return false;

        // Process::Patch flushed the data cache for the range.
        if (r == Region::Text)
            svcInvalidateEntireInstructionCache();
        return true;
    }

    bool Write32(u32 addr, u32 value, u32 *original) { return Write(addr, &value, 4, original); }
    bool Write8(u32 addr, u8 value, u8 *original)    { return Write(addr, &value, 1, original); }

    bool Replace32(u32 addr, u32 expected, u32 value)
    {
        u32 cur;
        if (!Read32(addr, cur) || cur != expected)
            return false;
        return Write32(addr, value);
    }
}
