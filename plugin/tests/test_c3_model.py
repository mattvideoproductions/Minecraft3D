"""Host tests for the v0.3 pure logic, as a line-by-line Python model.

There is no desktop C/C++ compiler on the build PC, so the C++ host test
(test_pica_shadow.cpp) cannot run here yet. This file models the same pure
functions 1:1 and runs the same scenarios plus randomized ones:

  - pica_shadow.cpp: parse / snapshots / sites / EmitTransition, checked
    against an independent GPU model (replay identity: prologue reproduces
    S_p0, the copy draws with the right-eye matrix, the epilogue restores the
    end state) on 2000 random streams;
  - the projection shear (disparity = s*px*(1 - C/D), comfort cap, hand class);
  - world_opts.cpp Auto3DChunks and HystStep (5 s / 20 s rules);
  - the 30 fps pacer cadence (comfort.cpp BeforeRunCmdlist) on a simulated
    vblank clock.

Run:  python -I test_c3_model.py
"""
import math
import random
import struct
import sys

# --------------------------------------------------------------- PICA model
REG_COUNT = 0x400
VSF, GSF, VCODE, GCODE, VOP, GOP, FATTR, FOG = range(8)
SIZES = {VSF: 96, GSF: 96, VCODE: 512, GCODE: 512, VOP: 128, GOP: 128, FATTR: 12, FOG: 128}
VSH, GSH = 0x2B0, 0x280
OFF_CODE_END, OFF_F_IDX, OFF_F0, OFF_F7 = 0x0F, 0x10, 0x11, 0x18
OFF_C_IDX, OFF_C0, OFF_C7, OFF_O_IDX, OFF_O0, OFF_O7 = 0x1B, 0x1C, 0x23, 0x25, 0x26, 0x2D


def inr(x, a, b):
    return a <= x <= b


def no_delta(i):
    if i <= 0x010 or i in (0x063, 0x110, 0x111):
        return True
    if inr(i, 0x0AF, 0x0B7) or i == 0x0E6 or inr(i, 0x0E8, 0x0EF):
        return True
    if i in (0x123, 0x124, 0x1C5) or inr(i, 0x1C8, 0x1CF):
        return True
    if i in (0x22E, 0x22F, 0x231) or inr(i, 0x232, 0x235) or i in (0x23C, 0x23D, 0x25F):
        return True
    for b in (GSH, VSH):
        if i == b + OFF_CODE_END or inr(i, b + OFF_F_IDX, b + OFF_F7) or inr(i, b + OFF_C_IDX, b + OFF_C7) \
                or inr(i, b + OFF_O_IDX, b + OFF_O7):
            return True
    return False


class State:
    def __init__(self):
        self.reg = [0] * REG_COUNT                 # value (unknown bytes: don't care)
        self.rbk = [0] * REG_COUNT                 # known-byte mask per register (audit B1)
        self.mem = {k: [None] * n for k, n in SIZES.items()}   # VSF/GSF entries: (f32, tuple4)

    def copy(self):
        s = State()
        s.reg = list(self.reg)
        s.rbk = list(self.rbk)
        s.mem = {k: list(v) for k, v in self.mem.items()}
        return s


def mask_bytes(m):
    return sum(0xFF << (8 * b) for b in range(4) if m & (1 << b))


class Parser:
    """Mirror of mc3d::pica::Parser."""

    def __init__(self, cur):
        self.cur = cur
        self.begin_list()
        self.jump = False

    def begin_list(self):
        self.fu = {VSH: [0, True, []], GSH: [0, True, []]}   # idx, f32, pending [(val, addr)]
        self.cidx = {VSH: 0, GSH: 0}
        self.oidx = {VSH: 0, GSH: 0}
        self.fattr_idx, self.fattr_buf, self.fog_idx = 0, [], 0
        self.in_range = False
        self.grp = None

    def setup(self, p0, ph):
        self.p0, self.ph = p0, ph
        self.saw_p0 = self.saw_ph = self.after_p0 = False
        self.fail = None
        self.sites = []
        self.fin = []
        self.snap_p0 = self.snap_ph = None

    def set_fail(self, f):
        if self.fail is None:
            self.fail = f

    def group_broken(self):
        if self.grp and self.grp["filled"] and self.grp["base"] == 0:
            self.set_fail("partial")
        self.grp = None

    def on_vec4(self, reg, f32, words, addrs):
        wvp, proj = reg <= 3, 8 <= reg <= 11
        if not wvp and not proj:
            if self.grp:
                self.group_broken()
            return
        if not f32:
            if self.grp:
                self.group_broken()
            if wvp:
                self.set_fail("f24")
            return
        if reg in (0, 8):
            if self.grp:
                self.group_broken()
            self.grp = {"base": reg, "filled": 0, "rows": [], "addr0": None}
        elif not (self.grp and reg == self.grp["base"] + self.grp["filled"]):
            if self.grp:
                self.group_broken()
            if wvp:
                self.set_fail("partial")
            return
        self.grp["rows"].append(list(words))
        if self.grp["filled"] == 0:
            self.grp["addr0"] = list(addrs)
        self.grp["filled"] += 1
        if self.grp["filled"] == 4:
            self.sites.append({"base": self.grp["base"], "rows": self.grp["rows"], "row0": self.grp["addr0"]})
            self.grp = None

    def write(self, i, v, m, addr):
        c = self.cur
        if i == 0x010 and self.in_range:
            self.fin.append(addr)                   # v0.3.1: request boundary, cut there
        if i in (0x23C, 0x23D):
            self.jump = True
            self.set_fail("jump")
        if self.after_p0 and (inr(i, 0x0B0, 0x0B7) or i == 0x124 or inr(i, 0x1C8, 0x1CF)):
            self.set_fail("lut")
        if m:
            b = mask_bytes(m)
            c.reg[i] = (c.reg[i] & ~b & 0xFFFFFFFF) | (v & b)
            c.rbk[i] |= m
        for b, kind_f, kind_c, kind_o in ((VSH, VSF, VCODE, VOP), (GSH, GSF, GCODE, GOP)):
            fu = self.fu[b]
            if i == b + OFF_F_IDX:
                if b == VSH and self.in_range and self.grp:
                    self.group_broken()
                val = c.reg[i]
                fu[0], fu[1], fu[2] = val & 0x7F, (val >> 31) != 0, []
                return
            if inr(i, b + OFF_F0, b + OFF_F7):
                fu[2].append((v, addr))
                need = 4 if fu[1] else 3
                if len(fu[2]) >= need:
                    reg = fu[0]
                    words = [x for x, _ in fu[2]]
                    if reg < 96:
                        c.mem[kind_f][reg] = (fu[1], tuple(words + [0] * (4 - len(words))))
                        if b == VSH and self.in_range:
                            self.on_vec4(reg, fu[1], words, [a for _, a in fu[2]])
                    fu[0] = (fu[0] + 1) & 0x7F
                    fu[2] = []
                return
            if i == b + OFF_C_IDX:
                self.cidx[b] = c.reg[i] & 0xFFF
                return
            if inr(i, b + OFF_C0, b + OFF_C7):
                if self.cidx[b] < 512:
                    c.mem[kind_c][self.cidx[b]] = v
                self.cidx[b] += 1
                return
            if i == b + OFF_O_IDX:
                self.oidx[b] = c.reg[i] & 0xFFF
                return
            if inr(i, b + OFF_O0, b + OFF_O7):
                if self.oidx[b] < 128:
                    c.mem[kind_o][self.oidx[b]] = v
                self.oidx[b] += 1
                return
        if i == 0x232:
            self.fattr_idx, self.fattr_buf = c.reg[i] & 0xF, []
            return
        if inr(i, 0x233, 0x235):
            self.fattr_buf.append(v)
            if len(self.fattr_buf) >= 3:
                if self.fattr_idx < 12:
                    c.mem[FATTR][self.fattr_idx] = tuple(self.fattr_buf[:3])
                    self.fattr_idx += 1
                self.fattr_buf = []
            return
        if i == 0x0E6:
            self.fog_idx = c.reg[i] & 0x7F
            return
        if inr(i, 0x0E8, 0x0EF):
            c.mem[FOG][self.fog_idx & 0x7F] = v
            self.fog_idx = (self.fog_idx + 1) & 0x7F

    def run(self, words, begin, end):
        p = begin
        while True:
            if self.p0 is not None and p == self.p0 and not self.saw_p0:
                self.snap_p0 = self.cur.copy()
                self.saw_p0 = self.after_p0 = self.in_range = True
                self.grp = None
            if self.ph is not None and p == self.ph and not self.saw_ph:
                if self.in_range and self.grp:
                    self.group_broken()
                self.snap_ph = self.cur.copy()
                self.saw_ph, self.in_range = True, False
            if p >= end:
                return True
            hdr = words[p + 1]
            idf, m, n, seq = hdr & 0xFFFF, (hdr >> 16) & 0xF, (hdr >> 20) & 0xFF, hdr >> 31
            total = (2 + n + 1) & ~1
            if idf >= REG_COUNT or end - p < total:
                self.set_fail("header")
                return False
            for mk in (self.p0, self.ph):
                if mk is not None and p < mk < p + total:
                    self.set_fail("header")
                    return False
            for k in range(n + 1):
                a = p if k == 0 else p + 1 + k
                self.write(idf + k if seq else idf, words[a], m, a)
            p += total


def emit_transition(frm, to, force):
    """Mirror of EmitTransition; returns (fail, words)."""
    def reg_mask(i):
        tb, fb = to.rbk[i], frm.rbk[i]
        if fb & ~tb & 0xF:
            return -1
        if force and force.get((None, i)):
            return tb
        m = tb & ~fb & 0xF
        for b in range(4):
            if tb & fb & (1 << b) and ((to.reg[i] ^ frm.reg[i]) >> (8 * b)) & 0xFF:
                m |= 1 << b
        return m

    def need(kind, idx):
        t = to.mem[kind][idx]
        f = frm.mem[kind][idx]
        if t is None:
            return -1 if f is not None else 0
        if force and force.get((kind, idx)):
            return 1
        return 1 if f is None or f != t else 0

    for i in range(REG_COUNT):
        if not no_delta(i) and reg_mask(i) < 0:
            return "unknown", []
    for k, n in SIZES.items():
        for i in range(n):
            if need(k, i) < 0:
                return "unknown", []
    out = []

    def reg(i, v, m=0xF):
        out.extend([v, (m << 16) | i])
    reg(0x111, 1)
    reg(0x110, 1)
    sd = bool(to.rbk[0x245] & 1)
    if sd:
        reg(0x245, 1, 1)
    for i in range(REG_COUNT):
        if i != 0x245 and not no_delta(i):
            m = reg_mask(i)
            if m > 0:
                reg(i, to.reg[i], m)
    for kind, base in ((VSF, VSH), (GSF, GSH)):
        for r in range(96):
            if need(kind, r) > 0:
                f32, w = to.mem[kind][r]
                out.extend([r | (0x80000000 if f32 else 0), 0x000F0000 | (base + OFF_F_IDX)])
                if f32:
                    out.extend([w[0], (3 << 20) | 0xF0000 | (base + OFF_F0), w[1], w[2], w[3], 0])
                else:
                    out.extend([w[0], (2 << 20) | 0xF0000 | (base + OFF_F0), w[1], w[2]])
    for kc, ko, base in ((VCODE, VOP, VSH), (GCODE, GOP, GSH)):
        anyc = False
        for i in range(512):
            if need(kc, i) > 0:
                out.extend([i, 0xF0000 | (base + OFF_C_IDX), to.mem[kc][i], 0xF0000 | (base + OFF_C0)])
                anyc = True
        if anyc:
            reg(base + OFF_CODE_END, 1)
        for i in range(128):
            if need(ko, i) > 0:
                out.extend([i, 0xF0000 | (base + OFF_O_IDX), to.mem[ko][i], 0xF0000 | (base + OFF_O0)])
    for i in range(12):
        if need(FATTR, i) > 0:
            w = to.mem[FATTR][i]
            out.extend([i, 0xF0232, w[0], 0x80000000 | (2 << 20) | 0xF0000 | 0x233, w[1], w[2]])
    for i in range(128):
        if need(FOG, i) > 0:
            out.extend([i, 0xF00E6, to.mem[FOG][i], 0xF00E8])
    if sd:
        reg(0x245, to.reg[0x245], to.rbk[0x245])
    return None, out


def f2w(f):
    return struct.unpack("<I", struct.pack("<f", f))[0]


def w2f(w):
    return struct.unpack("<f", struct.pack("<I", w))[0]


def shear_xyzw(r0, r3, k, kc):
    o = [r0[i] + k * r3[i] for i in range(4)]
    o[3] -= kc
    return o


def shear_row0(row0, row3, k, kc):
    a = [w2f(row0[3]), w2f(row0[2]), w2f(row0[1]), w2f(row0[0])]
    b = [w2f(row3[3]), w2f(row3[2]), w2f(row3[1]), w2f(row3[0])]
    o = [w2f(f2w(x)) for x in shear_xyzw(a, b, k, kc)]   # float32 rounding
    return [f2w(o[3]), f2w(o[2]), f2w(o[1]), f2w(o[0])]


# ------------------------------------------- independent GPU model (oracle)
class Gpu:
    def __init__(self, seed_regs=None):
        self.reg = dict(seed_regs or {})
        self.fl = {}
        self.code = {}
        self.fidx, self.f32, self.fbuf = 0, True, []
        self.cidx = 0
        self.draws = []

    def run(self, w):
        p = 0
        while p < len(w):
            h = w[p + 1]
            i, m, n, seq = h & 0xFFFF, (h >> 16) & 0xF, (h >> 20) & 0xFF, h >> 31
            for k in range(n + 1):
                self.write(i + k if seq else i, w[p] if k == 0 else w[p + 1 + k], m)
            p += (2 + n + 1) & ~1

    def write(self, r, v, m):
        b = mask_bytes(m)
        self.reg[r] = (self.reg.get(r, 0) & ~b & 0xFFFFFFFF) | (v & b)
        if r == 0x2C0:
            self.fidx, self.f32, self.fbuf = v & 0x7F, bool(v >> 31), []
        elif 0x2C1 <= r <= 0x2C8:
            self.fbuf.append(v)
            if len(self.fbuf) == (4 if self.f32 else 3):
                self.fl[self.fidx] = (self.f32, tuple(self.fbuf + [0] * (4 - len(self.fbuf))))
                self.fidx, self.fbuf = self.fidx + 1, []
        elif r == 0x2CB:
            self.cidx = v & 0xFFF
        elif 0x2CC <= r <= 0x2D3:
            self.code[self.cidx] = v
            self.cidx += 1
        elif r == 0x22E:
            self.draws.append((self.fl.get(0), self.reg.get(0x41), self.reg.get(0x11D)))

    def tracked(self):
        regs = {i: v for i, v in self.reg.items() if not no_delta(i) and i != 0x245}
        return regs, dict(self.fl), dict(self.code)


# ------------------------------------------------------ stream generator
def cmd(w, i, v, m=0xF):
    w.extend([v, (m << 16) | i])


def burst(w, reg, data):
    cmd(w, 0x2C0, 0x80000000 | reg)
    w.extend([data[0], ((len(data) - 1) << 20) | 0xF0000 | 0x2C1] + data[1:])
    if len(w) & 1:
        w.append(0)


def mat_words(rows):
    return [f2w(rows[r][c]) for r in range(4) for c in (3, 2, 1, 0)]


def rand_mat(rng, persp):
    m = [[rng.uniform(-2, 2) for _ in range(4)] for _ in range(4)]
    m[3] = [rng.uniform(-1, 1), rng.uniform(-1, 1), rng.uniform(-1, 1), rng.uniform(1, 9)] if persp else [0, 0, 0, 1]
    return [[w2f(f2w(x)) for x in r] for r in m]


MASKED_REGS = [0x229, 0x080, 0x100, 0x253]     # the game writes these with byte masks only
STATE_REGS = [0x40, 0x41, 0x42, 0x43, 0x4D, 0x4E, 0x100, 0x101, 0x107, 0x11C, 0x11D, 0x11E, 0x200, 0x201, 0x229,
              0x245, 0x25E, 0x2B0, 0x2BA]


def random_section(rng, w, n_ops, allow_matrix=True):
    for _ in range(n_ops):
        k = rng.random()
        if k < 0.35:
            cmd(w, rng.choice(STATE_REGS), rng.getrandbits(32), 0xF if rng.random() < 0.85 else rng.randint(1, 14))
        elif k < 0.45:
            cmd(w, rng.choice(MASKED_REGS), rng.getrandbits(32), rng.randint(1, 14))
        elif k < 0.65 and allow_matrix:
            burst(w, 0, mat_words(rand_mat(rng, rng.random() < 0.6)))
            cmd(w, 0x22E, 1)
        elif k < 0.75:
            reg = rng.choice([4, 12, 17, 19])
            burst(w, reg, [rng.getrandbits(32) for _ in range(4)])
        elif k < 0.85:
            cmd(w, 0x2CB, rng.randint(0, 20))
            for _ in range(rng.randint(1, 3)):
                cmd(w, 0x2CC, rng.getrandbits(32))
        else:
            cmd(w, 0x22E, 1)


def replay_once(rng):
    w = []
    # warm part: every state register and c0 written at least once
    for r in STATE_REGS:
        cmd(w, r, rng.getrandbits(32))
    for r in MASKED_REGS:                       # every byte, one masked write each
        for b in range(4):
            cmd(w, r, rng.getrandbits(32), 1 << b)
    burst(w, 0, mat_words(rand_mat(rng, False)))
    for reg in (4, 12, 17, 19):                 # every uniform the sections use
        burst(w, reg, [rng.getrandbits(32) for _ in range(4)])
    for i in range(24):                         # code words the sections may touch (idx <= 20, 3 words)
        cmd(w, 0x2CB, i)
        cmd(w, 0x2CC, rng.getrandbits(32))
    random_section(rng, w, rng.randint(0, 10))
    p0 = len(w)
    random_section(rng, w, rng.randint(1, 25))
    ph = len(w)
    random_section(rng, w, rng.randint(0, 15))
    cmd(w, 0x25E, 0, 0x8)
    end = len(w)

    cur = State()
    ps = Parser(cur)
    ps.setup(p0, ph)
    assert ps.run(w, 0, end)
    assert ps.fail is None, ps.fail
    k_full = rng.uniform(-0.2, 0.2)
    kc_full = k_full * rng.uniform(0.2, 2)
    force = {}
    copy = w[p0:ph]
    lefts = []
    for s in ps.sites:
        rgt = shear_row0(s["rows"][0], s["rows"][3], 0.5 * k_full, 0.5 * kc_full)
        lefts.append(shear_row0(s["rows"][0], s["rows"][3], -0.5 * k_full, -0.5 * kc_full))
        for j in range(4):
            copy[s["row0"][j] - p0] = rgt[j]
        force[(VSF, s["base"])] = 1
    f1, pro = emit_transition(cur, ps.snap_p0, None)
    f2, epi = emit_transition(ps.snap_ph, cur, force)
    assert f1 is None and f2 is None, (f1, f2)

    # oracle
    g_end = Gpu()
    g_end.run(w[:end])
    g_p0 = Gpu()
    g_p0.run(w[:p0])
    g_ph = Gpu()
    g_ph.run(w[:ph])
    g = Gpu(g_end.reg)
    g.fl, g.code = dict(g_end.fl), dict(g_end.code)
    g.run(pro)
    assert g.tracked() == g_p0.tracked(), "prologue != S_p0"
    g.draws = []
    g.run(copy)
    # the copy must draw exactly what the original range drew, with row 0 sheared
    g_orig = Gpu(g_p0.reg)
    g_orig.fl, g_orig.code = dict(g_p0.fl), dict(g_p0.code)
    g_orig.run(w[p0:ph])
    assert len(g.draws) == len(g_orig.draws)
    for a, b in zip(g.draws, g_orig.draws):
        assert a[1:] == b[1:]                  # same registers at every draw
    g.run(epi)
    assert g.tracked() == g_end.tracked(), "epilogue != S_end"
    # left patches apply in place to the original range
    for s, lw in zip(ps.sites, lefts):
        for j in range(4):
            w[s["row0"][j]] = lw[j]
    return len(ps.sites)


def test_replay_random():
    rng = random.Random(1234)
    sites = 0
    for _ in range(2000):
        sites += replay_once(rng)
    assert sites > 1000
    return f"2000 random streams, {sites} matrix sites"


def test_failures():
    w = []
    p0 = len(w)
    cmd(w, 0x41, 5)
    ph = len(w)
    s = State()
    ps = Parser(s)
    ps.setup(p0, ph)
    assert ps.run(w, 0, len(w))
    assert emit_transition(s, ps.snap_p0, None)[0] == "unknown"
    w = []
    cmd(w, 0x41, 1)
    p0 = len(w)
    burst(w, 1, [f2w(1.0)] * 4)
    ph = len(w)
    ps = Parser(State())
    ps.setup(p0, ph)
    assert ps.run(w, 0, len(w)) and ps.fail == "partial"
    w = []
    p0 = len(w)
    cmd(w, 0x010, 0x12345678)
    ps = Parser(State())
    ps.setup(p0, len(w))
    assert ps.run(w, 0, len(w)) and ps.fail is None and ps.fin == [p0]
    w = []
    w.extend([1, 0x80000000 | (2 << 20) | 0xF0000 | 0x41, 2, 3])
    ps = Parser(State())
    ps.setup(2, None)
    assert not ps.run(w, 0, len(w)) and ps.fail == "header"
    w = []
    cmd(w, 0x23C, 1)
    ps = Parser(State())
    ps.setup(None, None)
    ps.run(w, 0, len(w))
    assert ps.jump
    # B1: a byte of a masked-only register unknown at p0 and written after it
    w = []
    cmd(w, 0x229, 0x11, 0x1)
    p0 = len(w)
    cmd(w, 0x229, 0x2200, 0x2)
    s = State()
    ps = Parser(s)
    ps.setup(p0, len(w))
    assert ps.run(w, 0, len(w))
    assert emit_transition(s, ps.snap_p0, None)[0] == "unknown"
    # B1: a masked-only register (byte 0 only) changed after pH is restored with a masked write
    w = []
    cmd(w, 0x229, 0x11, 0x1)
    p0 = len(w)
    ph = len(w)
    cmd(w, 0x229, 0x33, 0x1)
    s = State()
    ps = Parser(s)
    ps.setup(p0, ph)
    assert ps.run(w, 0, len(w))
    f, pro = emit_transition(s, ps.snap_p0, None)
    assert f is None and any(pro[i] == 0x11 and pro[i + 1] == (1 << 16) | 0x229 for i in range(0, len(pro), 2))
    return "unknown-state, partial matrix, split in range, mark inside command, jump, masked-register bytes"


# --------------------------------------------------------- projection math
PRESETS = {  # name: (px, cap, hand)
    "Deep": (18, 4, -3), "Immersive": (18, 12, -6), "Normal": (14, 6, -2), "Gentle": (10, 3, 0),
    "Window": (14, 0, 2), "Zero": (0, 0, 0)}
KDMIN = 0.3


def disparity_px(s, px, cap, D):
    a = s * px / 200.0
    C = KDMIN * (1 + cap / px) if px > 0 else 1.0
    r0, r3 = [0.86, 0, 0, 0], [0, 0, -1, 0]
    v = [0, 0, -D, 1]
    L = shear_xyzw(r0, r3, -0.5 * a, -0.5 * a * C)
    R = shear_xyzw(r0, r3, 0.5 * a, 0.5 * a * C)
    dot = lambda m: sum(m[i] * v[i] for i in range(4))
    return (dot(R) - dot(L)) / D * 200.0


def test_projection():
    for name, (px, cap, hand) in PRESETS.items():
        if px == 0:
            assert abs(disparity_px(1.0, px, cap, 2.0)) < 1e-9
            continue
        # far = px behind, the nearest surface (0.3 blocks) = exactly cap in front
        assert abs(disparity_px(1.0, px, cap, 1e6) - px) < 1e-3, name
        assert abs(disparity_px(1.0, px, cap, KDMIN) + cap) < 1e-3, name
        # half slider = half everything
        assert abs(disparity_px(0.5, px, cap, 8.0) - 0.5 * disparity_px(1.0, px, cap, 8.0)) < 1e-6
        # monotonic in depth
        ds = [disparity_px(1.0, px, cap, D) for D in (0.3, 0.5, 1, 2, 4, 16, 64)]
        assert all(b > a for a, b in zip(ds, ds[1:])), name
    # constant (hand / HUD) class: x' = x + k*w -> the same disparity at every depth
    s, hand = 1.0, -3
    K = s * hand / 200.0
    for D in (0.5, 0.84, 3):
        r0 = [0.86, 0, 0, 0]
        r3 = [0, 0, -1, 0]
        L = shear_xyzw(r0, r3, -0.5 * K, 0)
        R = shear_xyzw(r0, r3, 0.5 * K, 0)
        v = [0, 0, -D, 1]
        d = (sum(R[i] * v[i] for i in range(4)) - sum(L[i] * v[i] for i in range(4))) / D * 200
        assert abs(d - hand) < 1e-6
    deep = disparity_px(1.0, 18, 4, 0.84)
    return f"presets ok; Deep: held item (0.84 blocks) {deep:+.1f} px, wall {disparity_px(1, 18, 4, 0.3):+.1f} px"


# ------------------------------------------------------ world_opts helpers
def auto3d(normal, choice):
    if normal <= 0:
        return 0
    t = choice if choice > 0 else (4 if normal >= 5 else (normal if normal < 3 else 3))
    t = min(t, normal)
    if t < 2:
        t = normal if normal < 2 else 2
    return t


def test_auto3d():
    assert [auto3d(n, 0) for n in (2, 3, 4, 5, 6, 8)] == [2, 3, 3, 4, 4, 4]
    assert auto3d(6, 2) == 2 and auto3d(3, 4) == 3 and auto3d(0, 0) == 0
    return "auto rule {2:2, 3:3, 4:3, 5:4, 6:4}"


class Hyst:
    def __init__(self):
        self.applied = False
        self.off = None
        self.last_grow = None


def hyst_step(h, want, now, off_t, gap_t):
    if want:
        h.off = None
        h.applied = True
        return True
    if not h.applied:
        return False
    if h.off is None:
        h.off = now
    if (now - h.off) % 2**32 >= off_t and (h.last_grow is None or (now - h.last_grow) % 2**32 >= gap_t):
        h.applied, h.last_grow, h.off = False, now, None
    return h.applied


def test_hysteresis():
    h = Hyst()
    t = 0
    assert hyst_step(h, True, t, 5000, 20000)               # shrink at once
    for t in range(0, 4999, 100):
        assert hyst_step(h, False, 1000 + t, 5000, 20000)   # still lowered < 5 s
    assert not hyst_step(h, False, 6000, 5000, 20000)       # grows back at 5 s
    assert hyst_step(h, True, 6500, 5000, 20000)            # 3D on again: lowered at once
    # off again at 7000: 5 s passes at 12000 but the last grow was at 6000 -> wait to 26000
    assert hyst_step(h, False, 7000, 5000, 20000)
    assert hyst_step(h, False, 12000, 5000, 20000)
    assert hyst_step(h, False, 25999, 5000, 20000)
    assert not hyst_step(h, False, 26000, 5000, 20000)
    # wiggling the slider every second never grows back in between
    h = Hyst()
    grows = 0
    state = False
    for s in range(0, 120):
        new = hyst_step(h, s % 2 == 0, s * 1000, 5000, 20000)
        grows += state and not new
        state = new
    assert grows == 0
    # u32 millisecond wrap
    h = Hyst()
    hyst_step(h, True, 2**32 - 1000, 5000, 20000)
    hyst_step(h, False, 2**32 - 500, 5000, 20000)
    assert not hyst_step(h, False, 5000, 5000, 20000)
    return "shrink at once, grow after 5 s, at most once per 20 s, slider wiggle = no rebuild"


# ----------------------------------------------------------------- pacer
def pace_sim(cpu_ms, gpu_ms, frames=600, lock=True, vbl=16.7139):
    """Event model of present(): WaitCmdlistDone(prev) -> pacer -> RunCmdlist.
    Swap = first vblank after the list finishes. Returns display intervals."""
    t = 0.0
    last_run_v = None
    gpu_free = 0.0
    shows = []
    for f in range(frames):
        t += cpu_ms(f)                          # record the frame
        t = max(t, gpu_free)                    # WaitCmdlistDone(previous)
        if lock:
            v = math.floor(t / vbl)
            if last_run_v is not None and v - last_run_v < 2:
                t = (last_run_v + 2) * vbl + 0.25   # woken by the 0.5 ms poll
                v = last_run_v + 2
            last_run_v = math.floor(t / vbl)
        gpu_free = t + gpu_ms(f)
        shows.append(math.floor(gpu_free / vbl) + 1)
    return [b - a for a, b in zip(shows, shows[1:])]


def test_pacer():
    iv = pace_sim(lambda f: 20.0, lambda f: 26.0)
    hit = iv.count(2) / len(iv)
    assert hit > 0.99, hit
    iv2 = pace_sim(lambda f: 22.0 + (8.0 if f % 50 == 0 else 0.0), lambda f: 24.0)
    hit2 = iv2.count(2) / len(iv2)
    unl = pace_sim(lambda f: 20.0, lambda f: 26.0, lock=False)
    jit = lambda xs: (sum((x - sum(xs) / len(xs)) ** 2 for x in xs) / len(xs)) ** 0.5
    assert jit(iv) < jit(unl)
    return (f"locked: {100 * hit:.0f}% of images 2 vblanks (GPU 26 ms); with a 30 ms CPU spike every 50 frames "
            f"{100 * hit2:.0f}%; unlocked jitter {jit(unl):.2f} vs locked {jit(iv):.2f} vblanks")


# ------------------------------------------- pieces (delta audit D-B1)
def plan_pieces(p0, ph, bounds):
    """Mirror of pica::PlanPieces (word indices here, so 8 bytes = 2 words)."""
    if ph < p0 or p0 % 2 or ph % 2:
        return None
    out, start = [], p0
    for b in bounds:
        if b % 2 or b < start + 2 or b > ph:
            return None
        out.append((start, b - 2, b))
        start = b
    out.append((start, ph, ph))
    return out


def split_trailer(w, base):
    """nngx 0x7C5518: pad to 16 bytes relative to the list base, then flush, invalidate, finalize."""
    if ((len(w) - base) * 4) & 8 == 0:
        cmd(w, 0x25E, 0, 0x8)
    cmd(w, 0x111, 1)
    cmd(w, 0x110, 1)
    cmd(w, 0x010, 0x12345678)
    return len(w)                 # segment end (16-aligned relative to base)


def commands(words):
    p, out = 0, []
    while p < len(words):
        h = words[p + 1]
        n = (h >> 20) & 0xFF
        out.append((h & 0xFFFF, (h >> 16) & 0xF))
        p += (2 + n + 1) & ~1
    assert p == len(words), "copy does not end on a command boundary"
    return out


def test_pieces():
    rng = random.Random(7)
    for _ in range(300):
        w = []
        base = 0
        cmd(w, 0x40, 1)
        p0 = len(w)
        bounds = []
        for _seg in range(rng.randint(0, 3)):
            random_section(rng, w, rng.randint(1, 8))
            bounds.append(split_trailer(w, base))
        random_section(rng, w, rng.randint(0, 8))
        ph = len(w)
        pieces = plan_pieces(p0, ph, bounds)
        assert pieces and len(pieces) == len(bounds) + 1
        copied = []
        for s, ce, e in pieces:
            piece = w[s:ce]
            ids = commands(piece)
            assert all(i != 0x010 for i, _ in ids), "a game finalize was copied"
            copied.extend(piece)
        # everything except the finalize commands is replayed, in order
        orig = [c for c in commands(w[p0:ph]) if c[0] != 0x010]
        assert commands(copied) == orig
    assert plan_pieces(0, 10, [4, 4]) is None and plan_pieces(0, 10, [12]) is None and plan_pieces(0, 10, [1]) is None
    return "300 random ranges with 0-3 splits: no finalize copied, every other command kept in order"


# ------------------------------------------------- governor (D-S1)
class Gov:
    def __init__(self, window=60, max_late=6, cool_ms=10000):
        self.window, self.max_late, self.cool_ms = window, max_late, cool_ms
        self.bits, self.cooling, self.cool_until, self.trips = 0, False, 0, 0


def gov_step(g, seen, late, now):
    if g.cooling:
        if ((now - g.cool_until) & 0xFFFFFFFF) >= 0x80000000:
            return False
        g.cooling, g.bits = False, 0
    if not seen:
        return True
    g.bits = ((g.bits << 1) | int(late)) & ((1 << g.window) - 1)
    if bin(g.bits).count("1") >= g.max_late:
        g.cooling, g.cool_until = True, (now + g.cool_ms) & 0xFFFFFFFF
        g.trips += 1
        return False
    return True


def test_governor():
    g = Gov()
    t = 0
    for i in range(600):                 # 5 % late: never trips
        assert gov_step(g, True, i % 20 == 0, t)
        t += 33
    g = Gov()
    allowed = [gov_step(g, True, i % 5 == 0, i * 33) for i in range(40)]   # 20 % late
    trip_at = allowed.index(False)
    assert g.trips == 1 and trip_at < 30
    t = 40 * 33
    assert not gov_step(g, False, False, t)                       # cooling
    assert gov_step(g, False, False, trip_at * 33 + 10000 + 1)    # back after 10 s, clean window
    return f"5 % late never trips; 20 % late trips after {trip_at + 1} frames; 10 s cool-down"


def test_governor_low_fps_cycle():
    """v0.3.1 evidence: the governor at hardware frame rates. Both-eye frames
    that miss the 30 slot (GPU or CPU > 33 ms) make nearly every paced frame
    late, so after each 10 s cool-down Comfort lasts only a few frames: the
    flat-plane / 3D flip Matt reported."""
    out = []
    for gpu in (30.0, 36.0, 42.0):              # ms per both-eye list (design 1: 34-53 ms before cuts)
        iv = pace_sim(lambda f: 22.0, lambda f, g=gpu: g, frames=3000)
        g = Gov()
        t, comfort_ms, flat_ms, fi = 0.0, 0.0, 0.0, 0
        runs = []
        run = 0
        while t < 120000.0:                     # two minutes of play
            if gov_step(g, False, False, int(t)):
                d = iv[fi % len(iv)]
                fi += 1
                ok = gov_step(g, True, d > 2, int(t))
                if ok:
                    comfort_ms += d * 16.7139
                    run += 1
                else:
                    runs.append(run)
                    run = 0
                t += d * 16.7139
            else:
                flat_ms += 25.0                 # unpaced one-eye frames while cooling
                t += 25.0
        share = comfort_ms / (comfort_ms + flat_ms)
        out.append((gpu, g.trips, share, max(runs) if runs else run))
    # 30 ms fits the slot: never trips. 36/42 ms: trips every ~10 s, 3D only a sliver of the time
    assert out[0][1] == 0
    assert out[1][1] >= 10 and out[1][2] < 0.2 and out[2][1] >= 10 and out[2][2] < 0.1
    return "; ".join(f"GPU {g:.0f} ms: {n} trips/2 min, 3D {100 * s:.0f}% of the time, longest 3D run {r} frames"
                     for g, n, s, r in out)


# --------------------------------------------- hold the last pair (v0.3.1)
class Hold:
    def __init__(self, max_run=3):
        self.max_run, self.run, self.valid = max_run, 0, False


def hold_step(h, replayed, pair_on_screen):
    """Mirror of pica::HoldStep: 'both' / 'hold' / 'mono'."""
    if replayed:
        h.valid, h.run = True, 0
        return "both"
    if h.valid and pair_on_screen and h.run < h.max_run:
        h.run += 1
        return "hold"
    h.valid, h.run = False, 0
    return "mono"


def pick_both_slot(recent):
    for j in range(3):
        if j not in recent[0] and j not in recent[1]:
            return j
    return -1


def note_slot(recent, eye, k):
    recent[eye][1] = recent[eye][0]
    recent[eye][0] = k


def test_hold():
    # isolated failures never show 2D; long runs fall back after max_run
    h = Hold()
    seq = [True, False, True, True, False, False, True] + [False] * 6 + [True]
    shown = [hold_step(h, r, True) for r in seq]
    assert shown[:7] == ["both", "hold", "both", "both", "hold", "hold", "both"]
    assert shown[7:13] == ["hold", "hold", "hold", "mono", "mono", "mono"] and shown[13] == "both"
    # no pair to hold (first frame, after a non-Comfort frame, or no shared slot): 2D
    h = Hold()
    assert hold_step(h, False, True) == "mono"
    hold_step(h, True, True)
    assert hold_step(h, False, False) == "mono" and hold_step(h, False, True) == "mono"
    h0 = Hold(0)                                  # hold=0: v0.3.0 behaviour
    hold_step(h0, True, True)
    assert hold_step(h0, False, True) == "mono"
    # slot safety (mirrors stereo_flat AfterTopB2L): a held pair is never the
    # slot the next frame writes, nor the slot shown just before it
    rng = random.Random(11)
    recent = [[-1, -1], [-1, -1]]
    h = Hold()
    shown_slots = []                              # (left, right) handed to the swap, per frame
    holds = 0
    for f in range(20000):
        ok = rng.random() < 0.8
        k = pick_both_slot(recent)
        if ok and k >= 0:
            # the new copy must not hit either of the last two pairs handed to the swap
            for prev in shown_slots[-2:]:
                assert k not in prev, (f, k, shown_slots[-2:])
            hold_step(h, True, True)
            note_slot(recent, 0, k)
            note_slot(recent, 1, k)
            shown_slots.append((k, k))
            continue
        pair = recent[0][0] >= 0 and recent[0][0] == recent[1][0]
        if hold_step(h, False, pair) == "hold":
            holds += 1
            shown_slots.append((recent[0][0], recent[1][0]))   # rotation not advanced
        else:
            # 2D: the left picture copied into a free LEFT slot, shown in both eyes
            kl = k if k >= 0 else next(j for j in range(3) if j not in recent[0])
            for prev in shown_slots[-2:]:
                assert kl not in prev[:1], (f, kl)
            note_slot(recent, 0, kl)
            note_slot(recent, 1, -1)
            shown_slots.append((kl, -1))
    assert holds > 1000
    return f"isolated failures held, runs > 3 go 2D, hold=0 = v0.3.0; 20000 random frames: {holds} holds, no slot reused while shown"


class Limiter:
    """pica::LimiterAllow: at most `burst` events per `window` ms."""
    def __init__(self, burst=12, window=1000):
        self.burst, self.window, self.start, self.n, self.suppressed, self.started = burst, window, 0, 0, 0, False

    def allow(self, now):
        if not self.started or ((now - self.start) & 0xFFFFFFFF) >= self.window:
            self.started, self.start, self.n = True, now, 0
        if self.n >= self.burst:
            self.suppressed += 1
            return False
        self.n += 1
        return True


def test_event_log():
    # v0.3.2 switch log (stereo_flat AfterTopB2L -> prof::PushEvent -> 64-slot
    # ring -> FlushEvents once per second). One event per change of the shown
    # picture; check the ring never overflows at one flush per second and how
    # big the log gets, for three failure patterns at 25 fps.
    def run(fail_p, seconds, burst_fail=None, seed=5):
        rng = random.Random(seed)
        h, lim = Hold(), Limiter()
        last, ring, peak, lines = "3D", 0, 0, 0
        frame_ms = 40
        for f in range(seconds * 25):
            now = f * frame_ms
            ok = rng.random() >= fail_p if burst_fail is None else not burst_fail(f)
            s = hold_step(h, ok, True)
            pic = {"both": "3D", "hold": "held", "mono": "2D"}[s]
            if pic != last:
                if lim.allow(now):
                    ring += 1
                last = pic
            if f % 25 == 24:                      # plugin thread flush, once per second
                peak = max(peak, ring)
                lines += ring
                ring = 0
        return peak, lines, lim.suppressed
    # Azahar-like: 1-3 isolated unknown-state frames per 10 s
    peak, lines, sup = run(0.008, 600)
    per_hour = lines * 6 * 95                     # ~95 bytes per line
    assert peak <= 12 and sup == 0
    assert per_hour < 400 * 1024, per_hour
    # pathological: every other frame fails (flapping 3D/held each frame)
    peak2, lines2, sup2 = run(0, 60, burst_fail=lambda f: f % 2 == 1)
    assert peak2 <= 12 < 64 and sup2 > 0 and lines2 <= 12 * 61
    # long failure runs (a loading screen with the slider up): few lines
    peak3, lines3, _ = run(0, 60, burst_fail=lambda f: 200 <= f < 1200)
    assert lines3 <= 4, lines3
    return (f"isolated failures: {lines / 10:.0f} lines/min (~{per_hour // 1024} KB/h), ring peak {peak}/64; "
            f"flapping every frame: <= 12 lines/s, {sup2} suppressed in 60 s; 40 s 2D run: {lines3} lines")


# ------------------------------------------------- v0.3.3 A/B swap (input_swap.cpp)
PAD_A, PAD_B = 1, 2


def swap_ab(b):                                   # pica::SwapAB
    return (b & ~3) | ((b & PAD_A) << 1) | ((b & PAD_B) >> 1)


class AbLatch:                                    # pica::AbLatch / AbLatchStep
    def __init__(self):
        self.active = False
        self.prev_quiet = True

    def step(self, want, touched):
        quiet = (touched & 3) == 0
        if want != self.active and quiet and self.prev_quiet:
            self.active = want
        self.prev_quiet = quiet
        return self.active


def test_ab_swap():
    """Model of the game's two pad reads per frame (RE 0x10B4DC):
    ReadLatest 0x11298C: newest raw sample, trigger/release vs ITS OWN previous
    raw hold (reader+0x54); Read 0x112B34: every sample since the last Read
    (newest first, ring of 8), trigger/release per sample vs the sample before.
    The plugin swaps hold/trig/rel after each call. Two game-side consumers
    track the buttons from trigger/release events only; they must never see a
    release of a button they don't hold or a press of one they do, must always
    agree with the reported hold, and must end empty once everything is up.
    Random taps, holds, A+B chords, toggles mid-press, 0-sample frames."""
    rng = random.Random(33)
    other = 0x4000 | 0x4                          # ZR + Select ride along, never swapped
    worst_wait = 0
    for run in range(300):
        latch = AbLatch()
        want = rng.random() < 0.5
        raw_hist = [0]                            # raw holds, one per 4 ms sample
        latest_prev = 0                           # ReadLatest's own previous raw hold
        read_pos = 1                              # next unread sample for Read
        held_l, held_r = set(), set()             # consumers (event-tracked)
        btn = 0
        pending_since = None
        for frame in range(400):
            # input for this frame: ~8 samples (33 ms at 4 ms)
            for _ in range(rng.choice((0, 3, 8, 8, 8))):     # <= 8: the ring never overflows
                r = rng.random()
                if r < 0.08:
                    btn ^= PAD_A
                elif r < 0.16:
                    btn ^= PAD_B
                elif r < 0.18:
                    btn ^= other
                if frame >= 370:
                    btn = 0                       # everything released at the end
                raw_hist.append(btn)
            if rng.random() < 0.05 and frame < 360:
                want = not want                   # toggle in the menu, often mid-press
            if latch.active != want and pending_since is None:
                pending_since = frame
            # --- ReadLatest
            cur = raw_hist[-1]
            hold, trig, rel = cur, (cur ^ latest_prev) & ~latest_prev, latest_prev & ~cur
            latest_prev = cur
            out = [hold, trig, rel]
            if latch.step(want, hold | trig | rel):
                out = [swap_ab(x) for x in out]
            for bit in (PAD_A, PAD_B):
                if out[1] & bit:
                    assert bit not in held_l, (run, frame, "L press while held")
                    held_l.add(bit)
                if out[2] & bit:
                    assert bit in held_l, (run, frame, "L release while not held")
                    held_l.discard(bit)
            assert held_l == {b for b in (PAD_A, PAD_B) if out[0] & b}, (run, frame, held_l, out)
            assert out[0] & ~3 == cur & ~3
            # --- Read (newest first, at most 8 back)
            new = list(range(max(read_pos, len(raw_hist) - 8), len(raw_hist)))
            read_pos = len(raw_hist)
            samples = []
            for i in reversed(new):
                h, p = raw_hist[i], raw_hist[i - 1]
                samples.append([h, (h ^ p) & ~p, p & ~h])
            if samples:
                touched = 0
                for s in samples:
                    touched |= s[0] | s[1] | s[2]
                if latch.step(want, touched):
                    samples = [[swap_ab(x) for x in s] for s in samples]
                for s in reversed(samples):           # consumer: oldest first
                    for bit in (PAD_A, PAD_B):
                        if s[1] & bit:
                            assert bit not in held_r, (run, frame, "R press while held")
                            held_r.add(bit)
                        if s[2] & bit:
                            assert bit in held_r, (run, frame, "R release while not held")
                            held_r.discard(bit)
                assert held_r == {b for b in (PAD_A, PAD_B) if samples[0][0] & b}, (run, frame)
            if latch.active == want and pending_since is not None:
                worst_wait = max(worst_wait, frame - pending_since)
                pending_since = None
        assert not held_l and not held_r, (run, held_l, held_r)
        assert latch.active == want, run          # a quiet end always applies the toggle
    # steady state: off = identity, on = exactly A<->B
    for b in range(64):
        assert swap_ab(swap_ab(b)) == b and swap_ab(b) & ~3 == b & ~3
    assert swap_ab(PAD_A) == PAD_B and swap_ab(PAD_B) == PAD_A and swap_ab(3) == 3
    return f"300 random runs x 400 frames, no stuck/phantom A or B; worst toggle delay {worst_wait} frames (random mashing)"


def main():
    tests = [test_failures, test_replay_random, test_pieces, test_governor, test_governor_low_fps_cycle, test_hold,
             test_projection, test_auto3d, test_hysteresis, test_pacer, test_event_log, test_ab_swap]
    failed = 0
    for t in tests:
        try:
            print(f"PASS {t.__name__}: {t()}")
        except AssertionError as e:
            failed += 1
            print(f"FAIL {t.__name__}: {e!r}")
    print("all passed" if not failed else f"{failed} FAILED")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
