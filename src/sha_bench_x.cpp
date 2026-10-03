// Cross-core interference experiments for the SHA bench (-D SHA_BENCH=3).
//
// Core 1 runs one ingredient of the mining loop at a time on the SHA
// registers while core 0 hammers one kind of bus traffic, and every result
// is checked. This is where rule 1 at the top of
// src/pipelined_hw_sha_classic_v2.S comes from:
//
//   W  eight register stores + START, with 0/1/2/4 instructions between
//      consecutive stores. With none, stores get lost as soon as core 0
//      touches DPORT; with one or more, never.
//   Z  BUSY sampled while the engine is certainly busy (interrupts off, 12
//      to ~40 cycles into a 68-cycle compute): how often it reads idle.
//      (A handful per 400k on a quiet chip, none under load.)
//   Y  engine sequences written the slow, obviously-correct way (generous
//      delays, no status reads): rewriting TEXT mid-compute, rewriting it
//      right behind START, START + CONTINUE. These survive everything, so
//      the engine itself is not what the other core disturbs.
#if defined(SHA_BENCH) && SHA_BENCH == 3

#include <Arduino.h>
#include <soc/dport_reg.h>
#include <soc/hwcrypto_reg.h>
#include <driver/periph_ctrl.h>

extern const uint32_t IV256[8];
void sb_compress(uint32_t st[8], const uint32_t in[16]);

#define SB        ((volatile uint32_t *)SHA_TEXT_BASE)
#define REG_START ((volatile uint32_t *)SHA_256_START_REG)
#define REG_CONT  ((volatile uint32_t *)SHA_256_CONTINUE_REG)
#define REG_LOAD  ((volatile uint32_t *)SHA_256_LOAD_REG)
#define REG_BUSY  ((volatile uint32_t *)SHA_256_BUSY_REG)

static volatile int s_mode = 0;
static const char *const s_modeName[] = {"quiet", "DPORT read", "APB read", "APB write", "DPORT write", "RAM only"};
#define NMODE (sizeof(s_modeName) / sizeof(s_modeName[0]))

static void IRAM_ATTR stress_task(void *)
{
    volatile uint32_t sink = 0;
    static volatile uint32_t ram[16];
    for (;;)
    {
        int m = s_mode;
        if (m == 0) { vTaskDelay(2); continue; }
        for (int i = 0; i < 20000; ++i)
        {
            switch (m)
            {
            case 1: sink += *(volatile uint32_t *)DPORT_PERI_CLK_EN_REG; break;
            case 2: sink += *(volatile uint32_t *)0x3FF44004; break; // GPIO_OUT_REG
            case 3: *(volatile uint32_t *)0x3FF4400C = 0; break;     // GPIO_OUT_W1TC_REG, no bits
            case 4: *(volatile uint32_t *)0x3FF000C8 = 0; break;     // DPORT_CPU_INTR_FROM_CPU_3_REG, unused
            case 5: ram[i & 15] += i; break;
            }
        }
        vTaskDelay(1);
    }
}

static inline uint32_t rd3(volatile uint32_t *reg)
{
    uint32_t a = *reg, b = *reg, c = *reg;
    return (a == b || a == c) ? a : b;
}

static inline void IRAM_ATTR spin(int n)
{
    for (volatile int k = 0; k < n; ++k) {}
}

static bool IRAM_ATTR digest_is(const uint32_t *expect)
{
    for (int i = 0; i < 8; ++i)
        if (rd3(&SB[i]) != expect[i]) return false;
    return true;
}

// ---- W: TEXT[0..7] = w[0..7] and START, GAP nops between stores ----
#define DEF_BURST(NAME, GAP)                                                                         \
    static void IRAM_ATTR __attribute__((noinline)) NAME(volatile uint32_t *sb, const uint32_t *w)    \
    {                                                                                                \
        uint32_t r0 = w[0], r1 = w[1], r2 = w[2], r3 = w[3], r4 = w[4], r5 = w[5], r6 = w[6], r7 = w[7]; \
        uint32_t one = 1;                                                                            \
        __asm__ __volatile__(                                                                        \
            "s32i.n %[r0], %[sb], 0\n  .rept " #GAP "\n nop.n\n .endr\n"                             \
            "s32i.n %[r1], %[sb], 4\n  .rept " #GAP "\n nop.n\n .endr\n"                             \
            "s32i.n %[r2], %[sb], 8\n  .rept " #GAP "\n nop.n\n .endr\n"                             \
            "s32i.n %[r3], %[sb], 12\n .rept " #GAP "\n nop.n\n .endr\n"                             \
            "s32i.n %[r4], %[sb], 16\n .rept " #GAP "\n nop.n\n .endr\n"                             \
            "s32i.n %[r5], %[sb], 20\n .rept " #GAP "\n nop.n\n .endr\n"                             \
            "s32i.n %[r6], %[sb], 24\n .rept " #GAP "\n nop.n\n .endr\n"                             \
            "s32i.n %[r7], %[sb], 28\n .rept " #GAP "\n nop.n\n .endr\n"                             \
            "s32i   %[one], %[sb], 0x90\n"                                                           \
            :                                                                                        \
            : [sb] "r"(sb), [r0] "r"(r0), [r1] "r"(r1), [r2] "r"(r2), [r3] "r"(r3), [r4] "r"(r4),    \
              [r5] "r"(r5), [r6] "r"(r6), [r7] "r"(r7), [one] "r"(one)                               \
            : "memory");                                                                             \
    }
DEF_BURST(burst_gap0, 0)
DEF_BURST(burst_gap1, 1)
DEF_BURST(burst_gap2, 2)
DEF_BURST(burst_gap4, 4)

typedef void (*burst_fn)(volatile uint32_t *, const uint32_t *);

static uint32_t IRAM_ATTR x_burst(burst_fn fn, const uint32_t *blk, const uint32_t *dig, uint32_t n)
{
    uint32_t bad = 0;
    for (uint32_t k = 0; k < n; ++k)
    {
        for (int i = 0; i < 8; ++i) { SB[i] = 0; spin(1); }
        for (int i = 8; i < 16; ++i) { SB[i] = blk[i]; spin(1); }
        fn(SB, blk);
        spin(40);
        *REG_LOAD = 1;
        spin(10);
        if (!digest_is(dig)) bad++;
    }
    return bad;
}

// ---- Z: BUSY while the engine is busy ----
static uint32_t IRAM_ATTR x_busy(uint32_t n)
{
    uint32_t idle = 0;
    for (uint32_t k = 0; k < n; ++k)
    {
        for (int i = 0; i < 16; ++i) { SB[i] = k + i; spin(1); }
        portDISABLE_INTERRUPTS();
        *REG_START = 1;
        __asm__ __volatile__(".rept 12\n nop.n\n .endr\n");
        uint32_t a = *REG_BUSY, b = *REG_BUSY, c = *REG_BUSY, d = *REG_BUSY;
        portENABLE_INTERRUPTS();
        idle += !a + !b + !c + !d;
        spin(40);
    }
    return idle;
}

// ---- Y: slow engine sequences ----
//   1: TEXT[8..15] rewritten in the middle of a compute
//   2: whole TEXT rewritten right behind START (what the block-2 fill does)
//   3: START + CONTINUE, TEXT only written while idle
//   4: 2 with a CONTINUE over the rewritten TEXT
static uint32_t IRAM_ATTR x_mix(int which, const uint32_t *blk, const uint32_t *blk2, const uint32_t *d1,
                                const uint32_t *d12, uint32_t n)
{
    uint32_t bad = 0;
    for (uint32_t k = 0; k < n; ++k)
    {
        for (int i = 0; i < 16; ++i) { SB[i] = blk[i]; spin(1); }
        *REG_START = 1;
        const uint32_t *expect = d1;
        switch (which)
        {
        case 1:
            spin(3);
            for (int i = 8; i < 16; ++i) { SB[i] = blk2[i]; spin(1); }
            spin(40);
            break;
        case 2:
            for (int i = 0; i < 16; ++i) { SB[i] = blk2[i]; spin(1); }
            spin(40);
            break;
        case 3:
            spin(40);
            for (int i = 0; i < 16; ++i) { SB[i] = blk2[i]; spin(1); }
            *REG_CONT = 1;
            spin(40);
            expect = d12;
            break;
        case 4:
            for (int i = 0; i < 16; ++i) { SB[i] = blk2[i]; spin(1); }
            spin(40);
            *REG_CONT = 1;
            spin(40);
            expect = d12;
            break;
        }
        *REG_LOAD = 1;
        spin(10);
        if (!digest_is(expect)) bad++;
    }
    return bad;
}

void sha_bench_x_pass()
{
    static bool started = false;
    if (!started)
    {
        started = true;
        xTaskCreatePinnedToCore(stress_task, "Stress", 4096, NULL, 1, NULL, 0);
    }
    uint32_t blk[16], blk2[16], dig[8], d12[8];
    uint32_t x = 0x1234567;
    for (int i = 0; i < 16; ++i)
    {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        blk[i] = x;
        blk2[i] = ~x + i;
    }
    memcpy(dig, IV256, sizeof(dig));
    sb_compress(dig, blk);
    memcpy(d12, dig, sizeof(d12));
    sb_compress(d12, blk2);

    static const burst_fn bursts[4] = {burst_gap0, burst_gap1, burst_gap2, burst_gap4};
    static const int gaps[4] = {0, 1, 2, 4};
    for (unsigned m = 0; m < NMODE; ++m)
    {
        s_mode = m;
        vTaskDelay(30);
        Serial.printf("W %-11s wrong digests per 100k after 8 stores + START:", s_modeName[m]);
        for (int g = 0; g < 4; ++g)
            Serial.printf("  gap %d: %u", gaps[g], x_burst(bursts[g], blk, dig, 100000));
        Serial.println();
        Serial.printf("Z %-11s BUSY read as idle during a compute: %u of 400k reads\n", s_modeName[m], x_busy(100000));
        Serial.printf("Y %-11s wrong digests per 100k: mid-compute rewrite %u, rewrite behind START %u, START+CONTINUE %u, both %u\n",
                      s_modeName[m], x_mix(1, blk, blk2, dig, d12, 100000), x_mix(2, blk, blk2, dig, d12, 100000),
                      x_mix(3, blk, blk2, dig, d12, 100000), x_mix(4, blk, blk2, dig, d12, 100000));
    }
    s_mode = 0;
}

#endif
