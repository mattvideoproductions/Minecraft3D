// PICA200 command-stream shadow (pure code, see pica_shadow.hpp).
//
// Command format (3dbrew "GPU/Internal Registers", same as the gd emitter
// 0x4B76B8 and the nngx split 0x7C5518 write):
//   word 0  first parameter
//   word 1  header: bits 0-15 register id, 16-19 byte-enable mask,
//           20-27 number of EXTRA parameters, 31 = consecutive registers
//   words 2.. the extra parameters, then one padding word if needed so the
//   next command starts 8-byte aligned.
#include "pica_shadow.hpp"

#include <cstring>
#include <initializer_list>

namespace mc3d::pica
{
    namespace
    {
        // Register ids (3dbrew names)
        constexpr u32 kFinalize = 0x010;
        constexpr u32 kEarlyDepthClear = 0x063;
        constexpr u32 kProctexLutIdx = 0x0AF, kProctexLutData0 = 0x0B0, kProctexLutData7 = 0x0B7;
        constexpr u32 kFogLutIdx = 0x0E6, kFogLutData0 = 0x0E8, kFogLutData7 = 0x0EF;
        constexpr u32 kFbInvalidate = 0x110, kFbFlush = 0x111;
        constexpr u32 kGasLutIdx = 0x123, kGasLutData = 0x124;
        constexpr u32 kLightLutIdx = 0x1C5, kLightLutData0 = 0x1C8, kLightLutData7 = 0x1CF;
        constexpr u32 kDrawArrays = 0x22E, kDrawElements = 0x22F, kVtxFunc = 0x231;
        constexpr u32 kFattrIdx = 0x232, kFattrData0 = 0x233, kFattrData2 = 0x235;
        constexpr u32 kJump0 = 0x23C, kJump1 = 0x23D;
        constexpr u32 kStartDrawFunc0 = 0x245;
        constexpr u32 kRestartPrim = 0x25F;
        // Shader units: GSH at 0x280, VSH at 0x2B0 (same layout)
        constexpr u32 kGshBase = 0x280, kVshBase = 0x2B0;
        constexpr u32 kOffCodeEnd = 0x0F, kOffFloatIdx = 0x10, kOffFloat0 = 0x11, kOffFloat7 = 0x18;
        constexpr u32 kOffCodeIdx = 0x1B, kOffCode0 = 0x1C, kOffCode7 = 0x23;
        constexpr u32 kOffOpIdx = 0x25, kOffOp0 = 0x26, kOffOp7 = 0x2D;

        inline bool In(u32 x, u32 a, u32 b) { return x >= a && x <= b; }

        inline u32 MaskBytes(u32 m)
        {
            return ((m & 1) ? 0x000000FFu : 0) | ((m & 2) ? 0x0000FF00u : 0) |
                   ((m & 4) ? 0x00FF0000u : 0) | ((m & 8) ? 0xFF000000u : 0);
        }

        inline float F(u32 w) { float f; std::memcpy(&f, &w, 4); return f; }
        inline u32 W(float f) { u32 w; std::memcpy(&w, &f, 4); return w; }
    }

    bool IsNoDeltaReg(u32 id)
    {
        if (id <= kFinalize)                                    // IRQ area + finalize
            return true;
        if (id == kEarlyDepthClear || id == kFbInvalidate || id == kFbFlush)
            return true;
        if (In(id, kProctexLutIdx, kProctexLutData7) || id == kFogLutIdx || In(id, kFogLutData0, kFogLutData7))
            return true;
        if (id == kGasLutIdx || id == kGasLutData || id == kLightLutIdx || In(id, kLightLutData0, kLightLutData7))
            return true;
        if (id == kDrawArrays || id == kDrawElements || id == kVtxFunc || In(id, kFattrIdx, kFattrData2))
            return true;
        if (id == kJump0 || id == kJump1 || id == kRestartPrim)
            return true;
        for (u32 b : { kGshBase, kVshBase })
        {
            if (id == b + kOffCodeEnd || In(id, b + kOffFloatIdx, b + kOffFloat7) ||
                In(id, b + kOffCodeIdx, b + kOffCode7) || In(id, b + kOffOpIdx, b + kOffOp7))
                return true;
        }
        return false;
    }

    const char *FailName(Fail f)
    {
        switch (f)
        {
            case Fail::None:            return "none";
            case Fail::BadHeader:       return "bad-header";
            case Fail::Jump:            return "cmd-jump";
            case Fail::FinalizeInRange: return "split-in-range";
            case Fail::UntrackedLut:    return "lut";
            case Fail::PartialMatrix:   return "partial-matrix";
            case Fail::F24Matrix:       return "f24-matrix";
            case Fail::TooManySites:    return "too-many-sites";
            case Fail::PortMask:        return "port-mask";
            case Fail::UnknownAtP0:     return "unknown-state";
            case Fail::EmitOverflow:    return "no-room";
            case Fail::Count:           break;
        }
        return "?";
    }

    void Parser::ResetShadow()
    {
        if (cur)
            std::memset(cur->f, 0, sizeof(cur->f));
        BeginList();
        jump = false;
    }

    void Parser::BeginList()
    {
        vsf_ = {};
        gsf_ = {};
        vcodeIdx_ = gcodeIdx_ = vopIdx_ = gopIdx_ = fogIdx_ = 0;
        fattrIdx_ = fattrN_ = 0;
        inRange_ = false;
        grpFilled_ = 0;
    }

    void Parser::GroupBroken()
    {
        if (grpFilled_ > 0 && grpBase_ == 0)
            SetFail(Fail::PartialMatrix);
        grpFilled_ = 0;
    }

    void Parser::OnVec4(u32 reg, bool f32, const u32 *words, u32 *const *addrs)
    {
        const bool wvp = reg <= 3;
        const bool proj = reg >= 8 && reg <= 11;
        if (!wvp && !proj)
        {
            if (grpFilled_)
                GroupBroken();
            return;
        }
        if (!f32)
        {
            if (grpFilled_)
                GroupBroken();
            if (wvp)
                SetFail(Fail::F24Matrix);
            return;
        }
        if (reg == 0 || reg == 8)
        {
            if (grpFilled_)
                GroupBroken();
            grpBase_ = reg;
            grpFilled_ = 0;
            grpPos_ = static_cast<u32>(reinterpret_cast<uintptr_t>(addrs[0]));
        }
        else if (!(grpFilled_ > 0 && reg == grpBase_ + grpFilled_))
        {
            if (grpFilled_)
                GroupBroken();
            if (wvp)
                SetFail(Fail::PartialMatrix);
            return;
        }
        for (u32 i = 0; i < 4; i++)
            grpRaw_[grpFilled_][i] = words[i];
        if (grpFilled_ == 0)
            for (u32 i = 0; i < 4; i++)
                grpAddr0_[i] = addrs[i];
        grpFilled_++;
        if (grpFilled_ == 4)
        {
            if (sites && siteCount < siteCap)
            {
                Site &s = sites[siteCount++];
                for (u32 i = 0; i < 4; i++)
                {
                    s.row0[i] = grpAddr0_[i];
                    for (u32 j = 0; j < 4; j++)
                        s.raw[i][j] = grpRaw_[i][j];
                }
                s.pos = grpPos_;
                s.base = static_cast<u8>(grpBase_);
            }
            else
                SetFail(Fail::TooManySites);
            grpFilled_ = 0;
        }
    }

    // In range, a VSH burst may take the inline path only if none of the
    // vec4 registers it completes is c0-c3 or c8-c11 (no site to record)
    // and no matrix group is open (nothing to break).
    bool Parser::BurstClearOfMatrices(u32 words) const
    {
        const u32 need = vsf_.f32 ? 4u : 3u;
        const u32 first = vsf_.idx;
        const u32 regs = (vsf_.n + words) / need + 1;   // registers touched (upper bound)
        for (u32 r = 0; r < regs; r++)
        {
            const u32 x = (first + r) & 0x7F;
            if (x <= 3 || (x >= 8 && x <= 11))
                return false;
        }
        return true;
    }

    void Parser::FloatData(FloatUnit &u, u32 slotBase, bool vsh, u32 val, u32 *addr)
    {
        if (u.n < 4)
        {
            u.buf[u.n] = val;
            u.addr[u.n] = addr;
            u.n++;
        }
        const u32 need = u.f32 ? 4u : 3u;
        if (u.n < need)
            return;
        const u32 reg = u.idx;
        if (reg < kFloatCount)
        {
            u32 *s = cur->v[slotBase + reg];
            s[0] = u.buf[0]; s[1] = u.buf[1]; s[2] = u.buf[2]; s[3] = need == 4 ? u.buf[3] : 0;
            cur->f[slotBase + reg] = static_cast<u8>(kKnown | (u.f32 ? kF32 : 0));
            if (vsh && inRange_)
            {
                if (u.f32)
                    OnVec4(reg, true, u.buf, u.addr);
                else
                    OnVec4(reg, false, u.buf, u.addr);
            }
        }
        u.idx = (u.idx + 1) & 0x7F;
        u.n = 0;
    }

    // Register classes (Azahar v0.3.0: the generic Write cost 7-10 ms/frame;
    // a table lookup + an inline fast path for plain full-mask registers,
    // the vast majority of words, replaces the chain of range tests).
    namespace
    {
        enum Cls : u8
        {
            cPlain = 0, cFinalize, cJump, cLut, cPortOther,
            cVsfIdx, cVsfData, cGsfIdx, cGsfData,
            cVcodeIdx, cVcodeData, cGcodeIdx, cGcodeData,
            cVopIdx, cVopData, cGopIdx, cGopData,
            cFattrIdx, cFattrData, cFogIdx, cFogData,
        };
        u8   s_cls[kRegCount];
        bool s_clsReady = false;

        void BuildClasses()
        {
            for (u32 i = 0; i < kRegCount; i++)
                s_cls[i] = cPlain;
            s_cls[kFinalize] = cFinalize;
            s_cls[kJump0] = s_cls[kJump1] = cJump;
            for (u32 i = kProctexLutData0; i <= kProctexLutData7; i++) s_cls[i] = cLut;
            for (u32 i = kLightLutData0; i <= kLightLutData7; i++) s_cls[i] = cLut;
            s_cls[kGasLutData] = cLut;
            s_cls[kFattrIdx] = cFattrIdx;
            for (u32 i = kFattrData0; i <= kFattrData2; i++) s_cls[i] = cFattrData;
            s_cls[kFogLutIdx] = cFogIdx;
            for (u32 i = kFogLutData0; i <= kFogLutData7; i++) s_cls[i] = cFogData;
            const u32 bases[2] = { kVshBase, kGshBase };
            const u8 fi[2] = { cVsfIdx, cGsfIdx }, fd[2] = { cVsfData, cGsfData };
            const u8 ci[2] = { cVcodeIdx, cGcodeIdx }, cd[2] = { cVcodeData, cGcodeData };
            const u8 oi[2] = { cVopIdx, cGopIdx }, od[2] = { cVopData, cGopData };
            for (int u = 0; u < 2; u++)
            {
                const u32 b = bases[u];
                s_cls[b + kOffFloatIdx] = fi[u];
                for (u32 i = kOffFloat0; i <= kOffFloat7; i++) s_cls[b + i] = fd[u];
                s_cls[b + kOffCodeIdx] = ci[u];
                for (u32 i = kOffCode0; i <= kOffCode7; i++) s_cls[b + i] = cd[u];
                s_cls[b + kOffOpIdx] = oi[u];
                for (u32 i = kOffOp0; i <= kOffOp7; i++) s_cls[b + i] = od[u];
            }
            s_clsReady = true;
        }
    }

    void Parser::Write(u32 id, u32 val, u32 mask, u32 *addr)
    {
        // ---- plain register slot (also keeps index registers' last value) ----
        if (mask)
        {
            // Per-byte knowledge (audit B1): a masked write makes exactly
            // the bytes it writes known, whatever the others are.
            const u32 m = MaskBytes(mask);
            const u8 bytes = static_cast<u8>(RegBytes(cur->f[kRegBase + id]) | mask);
            cur->v[kRegBase + id][0] = (cur->v[kRegBase + id][0] & ~m) | (val & m);
            cur->f[kRegBase + id] = static_cast<u8>((bytes << kByteShift) | (bytes == 0xF ? kKnown : 0));
        }
        const u8 c = s_cls[id];
        const bool data = c == cVsfData || c == cGsfData || c == cVcodeData || c == cGcodeData || c == cVopData ||
                          c == cGopData || c == cFattrData || c == cFogData;
        if (data && mask != 0xF)
            SetFail(Fail::PortMask);
        switch (c)
        {
            case cPlain:
            case cPortOther:
                return;
            case cFinalize:
                // v0.3.1: a split inside the top range is a request boundary;
                // the replay is cut there (comfort.cpp). Remember where.
                if (inRange_)
                {
                    if (finCount < kMaxFin)
                        finAt[finCount++] = addr;
                    else
                        SetFail(Fail::FinalizeInRange);
                }
                return;
            case cJump:
                jump = true;
                SetFail(Fail::Jump);
                return;
            case cLut:
                if (afterP0)
                    SetFail(Fail::UntrackedLut);
                return;
            case cVsfIdx:
            case cGsfIdx:
            {
                FloatUnit &fu = c == cVsfIdx ? vsf_ : gsf_;
                if (c == cVsfIdx && inRange_ && grpFilled_)
                    GroupBroken();
                const u32 v = cur->v[kRegBase + id][0];
                fu.idx = v & 0x7F;
                fu.f32 = (v >> 31) != 0;
                fu.n = 0;
                return;
            }
            case cVsfData:
                FloatData(vsf_, kVsfBase, true, val, addr);
                return;
            case cGsfData:
                FloatData(gsf_, kGsfBase, false, val, addr);
                return;
            case cVcodeIdx: vcodeIdx_ = cur->v[kRegBase + id][0] & 0xFFF; return;
            case cGcodeIdx: gcodeIdx_ = cur->v[kRegBase + id][0] & 0xFFF; return;
            case cVopIdx:   vopIdx_ = cur->v[kRegBase + id][0] & 0xFFF; return;
            case cGopIdx:   gopIdx_ = cur->v[kRegBase + id][0] & 0xFFF; return;
            case cVcodeData:
            case cGcodeData:
            {
                u32 &ix = c == cVcodeData ? vcodeIdx_ : gcodeIdx_;
                if (ix < kCodeCount)
                {
                    const u32 s = (c == cVcodeData ? kVcodeBase : kGcodeBase) + ix;
                    cur->v[s][0] = val;
                    cur->f[s] = kKnown;
                }
                ix++;
                return;
            }
            case cVopData:
            case cGopData:
            {
                u32 &ix = c == cVopData ? vopIdx_ : gopIdx_;
                if (ix < kOpCount)
                {
                    const u32 s = (c == cVopData ? kVopBase : kGopBase) + ix;
                    cur->v[s][0] = val;
                    cur->f[s] = kKnown;
                }
                ix++;
                return;
            }
            case cFattrIdx:
                fattrIdx_ = cur->v[kRegBase + id][0] & 0xF;
                fattrN_ = 0;
                return;
            case cFattrData:
                fattrBuf_[fattrN_ < 3 ? fattrN_ : 2] = val;
                if (++fattrN_ >= 3)
                {
                    if (fattrIdx_ < kFattrCount)
                    {
                        const u32 s = kFattrBase + fattrIdx_;
                        cur->v[s][0] = fattrBuf_[0];
                        cur->v[s][1] = fattrBuf_[1];
                        cur->v[s][2] = fattrBuf_[2];
                        cur->v[s][3] = 0;
                        cur->f[s] = kKnown;
                        fattrIdx_++;
                    }
                    fattrN_ = 0;    // index 15 = immediate-mode vertex data: no state
                }
                return;
            case cFogIdx:
                fogIdx_ = cur->v[kRegBase + id][0] & 0x7F;
                return;
            case cFogData:
            {
                const u32 s = kFogBase + (fogIdx_ & 0x7F);
                cur->v[s][0] = val;
                cur->f[s] = kKnown;
                fogIdx_ = (fogIdx_ + 1) & 0x7F;
                return;
            }
            default:
                return;
        }
    }

    bool Parser::Run(const u32 *begin, const u32 *end)
    {
        if (!cur || !begin || end < begin)
        {
            SetFail(Fail::BadHeader);
            return false;
        }
        if (!s_clsReady)
            BuildClasses();
        u32 (*const v)[4] = cur->v;
        u8 *const f = cur->f;
        constexpr u8 kFull = static_cast<u8>((0xF << kByteShift) | kKnown);
        // Next mark ahead of p (marks are command boundaries).
        const u32 *p = begin;
        for (;;)
        {
            if (markP0 && p == markP0 && !sawP0)
            {
                if (snapP0)
                    std::memcpy(snapP0, cur, sizeof(State));
                sawP0 = true;
                afterP0 = true;
                inRange_ = true;
                grpFilled_ = 0;
            }
            if (markPH && p == markPH && !sawPH)
            {
                if (inRange_ && grpFilled_)
                    GroupBroken();
                if (snapPH)
                    std::memcpy(snapPH, cur, sizeof(State));
                sawPH = true;
                inRange_ = false;
            }
            if (p >= end)
                break;
            if (end - p < 2)
            {
                SetFail(Fail::BadHeader);
                return false;
            }
            u32 *const w = const_cast<u32 *>(p);
            const u32 hdr = w[1];
            const u32 idFull = hdr & 0xFFFF;
            const u32 mask = (hdr >> 16) & 0xF;
            const u32 count = (hdr >> 20) & 0xFF;
            const bool seq = (hdr >> 31) != 0;
            const u32 total = (2 + count + 1) & ~1u;      // padded to 8 bytes
            if (idFull >= kRegCount || static_cast<u32>(end - p) < total ||
                (seq && idFull + count >= kRegCount))
            {
                SetFail(Fail::BadHeader);
                return false;
            }
            // A mark strictly inside this command = we are out of step.
            if ((markP0 && markP0 > p && markP0 < p + total) || (markPH && markPH > p && markPH < p + total))
            {
                SetFail(Fail::BadHeader);
                return false;
            }
            const u8 c0 = s_cls[idFull];
            if (count == 0 && mask == 0xF && c0 == cPlain)
            {
                // fast path: one plain register, all bytes
                v[kRegBase + idFull][0] = w[0];
                f[kRegBase + idFull] = kFull;
            }
            else if (!seq && mask == 0xF && c0 == cVsfData &&
                     (!inRange_ || (grpFilled_ == 0 && BurstClearOfMatrices(count + 1))))
            {
                // fast path: a VSH float-uniform burst outside the top range
                // (no site recording needed): the FIFO inline, no per-word call.
                v[kRegBase + idFull][0] = w[count];
                f[kRegBase + idFull] = kFull;
                FloatUnit &u = vsf_;
                const u32 need = u.f32 ? 4u : 3u;
                for (u32 i = 0; i <= count; i++)
                {
                    u.buf[u.n++] = i == 0 ? w[0] : w[1 + i];
                    if (u.n >= need)
                    {
                        if (u.idx < kFloatCount)
                        {
                            u32 *sl = v[kVsfBase + u.idx];
                            sl[0] = u.buf[0]; sl[1] = u.buf[1]; sl[2] = u.buf[2]; sl[3] = need == 4 ? u.buf[3] : 0;
                            f[kVsfBase + u.idx] = static_cast<u8>(kKnown | (u.f32 ? kF32 : 0));
                        }
                        u.idx = (u.idx + 1) & 0x7F;
                        u.n = 0;
                    }
                }
            }
            else if (seq && mask == 0xF && c0 == cPlain)
            {
                // consecutive plain registers: check every id is plain first
                bool plain = true;
                for (u32 i = 1; i <= count && plain; i++)
                    plain = s_cls[idFull + i] == cPlain;
                if (plain)
                    for (u32 i = 0; i <= count; i++)
                    {
                        v[kRegBase + idFull + i][0] = i == 0 ? w[0] : w[1 + i];
                        f[kRegBase + idFull + i] = kFull;
                    }
                else
                    for (u32 i = 0; i <= count; i++)
                    {
                        u32 *a = i == 0 ? &w[0] : &w[1 + i];
                        Write(idFull + i, *a, mask, a);
                    }
            }
            else
                for (u32 i = 0; i <= count; i++)
                {
                    u32 *a = i == 0 ? &w[0] : &w[1 + i];
                    Write(seq ? idFull + i : idFull, *a, mask, a);
                }
            p += total;
            wordsParsed += total;
        }
        return true;
    }

    // ---- emitter ----
    void Emitter::Reg(u32 id, u32 val, u32 mask)
    {
        if (!Room(2))
            return;
        *p++ = val;
        *p++ = ((mask & 0xF) << 16) | id;
    }

    void Emitter::Word2(u32 idxReg, u32 idx, u32 dataReg, u32 val)
    {
        if (!Room(4))
            return;
        *p++ = idx;
        *p++ = 0x000F0000u | idxReg;
        *p++ = val;
        *p++ = 0x000F0000u | dataReg;
    }

    void Emitter::FloatVec4(bool gsh, u32 reg, bool f32, const u32 *w)
    {
        const u32 b = gsh ? kGshBase : kVshBase;
        if (!Room(f32 ? 8 : 6))
            return;
        *p++ = reg | (f32 ? 0x80000000u : 0);
        *p++ = 0x000F0000u | (b + kOffFloatIdx);
        if (f32)
        {
            *p++ = w[0];
            *p++ = (3u << 20) | 0x000F0000u | (b + kOffFloat0);
            *p++ = w[1];
            *p++ = w[2];
            *p++ = w[3];
            *p++ = 0;                                   // padding
        }
        else
        {
            *p++ = w[0];
            *p++ = (2u << 20) | 0x000F0000u | (b + kOffFloat0);
            *p++ = w[1];
            *p++ = w[2];
        }
    }

    void Emitter::Fattr(u32 idx, const u32 *w)
    {
        if (!Room(6))
            return;
        *p++ = idx;
        *p++ = 0x000F0000u | kFattrIdx;
        *p++ = w[0];
        *p++ = 0x80000000u | (2u << 20) | 0x000F0000u | kFattrData0;   // 0x233, 0x234, 0x235
        *p++ = w[1];
        *p++ = w[2];
    }

    void Emitter::Pad() {}

    namespace
    {
        u8   s_noDelta[kRegCount];
        bool s_noDeltaReady = false;
        inline bool NoDelta(u32 id)
        {
            if (!s_noDeltaReady)
            {
                for (u32 i = 0; i < kRegCount; i++)
                    s_noDelta[i] = IsNoDeltaReg(i) ? 1 : 0;
                s_noDeltaReady = true;
            }
            return s_noDelta[id] != 0;
        }
    }

    Fail EmitTransition(const State &from, const State &to, const u8 *force, Emitter &e)
    {
        auto need = [&](u32 s, u32 nw) -> int {      // 1 emit, 0 skip, -1 impossible
            const bool tk = (to.f[s] & kKnown) != 0, fk = (from.f[s] & kKnown) != 0;
            if (!tk)
                return fk ? -1 : 0;
            if (force && force[s])
                return 1;
            if (!fk || ((to.f[s] ^ from.f[s]) & kF32))
                return 1;
            for (u32 i = 0; i < nw; i++)
                if (to.v[s][i] != from.v[s][i])
                    return 1;
            return 0;
        };

        // Registers, byte by byte (audit B1): the bytes to write are those
        // known in `to` that are unknown in `from` or differ (all known
        // bytes when forced). A byte known in `from` but not in `to` was
        // written after p0 while unknown at p0: not expressible.
        auto regMask = [&](u32 id) -> int {         // -1 impossible, else byte mask to write
            const u8 tb = RegBytes(to.f[kRegBase + id]), fb = RegBytes(from.f[kRegBase + id]);
            if (fb & ~tb & 0xF)
                return -1;
            if (force && force[kRegBase + id])
                return tb;
            const u32 diff = to.v[kRegBase + id][0] ^ from.v[kRegBase + id][0];
            u8 m = static_cast<u8>(tb & ~fb & 0xF);
            for (u32 b = 0; b < 4; b++)
                if ((tb & fb & (1u << b)) && ((diff >> (8 * b)) & 0xFF))
                    m = static_cast<u8>(m | (1u << b));
            return m;
        };

        // Pass 1: is the transition expressible at all?
        for (u32 s = 0; s < kSlotCount; s++)
        {
            if (s < kRegCount)
            {
                if (!NoDelta(s) && regMask(s) < 0)
                    return Fail::UnknownAtP0;
                continue;
            }
            const u32 nw = (s >= kVsfBase && s < kVcodeBase) ? 4 : (s >= kFattrBase && s < kFogBase) ? 3 : 1;
            if (need(s, nw) < 0)
                return Fail::UnknownAtP0;
        }

        // Framebuffer: write back + drop whatever the GPU's framebuffer cache
        // holds before any target register can change (the nngx split does
        // the same at the end of every segment, 0x7C56B0/0x7C56B4).
        e.Reg(kFbFlush, 1);
        e.Reg(kFbInvalidate, 1);

        // Configuration mode while registers change (0x245 = 1), the target
        // value is written last.
        // 0x245 bit 0 lives in byte 0: only touched when that byte is known.
        const u8 sdBytes = RegBytes(to.f[kRegBase + kStartDrawFunc0]);
        const bool sdKnown = (sdBytes & 1) != 0;
        if (sdKnown)
            e.Reg(kStartDrawFunc0, 1, 0x1);
        for (u32 id = 0; id < kRegCount; id++)
        {
            if (id == kStartDrawFunc0 || NoDelta(id))
                continue;
            const int m = regMask(id);
            if (m > 0)
                e.Reg(id, to.v[kRegBase + id][0], static_cast<u32>(m));
        }
        for (u32 g = 0; g < 2; g++)
        {
            const u32 base = g ? kGsfBase : kVsfBase;
            for (u32 r = 0; r < kFloatCount; r++)
                if (need(base + r, 4) > 0)
                    e.FloatVec4(g != 0, r, (to.f[base + r] & kF32) != 0, to.v[base + r]);
        }
        for (u32 g = 0; g < 2; g++)
        {
            const u32 base = g ? kGcodeBase : kVcodeBase;
            const u32 ub = g ? kGshBase : kVshBase;
            bool any = false;
            for (u32 i = 0; i < kCodeCount; i++)
                if (need(base + i, 1) > 0)
                {
                    e.Word2(ub + kOffCodeIdx, i, ub + kOffCode0, to.v[base + i][0]);
                    any = true;
                }
            if (any)
                e.Reg(ub + kOffCodeEnd, 1);
            const u32 ob = g ? kGopBase : kVopBase;
            for (u32 i = 0; i < kOpCount; i++)
                if (need(ob + i, 1) > 0)
                    e.Word2(ub + kOffOpIdx, i, ub + kOffOp0, to.v[ob + i][0]);
        }
        for (u32 i = 0; i < kFattrCount; i++)
            if (need(kFattrBase + i, 3) > 0)
                e.Fattr(i, to.v[kFattrBase + i]);
        for (u32 i = 0; i < kFogCount; i++)
            if (need(kFogBase + i, 1) > 0)
                e.Word2(kFogLutIdx, i, kFogLutData0, to.v[kFogBase + i][0]);
        if (sdKnown)
            e.Reg(kStartDrawFunc0, to.v[kRegBase + kStartDrawFunc0][0], sdBytes);
        return e.overflow ? Fail::EmitOverflow : Fail::None;
    }

    u32 PlanPieces(u32 p0, u32 pH, const u32 *b, u32 nb, Piece *out)
    {
        if (pH < p0 || (p0 & 7) || (pH & 7))
            return 0;
        u32 start = p0;
        for (u32 i = 0; i < nb; i++)
        {
            if ((b[i] & 7) || b[i] < start + 8 || b[i] > pH)
                return 0;
            out[i] = Piece{ start, b[i] - 8, b[i] };
            start = b[i];
        }
        out[nb] = Piece{ start, pH, pH };
        return nb + 1;
    }

    bool GovernorStep(Governor &g, bool frameSeen, bool late, u32 nowMs)
    {
        const u32 win = g.window > 64 ? 64 : (g.window ? g.window : 1);
        if (g.cooling)
        {
            if (static_cast<s32>(nowMs - g.coolUntil) < 0)
                return false;
            g.cooling = false;
            g.bits = 0;
            g.count = 0;
        }
        if (!frameSeen)
            return true;
        g.bits = (g.bits << 1) | (late ? 1u : 0u);
        if (win < 64)
            g.bits &= (u64(1) << win) - 1;
        if (g.count < win)
            g.count++;
        if (static_cast<u32>(__builtin_popcountll(g.bits)) >= g.maxLate)
        {
            g.cooling = true;
            g.coolUntil = nowMs + g.coolMs;
            g.trips++;
            return false;
        }
        return true;
    }

    Show HoldStep(Hold &h, bool replayed, bool pairOnScreen)
    {
        if (replayed)
        {
            h.valid = true;
            h.run = 0;
            return Show::Both;
        }
        if (h.valid && pairOnScreen && h.run < h.maxRun)
        {
            h.run++;
            return Show::Hold;
        }
        HoldReset(h);           // a 2D frame replaces the pair: nothing to hold until the next replay
        return Show::Mono;
    }

    void HoldReset(Hold &h)
    {
        h.valid = false;
        h.run = 0;
    }

    bool HoldGap(Hold &h, u32 gapMs)
    {
        if (gapMs <= kHoldGapMs || !h.valid)
            return false;
        HoldReset(h);
        return true;
    }

    bool EventPush(EventRing &r, const Event &ev)
    {
        const u32 head = r.head;
        if (head - __atomic_load_n(&r.tail, __ATOMIC_ACQUIRE) >= kEventSlots)
        {
            r.dropped = r.dropped + 1;
            return false;
        }
        r.e[head & (kEventSlots - 1)] = ev;
        __atomic_store_n(&r.head, head + 1, __ATOMIC_RELEASE);
        return true;
    }

    bool EventPop(EventRing &r, Event &out)
    {
        const u32 tail = r.tail;
        if (tail == __atomic_load_n(&r.head, __ATOMIC_ACQUIRE))
            return false;
        out = r.e[tail & (kEventSlots - 1)];
        __atomic_store_n(&r.tail, tail + 1, __ATOMIC_RELEASE);
        return true;
    }

    bool LimiterAllow(EventLimiter &l, u32 nowMs)
    {
        if (!l.started || nowMs - l.winStart >= l.windowMs)
        {
            l.started = true;
            l.winStart = nowMs;
            l.inWin = 0;
        }
        if (l.inWin >= l.burst)
        {
            l.suppressed++;
            return false;
        }
        l.inWin++;
        return true;
    }

    float HudScale(float m0, int size)
    {
        const float px = (m0 < 0.0f ? -m0 : m0) * 200.0f;
        if (size <= 0 || !(px >= 0.5f && px <= 16.0f))
            return 1.0f;
        float s;
        if (px >= 1.5f)
        {
            // Two or more screen pixels per GUI unit: one whole pixel less.
            float t = static_cast<float>(static_cast<int>(px));     // floor (px > 0)
            if (t >= px - 0.01f)
                t -= 1.0f;
            s = (t < 1.0f ? 1.0f : t) / px;
        }
        else
            s = 0.8f;           // 1:1 already (1.9.19 top HUD): no smaller exact size
        if (size >= 2)
            s *= 0.8f;
        return s < 1.0f ? s : 1.0f;
    }

    bool AbLatchStep(AbLatch &l, bool want, u32 touched)
    {
        const bool quiet = (touched & (kPadA | kPadB)) == 0;
        if (want != l.active && quiet && l.prevQuiet)
            l.active = want;
        l.prevQuiet = quiet;
        return l.active;
    }

    void ShearXYZW(const float r0[4], const float r3[4], float k, float kc, float out[4])
    {
        for (int i = 0; i < 4; i++)
            out[i] = r0[i] + k * r3[i];
        out[3] -= kc;
    }

    void ShearRow0(const u32 row0[4], const u32 row3[4], float k, float kc, u32 out[4])
    {
        // arrival order w, z, y, x  ->  x, y, z, w
        const float a[4] = { F(row0[3]), F(row0[2]), F(row0[1]), F(row0[0]) };
        const float b[4] = { F(row3[3]), F(row3[2]), F(row3[1]), F(row3[0]) };
        float o[4];
        ShearXYZW(a, b, k, kc, o);
        out[3] = W(o[0]);
        out[2] = W(o[1]);
        out[1] = W(o[2]);
        out[0] = W(o[3]);
    }
}
