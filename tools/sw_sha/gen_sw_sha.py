#!/usr/bin/env python3
"""
Generates sw_sha256d_prefilter_xtensa.S: a hand-scheduled Xtensa (ESP32) version
of the software miner's hot path, nerd_sha256d_baked(), up to its early exit.

bool nerd_sha256d_prefilter(const uint32_t *midstate, const uint8_t *tail16,
                            const uint32_t *bake)
  Returns true when the double SHA-256 of the header passes the 16-bit
  prefilter (low 16 bits of the digest's last word zero, i.e. hash[30] and
  hash[31] zero), exactly when nerd_sha256d_baked() returns true. The caller
  then calls nerd_sha256d_baked() for the full hash (1 nonce in 65536).

Why it is faster than the C version: the eight SHA state words, the two
round temporaries and the Maj() carry live in registers for all 121 rounds
(register roles rotate instead of values being moved), constants are folded
at generation time (zero message words, K+W of the padding words, the
IV-only first round of the second hash), and the message schedule is
computed in its own phase with a register cache of the most recent words.
GCC spills the state to the stack on every round (~2000 stack accesses per
call).

Usage: gen_sw_sha.py src/sw_sha256d_prefilter.S      (also importable: build() returns the text)
"""
import sys

K = [
    0x428A2F98, 0x71374491, 0xB5C0FBCF, 0xE9B5DBA5, 0x3956C25B, 0x59F111F1, 0x923F82A4, 0xAB1C5ED5,
    0xD807AA98, 0x12835B01, 0x243185BE, 0x550C7DC3, 0x72BE5D74, 0x80DEB1FE, 0x9BDC06A7, 0xC19BF174,
    0xE49B69C1, 0xEFBE4786, 0x0FC19DC6, 0x240CA1CC, 0x2DE92C6F, 0x4A7484AA, 0x5CB0A9DC, 0x76F988DA,
    0x983E5152, 0xA831C66D, 0xB00327C8, 0xBF597FC7, 0xC6E00BF3, 0xD5A79147, 0x06CA6351, 0x14292967,
    0x27B70A85, 0x2E1B2138, 0x4D2C6DFC, 0x53380D13, 0x650A7354, 0x766A0ABB, 0x81C2C92E, 0x92722C85,
    0xA2BFE8A1, 0xA81A664B, 0xC24B8B70, 0xC76C51A3, 0xD192E819, 0xD6990624, 0xF40E3585, 0x106AA070,
    0x19A4C116, 0x1E376C08, 0x2748774C, 0x34B0BCB5, 0x391C0CB3, 0x4ED8AA4A, 0x5B9CCA4F, 0x682E6FF3,
    0x748F82EE, 0x78A5636F, 0x84C87814, 0x8CC70208, 0x90BEFFFA, 0xA4506CEB, 0xBEF9A3F7, 0xC67178F2]
IV = [0x6A09E667, 0xBB67AE85, 0x3C6EF372, 0xA54FF53A, 0x510E527F, 0x9B05688C, 0x1F83D9AB, 0x5BE0CD19]
M = 0xFFFFFFFF


def rotr(x, n): return ((x >> n) | (x << (32 - n))) & M
def S0(x): return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3)
def S1(x): return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10)
def S2(x): return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22)
def S3(x): return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25)
def F1(e, f, g): return g ^ (e & (f ^ g))
def F0(a, b, c): return (a & b) | (c & (a | b))


FRAME = 320          # W[64] at sp+0..255, saved pointers above, 32 bytes reserved (ABI)
SAVE_DIGEST = 256
ALL_REGS = ["a%d" % i for i in range(2, 16)]


class Gen:
    def __init__(self):
        self.code = []
        self.lits = {}

    def lit(self, v):
        v &= M
        if v not in self.lits:
            self.lits[v] = ".Lsw_k%d" % len(self.lits)
        return self.lits[v]

    def i(self, s):
        self.code.append("    " + s)

    def const(self, reg, v):
        v &= M
        if v < 2048 or v >= (M + 1 - 2048):
            sv = v if v < 2048 else v - (M + 1)
            self.i("movi    %s, %d" % (reg, sv))
        else:
            self.i("l32r    %s, %s" % (reg, self.lit(v)))

    def add_const(self, dst, src, v):
        v &= M
        sv = v if v < 0x80000000 else v - (M + 1)
        if -128 <= sv <= 127:
            self.i("addi    %s, %s, %d" % (dst, src, sv))
        elif sv % 256 == 0 and -32768 <= sv <= 32512:
            self.i("addmi   %s, %s, %d" % (dst, src, sv))
        else:
            return False
        return True

    def rot(self, dst, src, n):
        self.i("ssai    %d" % n)
        self.i("src     %s, %s, %s" % (dst, src, src))


# ---------------------------------------------------------------------------
# Message schedule phase: W[t] for t in [lo, hi], with a small LRU register
# cache of recent words so W[t-2] and W[t-7] usually need no load.
# W entries: ("c", value) constant, ("m",) in its stack slot, ("r", reg) not
# stored yet (only during setup).
# ---------------------------------------------------------------------------
class Sched:
    def __init__(self, g, W, regs):
        self.g, self.W = g, W
        self.free = list(regs)
        self.cache = {}   # t -> reg
        self.lru = []

    def take(self):
        if self.free:
            return self.free.pop(0)
        t = self.lru.pop(0)
        r = self.cache.pop(t)
        return r

    def give(self, r):
        self.free.append(r)

    def cached(self, t):
        if t in self.cache:
            self.lru.remove(t)
            self.lru.append(t)
            return self.cache[t]
        return None

    def put_cache(self, t, r):
        self.cache[t] = r
        self.lru.append(t)

    def expand(self, t):
        g, W = self.g, self.W
        terms = [(S1, t - 2), (None, t - 7), (S0, t - 15), (None, t - 16)]
        csum = 0
        var = []
        for fn, s in terms:
            if W[s][0] == "c":
                csum = (csum + (fn(W[s][1]) if fn else W[s][1])) & M
            else:
                var.append((fn, s))
        if not var:
            W[t] = ("c", csum)
            return
        # pin cached sources so the temps below can't evict them
        pinned = set()
        srcreg = {}
        for fn, s in var:
            r = self.cached(s)
            if r is not None:
                srcreg[s] = r
                pinned.add(s)
        # loads first (hide the load-use latency)
        for fn, s in var:
            if s not in srcreg:
                r = self.take_unpinned(pinned)
                g.i("l32i    %s, a1, %d" % (r, 4 * s))
                srcreg[s] = r
                self.put_cache(s, r)
                pinned.add(s)
        acc = self.take_unpinned(pinned)
        tmp = self.take_unpinned(pinned)
        tmp2 = self.take_unpinned(pinned)
        first = True

        def accum(src):
            nonlocal first
            if first:
                g.i("mov     %s, %s" % (acc, src)) if src != acc else None
                first = False
            else:
                g.i("add     %s, %s, %s" % (acc, acc, src))

        # sigma terms first (longest), plain terms after
        for fn, s in var:
            x = srcreg[s]
            if fn is S1:
                dst = acc if first else tmp
                g.rot(dst, x, 17)
                g.rot(tmp2, x, 19)
                g.i("xor     %s, %s, %s" % (dst, dst, tmp2))
                g.i("srli    %s, %s, 10" % (tmp2, x))
                g.i("xor     %s, %s, %s" % (dst, dst, tmp2))
                if first:
                    first = False
                else:
                    g.i("add     %s, %s, %s" % (acc, acc, dst))
            elif fn is S0:
                if first:
                    dst = acc
                else:
                    dst = tmp
                g.rot(dst, x, 7)
                g.rot(tmp2, x, 18)
                g.i("xor     %s, %s, %s" % (dst, dst, tmp2))
                g.i("srli    %s, %s, 3" % (tmp2, x))
                g.i("xor     %s, %s, %s" % (dst, dst, tmp2))
                if first:
                    first = False
                else:
                    g.i("add     %s, %s, %s" % (acc, acc, dst))
        for fn, s in var:
            if fn is None:
                accum(srcreg[s])
        if csum:
            if not g.add_const(acc, acc, csum):
                g.const(tmp, csum)
                g.i("add     %s, %s, %s" % (acc, acc, tmp))
        g.i("s32i    %s, a1, %d" % (acc, 4 * t))
        W[t] = ("m",)
        self.give(tmp)
        self.give(tmp2)
        self.put_cache(t, acc)

    def take_unpinned(self, pinned):
        if self.free:
            return self.free.pop(0)
        for i, t in enumerate(self.lru):
            if t not in pinned:
                self.lru.pop(i)
                return self.cache.pop(t)
        raise RuntimeError("out of registers")

    def flush(self):
        for t, r in self.cache.items():
            self.free.append(r)
        self.cache.clear()
        self.lru.clear()


# ---------------------------------------------------------------------------
# Compression rounds
# ---------------------------------------------------------------------------
class Rounds:
    """st = [a,b,c,d,e,f,g,h] register names; P holds b^c; T,U,X,Y temps."""

    def __init__(self, g, st, P, temps):
        self.g, self.st, self.P = g, st, P
        self.T, self.U, self.X, self.Y = temps

    def round(self, t, W, full=True):
        g = self.g
        a, b, c, d, e, f, gg, h = self.st
        T, U, X, Y = self.T, self.U, self.X, self.Y
        if W[t][0] == "c":
            g.const(Y, K[t] + W[t][1])
            wreg = None
        else:
            g.const(Y, K[t])
            g.i("l32i    %s, a1, %d" % (X, 4 * t))
            wreg = X
        # S3(e)
        g.rot(T, e, 6)
        g.rot(U, e, 11)
        g.i("xor     %s, %s, %s" % (T, T, U))
        g.rot(U, e, 25)
        g.i("xor     %s, %s, %s" % (T, T, U))
        g.i("add     %s, %s, %s" % (h, h, Y))
        if wreg:
            g.i("add     %s, %s, %s" % (h, h, wreg))
        g.i("add     %s, %s, %s" % (h, h, T))
        # F1(e,f,g) = g ^ (e & (f ^ g))
        g.i("xor     %s, %s, %s" % (U, f, gg))
        g.i("and     %s, %s, %s" % (U, U, e))
        g.i("xor     %s, %s, %s" % (U, U, gg))
        g.i("add     %s, %s, %s" % (h, h, U))        # h = T1
        g.i("add     %s, %s, %s" % (d, d, h))        # d = new e
        if full:
            # Maj(a,b,c) = ((a^b) & (b^c)) ^ b, b^c carried in P
            g.i("xor     %s, %s, %s" % (U, a, b))
            g.i("and     %s, %s, %s" % (T, U, self.P))
            g.i("xor     %s, %s, %s" % (T, T, b))
            g.i("add     %s, %s, %s" % (h, h, T))
            # S2(a)
            g.rot(X, a, 2)
            g.rot(Y, a, 13)
            g.i("xor     %s, %s, %s" % (X, X, Y))
            g.rot(Y, a, 22)
            g.i("xor     %s, %s, %s" % (X, X, Y))
            g.i("add     %s, %s, %s" % (h, h, X))    # h = new a
            self.P, self.U = U, self.P               # next b^c = this a^b
        self.st = [h, a, b, c, d, e, f, gg]


def build():
    g = Gen()
    g.i("entry   a1, %d" % FRAME)
    g.i("s32i    a2, a1, %d" % SAVE_DIGEST)
    # ---------------- first SHA-256: message schedule ----------------
    W = [None] * 64
    # W3 = big-endian word at tail16+12 (the nonce)
    g.i("l8ui    a6, a3, 12")
    g.i("l8ui    a7, a3, 13")
    g.i("l8ui    a8, a3, 14")
    g.i("l8ui    a9, a3, 15")
    g.i("slli    a6, a6, 24")
    g.i("slli    a7, a7, 16")
    g.i("or      a6, a6, a7")
    g.i("slli    a8, a8, 8")
    g.i("or      a6, a6, a8")
    g.i("or      a6, a6, a9")
    g.i("s32i    a6, a1, 12")
    # baked words: W0..W2 = bake[0..2], W16 = bake[3], W17 = bake[4]
    for t, bi in ((2, 2), (16, 3), (17, 4)):
        g.i("l32i    a7, a4, %d" % (4 * bi))
        g.i("s32i    a7, a1, %d" % (4 * t))
    for t in (0, 1, 2, 3, 16, 17):
        W[t] = ("m",)
    W[4] = ("c", 0x80000000)
    for t in range(5, 15):
        W[t] = ("c", 0)
    W[15] = ("c", 640)
    regs = [r for r in ALL_REGS if r != "a4"]
    sc = Sched(g, W, regs)
    sc.put_cache(3, "a6")
    sc.free.remove("a6")
    for t in range(18, 64):
        sc.expand(t)
    sc.flush()
    # ---------------- first SHA-256: rounds 3..63 ----------------
    # state after the baked rounds 0..2: A[0..7] = bake[5..12]
    sregs = ["a2", "a3", "a5", "a6", "a7", "a8", "a9", "a10"]
    for k, r in enumerate(sregs):
        g.i("l32i    %s, a4, %d" % (r, 4 * (5 + k)))
    A = dict(zip(range(8), sregs))
    # round 3: P(A5,A6,A7,A0,A1,A2,A3,A4, W3): T1 = bake13 + W3, T2 = bake14
    g.i("l32i    a11, a4, %d" % (4 * 13))
    g.i("l32i    a12, a1, 12")
    g.i("l32i    a13, a4, %d" % (4 * 14))
    g.i("add     a11, a11, a12")                 # T1
    g.i("add     %s, %s, a11" % (A[0], A[0]))    # d += T1 -> new e
    g.i("add     %s, a11, a13" % A[4])           # h = T1 + T2 -> new a
    st = [A[4], A[5], A[6], A[7], A[0], A[1], A[2], A[3]]
    P = "a11"
    g.i("xor     %s, %s, %s" % (P, st[1], st[2]))  # b ^ c for round 4
    rd = Rounds(g, st, P, ["a12", "a13", "a14", "a15"])
    # a4 is free from here on: use it as the 5th temp by swapping it in for X
    rd.X = "a4"
    spare = "a14"
    rd.T, rd.U, rd.Y = "a12", "a13", "a15"
    for t in range(4, 64):
        rd.round(t, W)
    # ---------------- feed-forward: second hash input W[0..7] ----------------
    st = rd.st
    dp = rd.T
    g.i("l32i    %s, a1, %d" % (dp, SAVE_DIGEST))
    tmp = [rd.U, rd.Y, rd.X, spare]
    for k in range(8):
        r = tmp[k % 2]
        g.i("l32i    %s, %s, %d" % (r, dp, 4 * k))
        g.i("add     %s, %s, %s" % (st[k], st[k], r))
        g.i("s32i    %s, a1, %d" % (st[k], 4 * k))
    # ---------------- second SHA-256: message schedule ----------------
    W2 = [None] * 64
    for t in range(8):
        W2[t] = ("m",)
    W2[8] = ("c", 0x80000000)
    for t in range(9, 15):
        W2[t] = ("c", 0)
    W2[15] = ("c", 256)
    # the 8 sums are still in registers: seed the cache with them
    sc2 = Sched(g, W2, [r for r in ALL_REGS if r not in st])
    for k in range(8):
        sc2.put_cache(k, st[k])
    for t in range(16, 61):
        sc2.expand(t)
    sc2.flush()
    # ---------------- second SHA-256: rounds 0..60 ----------------
    a0, b0, c0, d0, e0, f0, g0, h0 = IV
    C1 = (h0 + S3(e0) + F1(e0, f0, g0) + K[0]) & M   # T1 - W0
    C2 = (S2(a0) + F0(a0, b0, c0)) & M               # T2
    s2 = ["a2", "a3", "a4", "a5", "a6", "a7", "a8", "a9"]
    w0, ka, ke = "a10", "a11", "a12"
    g.i("l32i    %s, a1, 0" % w0)
    g.const(ka, C1 + C2)
    g.const(ke, d0 + C1)
    g.i("add     %s, %s, %s" % (s2[0], w0, ka))     # new a
    g.i("add     %s, %s, %s" % (s2[4], w0, ke))     # new e
    for r, v in zip([s2[1], s2[2], s2[3], s2[5], s2[6], s2[7]], [a0, b0, c0, e0, f0, g0]):
        g.const(r, v)
    P = "a10"
    g.const(P, a0 ^ b0)                              # b^c for round 1
    rd = Rounds(g, s2, P, ["a11", "a12", "a13", "a14"])
    for t in range(1, 57):
        rd.round(t, W2)
    for t in range(57, 61):
        rd.round(t, W2, full=False)
    # e of round 60 = digest word 7 - IV[7]; prefilter: low 16 bits of
    # (e + 0x5BE0CD19) zero  <=>  (e & 0xFFFF) == 0x32E7
    e60 = rd.st[4]
    g.e60 = e60
    g.i("extui   %s, %s, 0, 16" % ("a11", e60))
    g.const("a12", 0x32E7)
    g.i("movi.n  a2, 0")
    g.i("bne     a11, a12, .Lsw_pf_out")
    g.i("movi.n  a2, 1")
    g.code.append(".Lsw_pf_out:")
    g.i("retw.n")

    head = [
        "// GENERATED by tools/sw_sha/gen_sw_sha.py -- do not edit by hand; change the",
        "// generator and run it again, then tools/sw_sha/test_sw_sha.py.",
        "//",
        "// bool nerd_sha256d_prefilter(const uint32_t *midstate, const uint8_t *tail16,",
        "//                             const uint32_t *bake)",
        "// Software SHA-256d of a block header up to the 16-bit early exit of",
        "// nerd_sha256d_baked(): true exactly when that function would return true.",
        "// Xtensa LX6/LX7, windowed ABI, IRAM. %d instructions on the only path." % sum(
            1 for l in g.code if l.startswith("    ")),
        "",
        "// Used by minerWorkerSw() on the classic ESP32 only: the only target it was",
        "// run on (Espressif QEMU, against the C version and hashlib).",
        "#include \"sdkconfig.h\"",
        "#if defined(CONFIG_IDF_TARGET_ESP32)",
        "    .section .iram1.sw_sha256d, \"ax\"",
        "    .literal_position",
    ]
    for v, name in g.lits.items():
        head.append("    .literal %s, 0x%08x" % (name, v))
    head += [
        "    .balign 4",
        "    .global nerd_sha256d_prefilter",
        "    .type   nerd_sha256d_prefilter, @function",
        "nerd_sha256d_prefilter:",
    ]
    tail = ["    .size   nerd_sha256d_prefilter, .-nerd_sha256d_prefilter", "#endif", ""]
    return "\n".join(head + g.code + tail), g


if __name__ == "__main__":
    text, g = build()
    open(sys.argv[1], "w").write(text)
    n = sum(1 for l in g.code if l.startswith("    "))
    print("instructions:", n, "literals:", len(g.lits))
