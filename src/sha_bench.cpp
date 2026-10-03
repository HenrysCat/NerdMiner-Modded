// On-device bench for the classic-ESP32 SHA engine. Built only with
// -D SHA_BENCH=<mode>; it runs instead of the miner (see setup()) and prints
// to the serial port forever. Every result is checked against software.
//
//   SHA_BENCH=1  this file: how the engine behaves, cycle by cycle
//     T  what the CPU-side register traffic costs
//     A  how long START / CONTINUE / LOAD keep the engine BUSY
//     B  when the engine samples each TEXT word after START / CONTINUE
//     C  when LOAD writes each TEXT word, and what a concurrent CPU store does
//     R  when a digest word written by LOAD can be read back
//   SHA_BENCH=2  sha_bench_var.cpp: mining-loop variants, hash by hash,
//                under core-0 bus traffic
//   SHA_BENCH=3  sha_bench_x.cpp: what the other core's bus traffic breaks
//
// The schedule of the pipelined mining loop
// (src/pipelined_hw_sha_classic_v2.S) is derived from these numbers.
#ifdef SHA_BENCH

#include <Arduino.h>
#include <soc/dport_reg.h>
#include <soc/hwcrypto_reg.h>
#include <driver/periph_ctrl.h>

#define SB        ((volatile uint32_t *)SHA_TEXT_BASE)
#define REG_START ((volatile uint32_t *)SHA_256_START_REG)
#define REG_CONT  ((volatile uint32_t *)SHA_256_CONTINUE_REG)
#define REG_LOAD  ((volatile uint32_t *)SHA_256_LOAD_REG)
#define REG_BUSY  ((volatile uint32_t *)SHA_256_BUSY_REG)

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

extern const uint32_t IV256[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                  0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

// Software reference: one SHA-256 compression over 16 message words given
// exactly as the engine's TEXT registers hold them.
void sb_compress(uint32_t st[8], const uint32_t in[16])
{
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) w[i] = in[i];
    for (int i = 16; i < 64; ++i)
    {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = st[0], b = st[1], c = st[2], d = st[3], e = st[4], f = st[5], g = st[6], h = st[7];
    for (int i = 0; i < 64; ++i)
    {
        uint32_t S1 = ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + K256[i] + w[i];
        uint32_t S0 = ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22);
        uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    st[0] += a; st[1] += b; st[2] += c; st[3] += d; st[4] += e; st[5] += f; st[6] += g; st[7] += h;
}

// One probe per delay D: trigger an engine op, burn exactly D one-cycle
// nops, then either store to a TEXT word or sample BUSY once. These live in
// flash (128 variants don't fit in IRAM), so every measurement is taken
// twice and the second, cache-warm, result is the one kept.
typedef void (*pstore_fn)(volatile uint32_t *trig, volatile uint32_t *dst, uint32_t val, volatile uint32_t *busy);
typedef uint32_t (*pbusy_fn)(volatile uint32_t *trig, volatile uint32_t *busy);

#define DEF_PROBE(D)                                                                                        \
    static __attribute__((noinline)) void pstore_##D(volatile uint32_t *trig,                     \
                                                               volatile uint32_t *dst, uint32_t val,        \
                                                               volatile uint32_t *busy)                     \
    {                                                                                                       \
        uint32_t one = 1, t;                                                                                \
        __asm__ __volatile__(                                                                               \
            "s32i.n %[one], %[trig], 0\n memw\n"                                                            \
            ".rept " #D "\n nop.n\n .endr\n"                                                                \
            "s32i.n %[val], %[dst], 0\n"                                                                    \
            "1: l32i.n %[t], %[busy], 0\n bnez.n %[t], 1b\n"                                                \
            : [t] "=&r"(t)                                                                                  \
            : [one] "r"(one), [trig] "r"(trig), [dst] "r"(dst), [val] "r"(val), [busy] "r"(busy)            \
            : "memory");                                                                                    \
    }                                                                                                       \
    static __attribute__((noinline)) uint32_t pbusy_##D(volatile uint32_t *trig,                  \
                                                                  volatile uint32_t *busy)                  \
    {                                                                                                       \
        uint32_t one = 1, t;                                                                                \
        __asm__ __volatile__(                                                                               \
            "s32i.n %[one], %[trig], 0\n memw\n"                                                            \
            ".rept " #D "\n nop.n\n .endr\n"                                                                \
            "l32i.n %[t], %[busy], 0\n"                                                                     \
            : [t] "=&r"(t)                                                                                  \
            : [one] "r"(one), [trig] "r"(trig), [busy] "r"(busy)                                            \
            : "memory");                                                                                    \
        return t;                                                                                           \
    }

#define ROW(X, h)                                                                                     \
    X(0x##h##0) X(0x##h##1) X(0x##h##2) X(0x##h##3) X(0x##h##4) X(0x##h##5) X(0x##h##6) X(0x##h##7) \
    X(0x##h##8) X(0x##h##9) X(0x##h##a) X(0x##h##b) X(0x##h##c) X(0x##h##d) X(0x##h##e) X(0x##h##f)
#define ALL(X) ROW(X, 0) ROW(X, 1) ROW(X, 2) ROW(X, 3) ROW(X, 4) ROW(X, 5) ROW(X, 6) ROW(X, 7)
#define NDELAY 128

ALL(DEF_PROBE)

#define REF_STORE(D) pstore_##D,
#define REF_BUSY(D) pbusy_##D,
static const pstore_fn s_pstore[NDELAY] = {ALL(REF_STORE)};
static const pbusy_fn s_pbusy[NDELAY] = {ALL(REF_BUSY)};

static inline void wait_idle()
{
    while (*REG_BUSY) {}
}

static void fill(const uint32_t *w)
{
    for (int i = 0; i < 16; ++i) SB[i] = w[i];
}

static void rnd_block(uint32_t *w, uint32_t seed)
{
    uint32_t x = seed * 2654435761u + 12345;
    for (int i = 0; i < 16; ++i)
    {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        w[i] = x;
    }
}

static bool eq8(const uint32_t *a, const uint32_t *b)
{
    for (int i = 0; i < 8; ++i)
        if (a[i] != b[i]) return false;
    return true;
}

static char s_line[NDELAY + 1];

// A: BUSY as seen D cycles after each op ('1' busy, '.' idle).
static void exp_busy()
{
    uint32_t A[16];
    rnd_block(A, 1);
    static const char *names[3] = {"START", "CONT ", "LOAD "};
    volatile uint32_t *regs[3] = {REG_START, REG_CONT, REG_LOAD};
    for (int op = 0; op < 3; ++op)
    {
        for (int d = 0; d < NDELAY; ++d)
        for (int rep = 0; rep < 2; ++rep)
        {
            fill(A);
            if (op >= 1) { *REG_START = 1; wait_idle(); fill(A); }
            if (op >= 2) { *REG_CONT = 1; wait_idle(); }
            portDISABLE_INTERRUPTS();
            uint32_t b = s_pbusy[d](regs[op], REG_BUSY);
            portENABLE_INTERRUPTS();
            wait_idle();
            s_line[d] = b ? '1' : '.';
        }
        s_line[NDELAY] = 0;
        Serial.printf("A busy %s %s\n", names[op], s_line);
    }
}

// B: store to TEXT[i] D cycles after START/CONTINUE. 'o' = engine used the
// old word, 'n' = it used the newly stored word, 'x' = neither (corrupt).
static void exp_sample(bool cont)
{
    uint32_t P[16], A[16], N[16];
    rnd_block(P, 7);
    rnd_block(A, 2);
    static const int idx[] = {0, 1, 2, 4, 7, 8, 9, 12, 14, 15};
    for (unsigned k = 0; k < sizeof(idx) / sizeof(idx[0]); ++k)
    {
        int i = idx[k];
        const uint32_t X = 0xC0FFEE00u + i;
        uint32_t base[8];
        memcpy(base, IV256, sizeof(base));
        if (cont) sb_compress(base, P);
        uint32_t eOld[8], eNew[8];
        memcpy(eOld, base, sizeof(base));
        sb_compress(eOld, A);
        memcpy(N, A, sizeof(N));
        N[i] = X;
        memcpy(eNew, base, sizeof(base));
        sb_compress(eNew, N);

        for (int d = 0; d < NDELAY; ++d)
        for (int rep = 0; rep < 2; ++rep)
        {
            if (cont) { fill(P); *REG_START = 1; wait_idle(); }
            fill(A);
            portDISABLE_INTERRUPTS();
            s_pstore[d](cont ? REG_CONT : REG_START, &SB[i], X, REG_BUSY);
            portENABLE_INTERRUPTS();
            *REG_LOAD = 1;
            wait_idle();
            uint32_t got[8];
            for (int j = 0; j < 8; ++j) got[j] = SB[j];
            s_line[d] = eq8(got, eOld) ? 'o' : (eq8(got, eNew) ? 'n' : 'x');
        }
        s_line[NDELAY] = 0;
        Serial.printf("B %s TEXT[%2d] %s\n", cont ? "CONT " : "START", i, s_line);
    }
}

// C: store to TEXT[j] D cycles after LOAD.
//   j<8 : 'e' engine's digest word won, 'c' CPU's word won, '!' other words damaged
//   j>=8: 'k' digest intact and CPU's word kept, '!' anything else
static void exp_load()
{
    uint32_t A[16];
    rnd_block(A, 3);
    uint32_t dig[8];
    memcpy(dig, IV256, sizeof(dig));
    sb_compress(dig, A);
    static const int idx[] = {0, 1, 3, 6, 7, 8, 11, 15};
    for (unsigned k = 0; k < sizeof(idx) / sizeof(idx[0]); ++k)
    {
        int j = idx[k];
        const uint32_t X = 0xBADC0DE0u + j;
        for (int d = 0; d < 64; ++d)
        for (int rep = 0; rep < 2; ++rep)
        {
            fill(A);
            *REG_START = 1;
            wait_idle();
            portDISABLE_INTERRUPTS();
            s_pstore[d](REG_LOAD, &SB[j], X, REG_BUSY);
            portENABLE_INTERRUPTS();
            uint32_t got[16];
            for (int q = 0; q < 16; ++q) got[q] = SB[q];
            bool others = true;
            for (int q = 0; q < 16; ++q)
            {
                if (q == j) continue;
                uint32_t want = q < 8 ? dig[q] : A[q];
                if (got[q] != want) others = false;
            }
            char c;
            if (!others) c = '!';
            else if (j < 8) c = got[j] == dig[j] ? 'e' : (got[j] == X ? 'c' : '!');
            else c = got[j] == X ? 'k' : '!';
            s_line[d] = c;
        }
        s_line[64] = 0;
        Serial.printf("C LOAD  TEXT[%2d] %s\n", j, s_line);
    }

    // Does a compute leave TEXT alone? Does LOAD leave TEXT[8..15] alone?
    fill(A);
    *REG_START = 1;
    wait_idle();
    bool keep = true;
    for (int q = 0; q < 16; ++q)
        if (SB[q] != A[q]) keep = false;
    *REG_LOAD = 1;
    wait_idle();
    bool keepHi = true;
    for (int q = 8; q < 16; ++q)
        if (SB[q] != A[q]) keepHi = false;
    bool digOk = true;
    for (int q = 0; q < 8; ++q)
        if (SB[q] != dig[q]) digOk = false;
    // Is the internal state still there after LOAD (LOAD twice gives the same digest)?
    for (int q = 0; q < 8; ++q) SB[q] = 0;
    *REG_LOAD = 1;
    wait_idle();
    bool reload = true;
    for (int q = 0; q < 8; ++q)
        if (SB[q] != dig[q]) reload = false;
    Serial.printf("C compute_keeps_TEXT=%d load_keeps_TEXT8_15=%d digest_ok=%d second_load_ok=%d\n",
                  keep, keepHi, digOk, reload);
}

// R: read TEXT[k] D cycles after LOAD: 'n' digest word already there,
// 'o' still the old contents, 'x' something else.
static void exp_loadread()
{
    uint32_t A[16];
    rnd_block(A, 5);
    uint32_t dig[8];
    memcpy(dig, IV256, sizeof(dig));
    sb_compress(dig, A);
    static const int idx[] = {0, 3, 7};
    for (unsigned k = 0; k < 3; ++k)
    {
        int j = idx[k];
        for (int d = 0; d < 32; ++d)
            for (int rep = 0; rep < 2; ++rep)
            {
                fill(A);
                *REG_START = 1;
                wait_idle();
                portDISABLE_INTERRUPTS();
                uint32_t v = s_pbusy[d](REG_LOAD, &SB[j]);
                portENABLE_INTERRUPTS();
                wait_idle();
                s_line[d] = v == dig[j] ? 'n' : (v == A[j] ? 'o' : 'x');
            }
        s_line[32] = 0;
        Serial.printf("R LOAD read TEXT[%d] %s\n", j, s_line);
    }
}

// T: cycle cost of the CPU-side traffic.
static IRAM_ATTR __attribute__((noinline)) uint32_t t_null()
{
    uint32_t a, b;
    __asm__ __volatile__("rsr %[a], ccount\n rsr %[b], ccount\n" : [a] "=&r"(a), [b] "=&r"(b));
    return b - a;
}
static IRAM_ATTR __attribute__((noinline)) uint32_t t_store16(volatile uint32_t *sb, uint32_t v)
{
    uint32_t a, b;
    __asm__ __volatile__(
        "rsr %[a], ccount\n"
        "s32i.n %[v], %[sb], 0\n s32i.n %[v], %[sb], 4\n s32i.n %[v], %[sb], 8\n s32i.n %[v], %[sb], 12\n"
        "s32i.n %[v], %[sb], 16\n s32i.n %[v], %[sb], 20\n s32i.n %[v], %[sb], 24\n s32i.n %[v], %[sb], 28\n"
        "s32i.n %[v], %[sb], 32\n s32i.n %[v], %[sb], 36\n s32i.n %[v], %[sb], 40\n s32i.n %[v], %[sb], 44\n"
        "s32i.n %[v], %[sb], 48\n s32i.n %[v], %[sb], 52\n s32i.n %[v], %[sb], 56\n s32i.n %[v], %[sb], 60\n"
        "rsr %[b], ccount\n"
        : [a] "=&r"(a), [b] "=&r"(b)
        : [v] "r"(v), [sb] "r"(sb)
        : "memory");
    return b - a;
}
static IRAM_ATTR __attribute__((noinline)) uint32_t t_loadstore16(volatile uint32_t *sb, const uint32_t *in)
{
    uint32_t a, b, t;
    __asm__ __volatile__(
        "rsr %[a], ccount\n"
        "l32i.n %[t], %[in], 0\n s32i.n %[t], %[sb], 0\n l32i.n %[t], %[in], 4\n s32i.n %[t], %[sb], 4\n"
        "l32i.n %[t], %[in], 8\n s32i.n %[t], %[sb], 8\n l32i.n %[t], %[in], 12\n s32i.n %[t], %[sb], 12\n"
        "l32i.n %[t], %[in], 16\n s32i.n %[t], %[sb], 16\n l32i.n %[t], %[in], 20\n s32i.n %[t], %[sb], 20\n"
        "l32i.n %[t], %[in], 24\n s32i.n %[t], %[sb], 24\n l32i.n %[t], %[in], 28\n s32i.n %[t], %[sb], 28\n"
        "l32i.n %[t], %[in], 32\n s32i.n %[t], %[sb], 32\n l32i.n %[t], %[in], 36\n s32i.n %[t], %[sb], 36\n"
        "l32i.n %[t], %[in], 40\n s32i.n %[t], %[sb], 40\n l32i.n %[t], %[in], 44\n s32i.n %[t], %[sb], 44\n"
        "l32i.n %[t], %[in], 48\n s32i.n %[t], %[sb], 48\n l32i.n %[t], %[in], 52\n s32i.n %[t], %[sb], 52\n"
        "l32i.n %[t], %[in], 56\n s32i.n %[t], %[sb], 56\n l32i.n %[t], %[in], 60\n s32i.n %[t], %[sb], 60\n"
        "rsr %[b], ccount\n"
        : [a] "=&r"(a), [b] "=&r"(b), [t] "=&r"(t)
        : [in] "r"(in), [sb] "r"(sb)
        : "memory");
    return b - a;
}
static IRAM_ATTR __attribute__((noinline)) uint32_t t_busyread8(volatile uint32_t *busy)
{
    uint32_t a, b, t;
    __asm__ __volatile__(
        "rsr %[a], ccount\n"
        "l32i.n %[t], %[busy], 0\n l32i.n %[t], %[busy], 0\n l32i.n %[t], %[busy], 0\n l32i.n %[t], %[busy], 0\n"
        "l32i.n %[t], %[busy], 0\n l32i.n %[t], %[busy], 0\n l32i.n %[t], %[busy], 0\n l32i.n %[t], %[busy], 0\n"
        "rsr %[b], ccount\n"
        : [a] "=&r"(a), [b] "=&r"(b), [t] "=&r"(t)
        : [busy] "r"(busy)
        : "memory");
    return b - a;
}
static IRAM_ATTR __attribute__((noinline)) uint32_t t_nop16()
{
    uint32_t a, b;
    __asm__ __volatile__(
        "rsr %[a], ccount\n"
        ".rept 16\n nop.n\n .endr\n"
        "rsr %[b], ccount\n"
        : [a] "=&r"(a), [b] "=&r"(b));
    return b - a;
}
static IRAM_ATTR __attribute__((noinline)) uint32_t t_memw8()
{
    uint32_t a, b;
    __asm__ __volatile__(
        "rsr %[a], ccount\n"
        ".rept 8\n memw\n .endr\n"
        "rsr %[b], ccount\n"
        : [a] "=&r"(a), [b] "=&r"(b));
    return b - a;
}

static void exp_cost()
{
    uint32_t A[16];
    rnd_block(A, 4);
    wait_idle();
    uint32_t n = 0, s = 0, ls = 0, br = 0, np = 0, mw = 0;
    for (int r = 0; r < 4; ++r)
    {
        portDISABLE_INTERRUPTS();
        n = t_null();
        s = t_store16(SB, 0);
        ls = t_loadstore16(SB, A);
        br = t_busyread8(REG_BUSY);
        np = t_nop16();
        mw = t_memw8();
        portENABLE_INTERRUPTS();
    }
    Serial.printf("T cycles: null=%u store16=%u loadstore16=%u busyread8=%u nop16=%u memw8=%u\n",
                  n, s - n, ls - n, br - n, np - n, mw - n);
}

#if SHA_BENCH == 2
void sha_bench_variants_pass();
#elif SHA_BENCH == 3
void sha_bench_x_pass();
#endif

static void bench_task(void *)
{
    periph_module_enable(PERIPH_SHA_MODULE);
    periph_module_reset(PERIPH_SHA_MODULE);
    for (;;)
    {
        Serial.printf("\n==== SHA_BENCH begin (core %d, cpu %u MHz) ====\n", xPortGetCoreID(), getCpuFrequencyMhz());
#if SHA_BENCH == 2
        sha_bench_variants_pass();
#elif SHA_BENCH == 3
        sha_bench_x_pass();
#else
        exp_cost();
        exp_busy();
        exp_sample(false);
        exp_sample(true);
        exp_load();
        exp_loadread();
#endif
        Serial.println("==== SHA_BENCH end ====");
        Serial.flush();
        vTaskDelay(4000 / portTICK_PERIOD_MS);
    }
}

void sha_bench_run()
{
    xTaskCreatePinnedToCore(bench_task, "ShaBench", 8192, NULL, 19, NULL, 1);
    for (;;) vTaskDelay(1000 / portTICK_PERIOD_MS);
}

#endif // SHA_BENCH
