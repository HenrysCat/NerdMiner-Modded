#!/usr/bin/env python3
"""Runs the generated Xtensa prefilter in a tiny interpreter and checks it
against hashlib on random block headers (full 32-bit check of the round-60 e
word, plus the true/false result on headers that do pass the prefilter)."""
import hashlib, os, random, re, struct, sys
sys.path.insert(0, os.path.dirname(__file__))
import gen_sw_sha as G

M = 0xFFFFFFFF


def compress(state, block_words):
    W = list(block_words) + [0] * 48
    for t in range(16, 64):
        W[t] = (G.S1(W[t - 2]) + W[t - 7] + G.S0(W[t - 15]) + W[t - 16]) & M
    a, b, c, d, e, f, g, h = state
    es = []
    for t in range(64):
        T1 = (h + G.S3(e) + G.F1(e, f, g) + G.K[t] + W[t]) & M
        T2 = (G.S2(a) + G.F0(a, b, c)) & M
        h, g, f, e, d, c, b, a = g, f, e, (d + T1) & M, c, b, a, (T1 + T2) & M
        es.append(e)
    return [(x + y) & M for x, y in zip(state, [a, b, c, d, e, f, g, h])], es


def bake_of(digest, tail):
    """nerd_sha256_bake() from nerdSHA256plus.cpp."""
    w = struct.unpack(">3I", tail[:12])
    bake = [0] * 15
    bake[0], bake[1], bake[2] = w
    bake[3] = (G.S1(0) + G.S0(bake[1]) + bake[0]) & M
    bake[4] = (G.S1(640) + G.S0(bake[2]) + bake[1]) & M
    A = list(digest)

    def P(ia, ib, ic, id_, ie, if_, ig, ih, x, k):
        t1 = (A[ih] + G.S3(A[ie]) + G.F1(A[ie], A[if_], A[ig]) + k + x) & M
        t2 = (G.S2(A[ia]) + G.F0(A[ia], A[ib], A[ic])) & M
        A[id_] = (A[id_] + t1) & M
        A[ih] = (t1 + t2) & M
    P(0, 1, 2, 3, 4, 5, 6, 7, bake[0], G.K[0])
    P(7, 0, 1, 2, 3, 4, 5, 6, bake[1], G.K[1])
    P(6, 7, 0, 1, 2, 3, 4, 5, bake[2], G.K[2])
    bake[5:13] = A
    bake[13] = (A[4] + G.S3(A[1]) + G.F1(A[1], A[2], A[3]) + G.K[3]) & M
    bake[14] = (G.S2(A[5]) + G.F0(A[5], A[6], A[7])) & M
    return bake


class Interp:
    def __init__(self, text):
        self.lits = {}
        self.prog = []
        self.labels = {}
        for line in text.splitlines():
            s = line.split("//")[0].strip()
            if not s or s.startswith("#"):
                continue
            m = re.match(r"\.literal (\S+), (0x[0-9a-f]+)", s)
            if m:
                self.lits[m.group(1)] = int(m.group(2), 16)
                continue
            if s.startswith("."):
                if s.endswith(":"):
                    self.labels[s[:-1]] = len(self.prog)
                continue
            if s.endswith(":"):
                self.labels[s[:-1]] = len(self.prog)
                continue
            op, _, args = s.partition(" ")
            self.prog.append((op, [a.strip() for a in args.split(",")] if args.strip() else []))

    def run(self, mem, a2, a3, a4, sp=0x3FFF0000):
        R = {"a%d" % i: 0xDEADBEEF for i in range(16)}
        R["a1"], R["a2"], R["a3"], R["a4"] = sp, a2, a3, a4
        sar = 0
        pc = 0
        n = 0

        def v(x):
            return R[x]
        while True:
            op, a = self.prog[pc]
            pc += 1
            n += 1
            if op == "entry":
                R["a1"] = (R["a1"] - int(a[1])) & M
            elif op in ("s32i", "s32i.n"):
                mem[(v(a[1]) + int(a[2])) & M] = v(a[0])
            elif op in ("l32i", "l32i.n"):
                addr = (v(a[1]) + int(a[2])) & M
                R[a[0]] = mem[addr]
            elif op == "l8ui":
                addr = (v(a[1]) + int(a[2])) & M
                R[a[0]] = mem[("b", addr)]
            elif op == "l32r":
                R[a[0]] = self.lits[a[1]]
            elif op in ("movi", "movi.n"):
                R[a[0]] = int(a[1]) & M
            elif op in ("mov", "mov.n"):
                R[a[0]] = v(a[1])
            elif op in ("add", "add.n"):
                R[a[0]] = (v(a[1]) + v(a[2])) & M
            elif op in ("addi", "addi.n"):
                R[a[0]] = (v(a[1]) + int(a[2])) & M
            elif op == "addmi":
                R[a[0]] = (v(a[1]) + int(a[2])) & M
            elif op == "xor":
                R[a[0]] = v(a[1]) ^ v(a[2])
            elif op == "and":
                R[a[0]] = v(a[1]) & v(a[2])
            elif op == "or":
                R[a[0]] = v(a[1]) | v(a[2])
            elif op == "slli":
                R[a[0]] = (v(a[1]) << int(a[2])) & M
            elif op == "srli":
                R[a[0]] = v(a[1]) >> int(a[2])
            elif op == "ssai":
                sar = int(a[0])
            elif op == "src":
                R[a[0]] = (((v(a[1]) << 32) | v(a[2])) >> sar) & M
            elif op == "extui":
                self.checked = v(a[1])   # the round-60 e word, before a2 is reused
                R[a[0]] = (v(a[1]) >> int(a[2])) & ((1 << int(a[3])) - 1)
            elif op == "bne":
                if v(a[0]) != v(a[1]):
                    pc = self.labels[a[2]]
            elif op in ("retw", "retw.n"):
                return R, n
            else:
                raise RuntimeError("unknown op " + op)


def check(interp, header, regs_out=None):
    first = struct.unpack(">16I", header[:64])
    digest, _ = compress(G.IV, first)
    tail = header[64:80]
    bake = bake_of(digest, tail)
    mem = {}
    base_d, base_t, base_b = 0x3FFB0000, 0x3FFB1000, 0x3FFB2000
    for i, x in enumerate(digest):
        mem[base_d + 4 * i] = x
    for i, b in enumerate(tail):
        mem[("b", base_t + i)] = b
    for i, x in enumerate(bake):
        mem[base_b + 4 * i] = x
    R, n = interp.run(mem, base_d, base_t, base_b)
    h = hashlib.sha256(hashlib.sha256(header).digest()).digest()
    expect = (h[30] == 0 and h[31] == 0)
    # full 32-bit check of the round-60 e word against a plain SHA-256
    pad1 = [struct.unpack(">I", header[64 + 4 * i:68 + 4 * i])[0] for i in range(4)] + [0x80000000] + [0] * 10 + [640]
    h1, _ = compress(digest, pad1)
    assert struct.pack(">8I", *h1) == hashlib.sha256(header).digest()
    h2, es = compress(G.IV, h1 + [0x80000000] + [0] * 6 + [256])
    assert struct.pack(">8I", *h2) == h
    e_ok = interp.checked == es[60]
    return R["a2"] == int(expect) and e_ok, expect, n


def main():
    text, g = G.build()
    it = Interp(text)
    it.e60 = g.e60
    rnd = random.Random(1)
    bad = 0
    N = int(sys.argv[1]) if len(sys.argv) > 1 else 300
    # 1) random headers: result must be false (almost always) and match
    for k in range(N):
        hdr = bytes(rnd.getrandbits(8) for _ in range(80))
        ok, exp, n = check(it, hdr)
        if not ok:
            bad += 1
    # 2) headers that DO pass: search nonces with hashlib, then check
    passes = 0
    for k in range(4):
        base = bytearray(rnd.getrandbits(8) for _ in range(80))
        nonce = 0
        while True:
            struct.pack_into("<I", base, 76, nonce)
            h = hashlib.sha256(hashlib.sha256(bytes(base)).digest()).digest()
            if h[30] == 0 and h[31] == 0:
                break
            nonce += 1
        ok, exp, n = check(it, bytes(base))
        passes += 1
        if not ok or not exp:
            bad += 1
        # the next nonce must fail
        struct.pack_into("<I", base, 76, nonce + 1)
        ok2, exp2, _ = check(it, bytes(base))
        if not ok2:
            bad += 1
    print(f"instructions executed per call: {n}")
    print(f"random headers: {N}, passing headers: {passes}, mismatches: {bad}")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
