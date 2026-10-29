// Host unit test for the pure Comfort 3D stream code (pica_shadow.cpp) and
// the pure world-option helpers. No 3DS code is involved.
//
// Build + run on any desktop compiler (this PC has none installed; the
// Python model test_c3_model.py runs the same scenarios today):
//   g++ -std=c++20 -O1 -Wall -Wextra -I../include ../source/pica_shadow.cpp test_pica_shadow.cpp -o t && ./t
// It is also compile-checked with devkitARM (-fsyntax-only) by tests/run_tests.ps1.
#include "pica_shadow.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace mc3d::pica;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

// ---- stream builder (same formats as gd 0x4B76B8 and nngx 0x7C5518) ----
struct Stream
{
    std::vector<u32> w;
    void Reg(u32 id, u32 v, u32 mask = 0xF) { w.push_back(v); w.push_back((mask << 16) | id); }
    void Seq(u32 id, const std::vector<u32> &vals)        // consecutive registers
    {
        w.push_back(vals[0]);
        w.push_back(0x80000000u | (static_cast<u32>(vals.size() - 1) << 20) | 0xF0000u | id);
        for (size_t i = 1; i < vals.size(); i++) w.push_back(vals[i]);
        if (w.size() & 1) w.push_back(0);
    }
    void Burst(u32 reg, const std::vector<u32> &data)     // f32 uniforms: w z y x per vec4
    {
        Reg(0x2C0, 0x80000000u | reg);
        w.push_back(data[0]);
        w.push_back((static_cast<u32>(data.size() - 1) << 20) | 0xF0000u | 0x2C1);
        for (size_t i = 1; i < data.size(); i++) w.push_back(data[i]);
        if (w.size() & 1) w.push_back(0);
    }
    size_t Pos() const { return w.size(); }
};

static u32 FW(float f) { u32 x; std::memcpy(&x, &f, 4); return x; }
static float WF(u32 x) { float f; std::memcpy(&f, &x, 4); return f; }

// Matrix rows (x,y,z,w) -> burst words (w,z,y,x per row)
static std::vector<u32> MatWords(const float rows[4][4])
{
    std::vector<u32> d;
    for (int r = 0; r < 4; r++)
        for (int c = 3; c >= 0; c--)
            d.push_back(FW(rows[r][c]));
    return d;
}

// ---- an independent GPU model: what a stream does to registers/uniforms ----
struct Gpu
{
    u32 reg[0x400] = {};
    u32 fl[96][4] = {};
    u32 code[512] = {};
    u32 fidx = 0, fn = 0, fbuf[4] = {}; bool f32 = true;
    u32 cidx = 0;
    std::vector<int> drawsSeen;     // value of c0.x (as bits) at every draw kick (0x22E)
    std::vector<u32> drawC0;
    void Run(const u32 *p, const u32 *e)
    {
        while (p < e)
        {
            const u32 h = p[1], id = h & 0xFFFF, m = (h >> 16) & 0xF, n = (h >> 20) & 0xFF;
            const bool seq = h >> 31;
            const u32 total = (2 + n + 1) & ~1u;
            for (u32 i = 0; i <= n; i++)
            {
                const u32 r = seq ? id + i : id, v = i == 0 ? p[0] : p[1 + i];
                Write(r, v, m);
            }
            p += total;
        }
    }
    void Write(u32 r, u32 v, u32 m)
    {
        u32 bm = 0;
        for (int b = 0; b < 4; b++) if (m & (1u << b)) bm |= 0xFFu << (8 * b);
        reg[r] = (reg[r] & ~bm) | (v & bm);
        if (r == 0x2C0) { fidx = v & 0x7F; f32 = v >> 31; fn = 0; }
        else if (r >= 0x2C1 && r <= 0x2C8)
        {
            fbuf[fn++] = v;
            if (fn == (f32 ? 4u : 3u)) { for (u32 i = 0; i < 4; i++) fl[fidx][i] = i < fn ? fbuf[i] : 0; fidx++; fn = 0; }
        }
        else if (r == 0x2CB) cidx = v & 0xFFF;
        else if (r >= 0x2CC && r <= 0x2D3) { if (cidx < 512) code[cidx] = v; cidx++; }
        else if (r == 0x22E) drawC0.push_back(fl[0][3]);   // x of row 0
    }
};

static bool SameTracked(const Gpu &a, const Gpu &b)
{
    for (u32 r = 0; r < 0x400; r++)
        if (!IsNoDeltaReg(r) && r != 0x245 && a.reg[r] != b.reg[r]) return false;
    return std::memcmp(a.fl, b.fl, sizeof(a.fl)) == 0 && std::memcmp(a.code, b.code, sizeof(a.code)) == 0;
}

static State s_cur, s_p0, s_pH;
static Site  s_sites[64];

static void TestReplayIdentity()
{
    const float wvpA[4][4] = { { 1.2f, 0, 0.1f, 2 }, { 0, 2, 0, 1 }, { 0, 0, -1, 3 }, { 0.3f, 0.1f, -0.9f, 5 } };
    const float wvpB[4][4] = { { 0.9f, 0, 0, 1 }, { 0, 1.5f, 0, 0 }, { 0, 0, -1, 1 }, { 0, 0, -1, 4 } };
    const float ortho[4][4] = { { 0.01f, 0, 0, -1 }, { 0, 0.016f, 0, -1 }, { 0, 0, 1, 0 }, { 0, 0, 0, 1 } };
    Stream s;
    // before p0: everything the range relies on is written (previous frame state)
    for (u32 r = 0x40; r < 0x50; r++) s.Reg(r, 0x1000 + r);
    s.Reg(0x11D, 0xAAAA0000);           // top colour buffer
    s.Burst(0, MatWords(ortho));
    s.Reg(0x2CB, 0); s.Reg(0x2CC, 0x11111111); s.Reg(0x2CC, 0x22222222);
    const size_t p0 = s.Pos();
    // [p0, pH): top screen
    s.Burst(0, MatWords(wvpA)); s.Reg(0x22E, 1);
    s.Reg(0x41, 0x7777);
    s.Burst(0, MatWords(wvpB)); s.Reg(0x22E, 1);
    s.Reg(0x2CB, 1); s.Reg(0x2CC, 0x33333333);
    const size_t pH = s.Pos();
    // [pH, end): bottom screen changes target, viewport, matrix, code
    s.Reg(0x11D, 0xBBBB0000);
    s.Reg(0x42, 0x9999);
    s.Burst(0, MatWords(ortho)); s.Reg(0x22E, 1);
    s.Reg(0x2CB, 0); s.Reg(0x2CC, 0x44444444);
    s.Reg(0x25E, 0, 0x8);               // nngx split pad (partial mask)
    const size_t end = s.Pos();

    Parser ps;
    ps.cur = &s_cur;
    ps.ResetShadow();
    ps.markP0 = &s.w[p0];
    ps.markPH = &s.w[pH];
    ps.snapP0 = &s_p0; ps.snapPH = &s_pH;
    ps.sites = s_sites; ps.siteCap = 64;
    CHECK(ps.Run(s.w.data(), s.w.data() + end));
    CHECK(ps.fail == Fail::None);
    CHECK(ps.siteCount == 2);
    CHECK(s_sites[0].base == 0 && s_sites[0].row0[3] == &s.w[p0 + 2]);   // x of row 0 = 4th data word

    // right eye: shear both sites, force c0 in the epilogue
    static u8 force[kSlotCount];
    std::memset(force, 0, sizeof(force));
    std::vector<u32> out(4096);
    Emitter e; e.p = out.data(); e.end = out.data() + out.size();
    CHECK(EmitTransition(s_cur, s_p0, nullptr, e) == Fail::None);
    const size_t pro = e.p - out.data();
    std::vector<u32> copy(s.w.begin() + p0, s.w.begin() + pH);
    for (u32 i = 0; i < ps.siteCount; i++)
    {
        u32 r[4];
        ShearRow0(s_sites[i].raw[0], s_sites[i].raw[3], 0.05f, 0.05f * 0.4f, r);
        for (int j = 0; j < 4; j++) copy[s_sites[i].row0[j] - &s.w[p0]] = r[j];
        force[kVsfBase + 0] = 1;
    }
    for (u32 x : copy) *e.p++ = x;
    CHECK(EmitTransition(s_pH, s_cur, force, e) == Fail::None);
    CHECK(!e.overflow);
    const size_t total = e.p - out.data();

    // GPU: run the original list, then prologue | copy | epilogue
    Gpu g, ref0, refH;
    g.Run(s.w.data(), s.w.data() + end);
    ref0.Run(s.w.data(), s.w.data() + p0);
    refH.Run(s.w.data(), s.w.data() + pH);
    const Gpu endState = g;
    g.drawC0.clear();
    g.Run(out.data(), out.data() + pro);
    CHECK(SameTracked(g, ref0));                    // prologue reproduces S_p0
    g.Run(out.data() + pro, out.data() + pro + copy.size());
    CHECK(g.drawC0.size() == 2);                    // both draws replayed
    CHECK(g.drawC0.size() == 2 && WF(g.drawC0[0]) != 1.2f);   // with the right-eye row 0
    g.Run(out.data() + pro + copy.size(), out.data() + total);
    CHECK(SameTracked(g, endState));                // epilogue restores the end state
}

static void TestUnknownAndPartial()
{
    // a register only written after p0 has no p0 value: not replayable
    Stream s;
    const size_t p0 = s.Pos();
    s.Reg(0x41, 5);
    const size_t pH = s.Pos();
    Parser ps; ps.cur = &s_cur; ps.ResetShadow();
    ps.markP0 = &s.w[p0]; ps.markPH = &s.w[pH]; ps.snapP0 = &s_p0; ps.snapPH = &s_pH;
    CHECK(ps.Run(s.w.data(), s.w.data() + s.Pos()));
    Emitter e; e.countOnly = true;
    CHECK(EmitTransition(s_cur, s_p0, nullptr, e) == Fail::UnknownAtP0);

    // c1 without c0 inside the range: partial matrix
    Stream t;
    t.Reg(0x41, 1);
    const size_t q0 = t.Pos();
    t.Burst(1, std::vector<u32>(4, FW(1.0f)));
    const size_t qH = t.Pos();
    Parser pt; pt.cur = &s_cur; pt.ResetShadow();
    pt.markP0 = &t.w[q0]; pt.markPH = &t.w[qH]; pt.snapP0 = &s_p0; pt.snapPH = &s_pH;
    pt.sites = s_sites; pt.siteCap = 64;
    CHECK(pt.Run(t.w.data(), t.w.data() + t.Pos()));
    CHECK(pt.fail == Fail::PartialMatrix);

    // finalize inside the range
    Stream u;
    const size_t u0 = u.Pos();
    u.Reg(0x010, 0x12345678);
    const size_t uH = u.Pos();
    Parser pu; pu.cur = &s_cur; pu.ResetShadow();
    pu.markP0 = &u.w[u0]; pu.markPH = &u.w[uH];
    CHECK(pu.Run(u.w.data(), u.w.data() + u.Pos()));
    CHECK(pu.fail == Fail::None && pu.finCount == 1 && pu.finAt[0] == &u.w[u0]);   // v0.3.1: recorded, not a failure

    // audit B1: masked-only register, byte 1 unknown at p0 but written after it
    Stream m;
    m.Reg(0x229, 0x11, 0x1);
    const size_t m0 = m.Pos();
    m.Reg(0x229, 0x2200, 0x2);
    Parser pm; pm.cur = &s_cur; pm.ResetShadow();
    pm.markP0 = &m.w[m0]; pm.snapP0 = &s_p0;
    CHECK(pm.Run(m.w.data(), m.w.data() + m.Pos()));
    Emitter em; em.countOnly = true;
    CHECK(EmitTransition(s_cur, s_p0, nullptr, em) == Fail::UnknownAtP0);
    // ... and byte 0 changed after p0 is restored with a mask-1 write
    Stream n;
    n.Reg(0x229, 0x11, 0x1);
    const size_t n0 = n.Pos();
    n.Reg(0x229, 0x33, 0x1);
    Parser pn; pn.cur = &s_cur; pn.ResetShadow();
    pn.markP0 = &n.w[n0]; pn.snapP0 = &s_p0;
    CHECK(pn.Run(n.w.data(), n.w.data() + n.Pos()));
    std::vector<u32> o(256);
    Emitter en; en.p = o.data(); en.end = o.data() + o.size();
    CHECK(EmitTransition(s_cur, s_p0, nullptr, en) == Fail::None);
    bool found = false;
    for (u32 *q = o.data(); q < en.p; q += 2)
        found |= q[0] == 0x11 && q[1] == ((1u << 16) | 0x229);
    CHECK(found);

    // a mark inside a command = out of step
    Stream v;
    v.Seq(0x41, { 1, 2, 3 });
    Parser pv; pv.cur = &s_cur; pv.ResetShadow();
    pv.markP0 = &v.w[2];
    CHECK(!pv.Run(v.w.data(), v.w.data() + v.Pos()));
}

static void TestShear()
{
    // disparity of a point at view depth D: NDC x difference * 200 px
    const float s = 1.0f, px = 18.0f, cap = 4.0f, C = 0.3f * (1.0f + cap / px);
    const float a = s * px / 200.0f;
    for (float D : { 0.3f, 1.0f, 4.0f, 64.0f })
    {
        const float r0[4] = { 0.86f, 0, 0, 0 }, r3[4] = { 0, 0, -1, 0 };
        const float v[4] = { 0.0f, 0.0f, -D, 1.0f };      // on the view axis
        float L[4], R[4];
        ShearXYZW(r0, r3, -0.5f * a, -0.5f * a * C, L);
        ShearXYZW(r0, r3, 0.5f * a, 0.5f * a * C, R);
        auto dot = [&](const float *m) { return m[0] * v[0] + m[1] * v[1] + m[2] * v[2] + m[3] * v[3]; };
        const float w = D;
        const float d = (dot(R) - dot(L)) / w * 200.0f;
        const float expect = s * px * (1.0f - C / D);
        CHECK(std::fabs(d - expect) < 1e-3f);
    }
}


static void TestPiecesAndGovernor()
{
    // D-B1: two split boundaries; no piece may include the 8-byte finalize
    Piece pc[4];
    const u32 b[2] = { 0x1040, 0x1100 };
    CHECK(PlanPieces(0x1000, 0x1200, b, 2, pc) == 3);
    CHECK(pc[0].start == 0x1000 && pc[0].copyEnd == 0x1038 && pc[0].end == 0x1040);
    CHECK(pc[1].start == 0x1040 && pc[1].copyEnd == 0x10F8);
    CHECK(pc[2].start == 0x1100 && pc[2].copyEnd == 0x1200);
    const u32 bad[2] = { 0x1100, 0x1040 };
    CHECK(PlanPieces(0x1000, 0x1200, bad, 2, pc) == 0);
    const u32 out[1] = { 0x1208 };
    CHECK(PlanPieces(0x1000, 0x1200, out, 1, pc) == 0);
    // D-S1 governor
    Governor g;
    u32 t = 0;
    bool ok = true;
    for (int i = 0; i < 600; i++, t += 33)
        ok &= GovernorStep(g, true, i % 20 == 0, t);
    CHECK(ok && g.trips == 0);
    Governor h;
    int tripAt = -1;
    for (int i = 0; i < 40 && tripAt < 0; i++)
        if (!GovernorStep(h, true, i % 5 == 0, i * 33))
            tripAt = i;
    CHECK(tripAt >= 0 && h.trips == 1);
    CHECK(!GovernorStep(h, false, false, 40 * 33));
    CHECK(GovernorStep(h, false, false, tripAt * 33 + 10001));
}

// v0.3.1: a failed Comfort frame keeps the previous pair (at most maxRun in a row)
static void TestHold()
{
    Hold h;
    CHECK(HoldStep(h, false, true) == Show::Mono);          // nothing to hold yet
    CHECK(HoldStep(h, true, true) == Show::Both);
    CHECK(HoldStep(h, false, true) == Show::Hold);          // isolated failure: no 2D frame
    CHECK(HoldStep(h, true, true) == Show::Both);
    for (int i = 0; i < 3; i++)
        CHECK(HoldStep(h, false, true) == Show::Hold);
    CHECK(HoldStep(h, false, true) == Show::Mono);          // 4th in a row: 2D
    CHECK(HoldStep(h, false, true) == Show::Mono);          // and stays 2D until a replay
    CHECK(HoldStep(h, true, true) == Show::Both);
    CHECK(HoldStep(h, false, false) == Show::Mono);         // pair no longer the newest slot
    CHECK(HoldStep(h, true, true) == Show::Both);
    HoldReset(h);                                           // non-Comfort frame in between
    CHECK(HoldStep(h, false, true) == Show::Mono);
    Hold z;
    z.maxRun = 0;                                           // hold=0: v0.3.0 behaviour
    CHECK(HoldStep(z, true, true) == Show::Both);
    CHECK(HoldStep(z, false, true) == Show::Mono);
}

// v0.3.2: HOME/sleep gap, event ring, flapping limit, HUD scale
static void TestV032()
{
    // a gap > 200 ms drops the held pair: the next failed frame is 2D, not the old picture
    Hold h;
    CHECK(HoldStep(h, true, true) == Show::Both);
    CHECK(!HoldGap(h, 34));                                 // a normal (slow) frame keeps it
    CHECK(HoldStep(h, false, true) == Show::Hold);
    CHECK(HoldStep(h, true, true) == Show::Both);
    CHECK(!HoldGap(h, kHoldGapMs));
    CHECK(HoldGap(h, 5000));                                // HOME / lid closed
    CHECK(HoldStep(h, false, true) == Show::Mono);
    CHECK(!HoldGap(h, 5000));                               // nothing left to reset

    // ring: FIFO, full = dropped and counted, wraps
    static EventRing r;
    Event e = {}, o = {};
    for (u32 i = 0; i < kEventSlots; i++)
    {
        e.ms = i;
        CHECK(EventPush(r, e));
    }
    e.ms = 999;
    CHECK(!EventPush(r, e) && r.dropped == 1);
    for (u32 i = 0; i < kEventSlots; i++)
        CHECK(EventPop(r, o) && o.ms == i);
    CHECK(!EventPop(r, o));
    for (u32 i = 0; i < 1000; i++)                          // many wraps, one in one out
    {
        e.ms = 5000 + i;
        CHECK(EventPush(r, e) && EventPop(r, o) && o.ms == 5000 + i);
    }
    CHECK(r.dropped == 1);

    // limiter: per-frame flapping at 30 fps for 10 s -> at most 12 lines per second
    EventLimiter l;
    u32 allowed = 0;
    for (u32 f = 0; f < 300; f++)
        allowed += LimiterAllow(l, 100000 + f * 33) ? 1 : 0;
    CHECK(allowed <= 12 * 11 && allowed >= 12 * 9);
    CHECK(l.suppressed == 300 - allowed);
    EventLimiter q;                                         // isolated events always pass
    for (u32 i = 0; i < 20; i++)
        CHECK(LimiterAllow(q, 0xFFFFF000u + i * 3000));     // across the u32 ms wrap too
    CHECK(q.suppressed == 0);

    // HUD scale
    const float m1 = 1.0f / 200.0f;                         // 1 px per unit (1.9.19 top HUD)
    CHECK(HudScale(m1, 0) == 1.0f);
    CHECK(std::fabs(HudScale(m1, 1) - 0.8f) < 1e-5f);
    CHECK(std::fabs(HudScale(-m1, 1) - 0.8f) < 1e-5f);     // sign-agnostic
    CHECK(std::fabs(HudScale(m1, 2) - 0.64f) < 1e-5f);
    CHECK(std::fabs(HudScale(2.0f / 200.0f, 1) - 0.5f) < 1e-5f);           // 2 px -> 1 px (exact)
    CHECK(std::fabs(HudScale(3.0f / 200.0f, 1) - 2.0f / 3.0f) < 1e-5f);    // 3 px -> 2 px
    CHECK(std::fabs(HudScale(2.5f / 200.0f, 1) * 2.5f - 2.0f) < 1e-4f);    // 2.5 px -> 2 px
    CHECK(HudScale(0.0f, 1) == 1.0f);                       // degenerate / unexpected matrices
    CHECK(HudScale(1.0f, 1) == 1.0f);                       // 200 px per unit: not a GUI
}

// v0.3.3: A/B swap bits and the latch (the full reader model is in test_c3_model.py test_ab_swap)
static void TestV033()
{
    CHECK(SwapAB(0) == 0);
    CHECK(SwapAB(kPadA) == kPadB && SwapAB(kPadB) == kPadA);
    CHECK(SwapAB(kPadA | kPadB) == (kPadA | kPadB));
    CHECK(SwapAB(0xFFFFFFFCu) == 0xFFFFFFFCu);              // Select, ZL/ZR, C-stick, ... untouched
    CHECK(SwapAB(0x4004u | kPadA) == (0x4004u | kPadB));
    for (u32 b = 0; b < 4096; b++)
        CHECK(SwapAB(SwapAB(b)) == b);

    AbLatch l;
    CHECK(!AbLatchStep(l, false, 0));
    CHECK(!AbLatchStep(l, true, kPadA));                    // A held: the switch waits
    CHECK(!AbLatchStep(l, true, kPadA));
    CHECK(!AbLatchStep(l, true, 0));                        // released, but the read before was not quiet
    CHECK(AbLatchStep(l, true, 0));                         // two quiet reads: now swapped
    CHECK(AbLatchStep(l, true, 0x4000u));                   // other buttons never block
    CHECK(AbLatchStep(l, false, kPadB));                    // mid-press switch off: still swapped
    CHECK(AbLatchStep(l, false, 0));                        // previous read touched B
    CHECK(!AbLatchStep(l, false, 0));
}

int main()
{
    TestPiecesAndGovernor();
    TestHold();
    TestV032();
    TestV033();
    TestReplayIdentity();
    TestUnknownAndPartial();
    TestShear();
    std::printf(g_fail ? "%d FAILED\n" : "all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
