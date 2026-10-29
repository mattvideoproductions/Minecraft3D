#pragma once
// PICA200 command-stream shadow for the Comfort 3D same-frame replay (v0.3).
//
// PURE CODE: no 3DS/CTRPF headers, no globals, no syscalls. Everything the
// runtime needs (buffer pointers, mark positions) is passed in, so this file
// can be compiled and unit-tested on a host (plugin/tests/).
//
// What it does (the project's design notes (not in this repo) 5, the project's design notes (not in this repo) 7 #10):
//   1. Parse a GPU command stream word by word and keep a shadow of every
//      piece of restorable GPU state: registers 0x000..0x3FF (byte-enable
//      masks honoured), VSH/GSH float uniforms (as raw words + f32/f24 mode),
//      shader code and operand-descriptor memories, fixed vertex attributes
//      and the fog LUT. Each slot carries a "known" bit: a value that was
//      never written since tracking started is unknown.
//   2. At two marks inside the stream, p0 (start of the top-screen range)
//      and pH (end of it), take full snapshots of the shadow.
//   3. Inside [p0, pH) record every complete 4-row matrix upload to VSH
//      registers c0-c3 (WORLDVIEWPROJ in all 36 matrix shaders) and c8-c11
//      (PROJ in renderchunk*; other programs use c8 for other things, so the
//      caller decides by exact match), with the word addresses of row 0.
//   4. Emit a PROLOGUE that turns the end state back into the p0 state and
//      an EPILOGUE that turns the pH state (+ the patched uniforms) back
//      into the end state, as plain register writes.
//
// Fail-safe policy: anything the parser does not fully understand sets a
// failure reason; the caller then shows that frame without the replay. The
// shadow is never "guessed".
#include <cstdint>
#include <cstddef>

namespace mc3d::pica
{
    using u8 = uint8_t;
    using u16 = uint16_t;
    using u32 = uint32_t;
    using s32 = int32_t;
    using u64 = uint64_t;

    // ---- slot space: one slot = one restorable piece of state ----
    enum : u32
    {
        kRegBase = 0, kRegCount = 0x400,
        kVsfBase = kRegBase + kRegCount, kFloatCount = 96,      // VSH float uniforms c0..c95
        kGsfBase = kVsfBase + kFloatCount,                       // GSH float uniforms
        kVcodeBase = kGsfBase + kFloatCount, kCodeCount = 512,   // VSH program words
        kGcodeBase = kVcodeBase + kCodeCount,                    // GSH program words
        kVopBase = kGcodeBase + kCodeCount, kOpCount = 128,      // VSH operand descriptors
        kGopBase = kVopBase + kOpCount,                          // GSH operand descriptors
        kFattrBase = kGopBase + kOpCount, kFattrCount = 12,      // fixed vertex attributes (3 words)
        kFogBase = kFattrBase + kFattrCount, kFogCount = 128,    // fog LUT
        kSlotCount = kFogBase + kFogCount,
    };

    enum : u8
    {
        kKnown = 1,     // value valid (registers: all four bytes known)
        kF32 = 2,       // float uniform written in f32 mode (4 words: w z y x); else f24 (3 words)
        // Registers only (v0.3 audit B1): bits 4-7 = which BYTES are known.
        // The game writes several registers only with byte masks (0x229,
        // 0x245, 0x253, 0x25E, 0x080, 0x100, ...), so a register can be
        // partly known; it is restored byte by byte with masked writes.
        kByteShift = 4,
    };
    inline u8 RegBytes(u8 f) { return static_cast<u8>(f >> kByteShift); }

    struct State
    {
        u32 v[kSlotCount][4];
        u8  f[kSlotCount];
    };

    // Registers with side effects or FIFO semantics: never re-emitted as
    // plain register state by the prologue/epilogue (their content is
    // restored through the slot memories above, or not at all).
    bool IsNoDeltaReg(u32 id);

    // Why a frame cannot be replayed. Values are stable (they index the
    // fallback counters in the log).
    enum class Fail : u8
    {
        None = 0,
        BadHeader,      // count runs past the end, or a mark falls inside a command
        Jump,           // CMDBUF_JUMP (0x23C/0x23D): stream continues elsewhere
        FinalizeInRange,// more than kMaxFin 0x010 (request boundaries) inside [p0, pH)
        UntrackedLut,   // lighting/proctex/gas LUT written after p0
        PartialMatrix,  // c0-c3 written without a complete 4-row burst in range
        F24Matrix,      // c0-c3 uploaded in f24 mode in range (cannot patch)
        TooManySites,   // more matrix uploads than the site table holds
        PortMask,       // FIFO data write with a partial byte mask
        UnknownAtP0,    // a slot the range needs was never seen before p0
        EmitOverflow,   // prologue/epilogue/copy would not fit the command buffer
        Count
    };
    const char *FailName(Fail f);

    // One complete matrix upload inside [p0, pH).
    struct Site
    {
        u32 *row0[4];   // addresses of row 0's words in arrival order (w, z, y, x)
        u32  raw[4][4]; // all four rows, raw words in arrival order (w, z, y, x)
        u32  pos;       // stream address of the burst's first data word
        u8   base;      // first register: 0 (WVP) or 8 (PROJ candidate)
    };

    struct Parser
    {
        // ---- configuration (set before Run) ----
        const u32 *markP0 = nullptr;    // snapshot S_p0 here, start recording sites
        const u32 *markPH = nullptr;    // snapshot S_pH here, stop recording sites
        State *snapP0 = nullptr;        // destination snapshots (caller-owned)
        State *snapPH = nullptr;
        Site  *sites = nullptr;         // caller-owned site table
        u32    siteCap = 0;

        // ---- results ----
        u32  siteCount = 0;
        u32  wordsParsed = 0;
        bool sawP0 = false, sawPH = false;
        bool afterP0 = false;           // the cursor is past p0 (state changed since the snapshot)
        bool jump = false;              // a command-buffer jump was seen: the shadow is no longer trustworthy
        // v0.3.1: 0x010 finalize commands inside [p0, pH) (the end of a nngx
        // split = a GPU request boundary); the caller cuts the replay there.
        static constexpr u32 kMaxFin = 16;
        u32 *finAt[kMaxFin] = {};
        u32  finCount = 0;
        Fail fail = Fail::None;         // first failure (frame-level)

        // ---- the live shadow ----
        State *cur = nullptr;

        void ResetShadow();             // everything unknown
        void BeginList();               // FIFO counters reset at a list start (nngx never splits a burst)
        // Parse [begin, end). Returns false on a structural error (the
        // shadow is then unreliable and must be reset by the caller).
        bool Run(const u32 *begin, const u32 *end);

    private:
        // FIFO state (not slots)
        struct FloatUnit { u32 idx; bool f32; u32 n; u32 buf[4]; u32 *addr[4]; };
        FloatUnit vsf_ = {}, gsf_ = {};
        u32 vcodeIdx_ = 0, gcodeIdx_ = 0, vopIdx_ = 0, gopIdx_ = 0, fogIdx_ = 0;
        u32 fattrIdx_ = 0, fattrN_ = 0, fattrBuf_[3] = {};
        bool inRange_ = false;
        // matrix group being collected (VSH only, in range)
        u32 grpBase_ = 0, grpFilled_ = 0;
        u32 *grpAddr0_[4] = {};
        u32 grpRaw_[4][4] = {};
        u32 grpPos_ = 0;

        void Write(u32 id, u32 val, u32 mask, u32 *addr);
        void FloatData(FloatUnit &u, u32 slotBase, bool vsh, u32 val, u32 *addr);
        void OnVec4(u32 reg, bool f32, const u32 *words, u32 *const *addrs);
        void GroupBroken();
        bool BurstClearOfMatrices(u32 words) const;
        void SetFail(Fail f) { if (fail == Fail::None) fail = f; }
    };

    // Command emitter with a hard bound. Never writes past `end`.
    struct Emitter
    {
        u32 *p = nullptr, *end = nullptr;
        bool overflow = false;
        bool countOnly = false;     // dry run: only count words, never write
        u32  words = 0;

        // true = write n words at p now; false = skip (counted, or no room)
        bool Room(u32 n)
        {
            if (countOnly) { words += n; return false; }
            if (overflow || !p || !end || end < p || static_cast<size_t>(end - p) < n) { overflow = true; return false; }
            words += n;
            return true;
        }
        void Reg(u32 id, u32 val, u32 mask = 0xF);     // [val][mask<<16|id]
        void FloatVec4(bool gsh, u32 reg, bool f32, const u32 *w);
        void Word2(u32 idxReg, u32 idx, u32 dataReg, u32 val);   // index write + one data word
        void Fattr(u32 idx, const u32 *w);
        void Pad();                                     // keep 8-byte alignment (always already aligned)
    };

    // Emit the state change `from` -> `to` (every slot whose known value in
    // `to` differs from `from`, plus every slot in `force`). Returns Fail::
    // UnknownAtP0 if `to` lacks a value that `from` has (the transition
    // cannot be expressed), Fail::EmitOverflow if it does not fit.
    // `force` may be null; it is a per-slot byte array (non-zero = rewrite).
    Fail EmitTransition(const State &from, const State &to, const u8 *force, Emitter &e);

    // Projection shear on one 4-word row-0 in arrival order (w, z, y, x),
    // with row 3 the same way: row0 += k*row3; row0.w -= kc. Pure float math
    // (design 2.2): clip x' = x + k*w - kc.
    void ShearRow0(const u32 row0[4], const u32 row3[4], float k, float kc, u32 out[4]);

    // Same, plain floats in x,y,z,w order (for tests and the refresh hook).
    void ShearXYZW(const float r0[4], const float r3[4], float k, float kc, float out[4]);

    // v0.3.1 (delta audit D-B1): the right-eye copy of [p0, pH) is cut at
    // the game's split boundaries b[] (each segment ends with the split's
    // 8-byte 0x010 finalize command). A piece copies [start, copyEnd):
    // up to b - 8 for every boundary, so the game's finalize is NEVER copied
    // (our own split writes a real one); the last piece ends at pH.
    // Returns the number of pieces (nb + 1), or 0 if the boundaries are not
    // strictly increasing, 8-aligned and inside (p0 + 8, pH].
    struct Piece { u32 start, copyEnd, end; };
    u32 PlanPieces(u32 p0, u32 pH, const u32 *b, u32 nb, Piece *out);

    // v0.3.1 (D-S1): Comfort governor. Each paced frame reports whether it
    // missed its 2-vblank slot. If `maxLate` of the last `window` frames
    // were late, Comfort is suspended for `coolMs` (the caller shows the
    // flat plane, unpaced), then retried with a clean window.
    struct Governor
    {
        u32  window = 60, maxLate = 6, coolMs = 10000;
        u64  bits = 0;          // last `window` (<= 64) frames, 1 = late
        u32  count = 0;         // frames in the window so far
        bool cooling = false;
        u32  coolUntil = 0;
        u32  trips = 0;
    };
    // Returns true if Comfort may run this frame. `nowMs` wraps safely.
    bool GovernorStep(Governor &g, bool frameSeen, bool late, u32 nowMs);

    // v0.3.1: what a Comfort frame shows. A frame whose replay failed keeps
    // the previous stereo pair on screen (one repeated picture, depth kept)
    // for up to `maxRun` frames in a row; only after that, or with no fresh
    // pair to keep, is it shown 2D (left picture in both eyes). A frame
    // that is not a Comfort frame calls HoldReset (the pair is then stale).
    enum class Show : u8 { Both, Hold, Mono };
    struct Hold
    {
        u32  maxRun = 3;        // ~100-150 ms at 20-30 fps; cfg hold=N, 0 = v0.3.0 (always 2D)
        u32  run = 0;           // held frames in a row
        bool valid = false;     // the last shown pair is a Comfort pair of the previous frame(s)
    };
    // replayed = this frame produced a fresh pair; pairOnScreen = the last
    // pair is still the newest in both eyes' slot rotation.
    Show HoldStep(Hold &h, bool replayed, bool pairOnScreen);
    void HoldReset(Hold &h);
    // v0.3.2 (audit v0.3.1 N3): a gap of more than kHoldGapMs between two
    // presented frames (HOME menu, sleep, a long load) makes the last pair
    // stale, so the first failed frames after it are shown 2D, never the
    // picture from before the gap. Returns true if it reset.
    constexpr u32 kHoldGapMs = 200;
    bool HoldGap(Hold &h, u32 gapMs);

    // v0.3.2: per-switch event log. The game main thread pushes one record
    // per change of what the top screen shows (and per render-distance
    // change); the plugin thread pops them and writes the file at most once
    // per second. Single producer, single consumer; when full, new records
    // are dropped and counted. No allocation, no locks.
    struct Event
    {
        u32 ms;                 // system clock, ms
        u8  kind, from, to, why;
        u16 frameMs10;          // last frame interval, 0.1 ms
        u16 fps10;              // smoothed fps x10
        s32 rd;                 // render distance drawn (chunks), -1 unknown
    };
    constexpr u32 kEventSlots = 64;     // power of two
    struct EventRing
    {
        Event e[kEventSlots];
        volatile u32 head = 0;  // producer only
        volatile u32 tail = 0;  // consumer only
        volatile u32 dropped = 0;
    };
    bool EventPush(EventRing &r, const Event &ev);  // false (and counted) when full
    bool EventPop(EventRing &r, Event &out);

    // Flapping limit: at most `burst` events per `windowMs`; the rest are
    // counted in `suppressed` (reported with the next line that gets out).
    struct EventLimiter
    {
        u32  burst = 12, windowMs = 1000;
        u32  winStart = 0, inWin = 0, suppressed = 0;
        bool started = false;
    };
    bool LimiterAllow(EventLimiter &l, u32 nowMs);

    // v0.3.2 HUD size: factor applied to a top-screen GUI ortho projection
    // whose x scale is m0 (NDC per GUI unit; the screen is 400 px = 2 NDC,
    // so px per unit = |m0| * 200). size 0 = Normal (1.0). 1 = Small: with
    // 2+ screen pixels per GUI unit, one whole pixel less (pixel-exact);
    // at about 1 pixel per unit (the 1.9.19 top HUD: 10-px font mc_10,
    // 9-px crosshair) there is no smaller exact size, so 0.8. 2 = Smaller:
    // 0.8 x Small. Returns 1.0 for an unexpected matrix (nothing changed).
    float HudScale(float m0, int size);

    // v0.3.3 A/B swap (input_swap.cpp). The game reads the pad through the
    // SDK ExtraPadReader (ReadLatest + Read); its outputs carry hold, trigger
    // and release words where bit 0 = A, bit 1 = B. SwapAB exchanges exactly
    // those two bits (nothing else: Select, ZL/ZR, C-stick bits untouched).
    constexpr u32 kPadA = 1u, kPadB = 2u;
    inline u32 SwapAB(u32 b) { return (b & ~(kPadA | kPadB)) | ((b & kPadA) << 1) | ((b & kPadB) >> 1); }
    // The swap in effect only follows the menu toggle while A and B are
    // untouched (no hold/trigger/release of either) in this read AND the
    // read before it (either reader), so a press that began under one
    // mapping always ends under the same one: nothing sticks when the
    // toggle flips mid-press. `touched` = OR of hold|trig|rel over every
    // sample the read returned (raw); call only for reads that returned
    // at least one sample. Returns whether to swap this read's output.
    struct AbLatch
    {
        bool active = false;
        bool prevQuiet = true;
    };
    bool AbLatchStep(AbLatch &l, bool want, u32 touched);
}
