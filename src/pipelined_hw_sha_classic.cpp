// Pipelined HW SHA256d mining loop for classic ESP32 (D0WD), single inline-asm
// block per inner loop. Ported (MIT license, same upstream) from
// dwespl/nerdminer-axehub's axehub_hw_pipelined_mine_classic(), which itself
// is a fork of BitMaker-hub/NerdMiner_v2 (this project's upstream).
//
// Technique vs. the plain-C classic-ESP32 HW path in mining.cpp: the C path
// fills registers, triggers the SHA engine, then busy-waits doing nothing
// until it's done. This version overlaps that otherwise-wasted busy-wait
// window with the next block's register fill / nonce increment / hash
// counter increment, so the CPU work happens "for free" behind hardware
// latency instead of after it. It also skips re-zeroing TEXT[9..14] on the
// third (digest) block, relying on those registers still holding zero from
// the second block's fill instead of rewriting them.
//
// Returns on candidate hit or mining-flag drop. The caller MUST treat a
// "hit" as unverified and re-check it with the existing software
// nerd_sha256d_baked() path before trusting/submitting it — this function
// only checks the early-reject 16 bits inline and does not compute or
// return the full digest.
#ifdef PIPELINED_ASM_MINING

#include <Arduino.h>
#include <soc/dport_reg.h>
#include <soc/hwcrypto_reg.h>
#include <driver/periph_ctrl.h>

extern "C" IRAM_ATTR bool pipelined_hw_mine_classic(
    volatile uint32_t *sha_base,         // SHA_TEXT_BASE
    const uint32_t *header_swapped,      // 20 words, header bytes 0..79, pre-bswapped
    uint32_t *nonce_swapped_inout,       // IN: starting nonce_swapped. OUT: post-increment.
    volatile uint32_t *hash_count_low,   // attempt counter, incremented in asm
    volatile bool *mining_flag,          // poll: exit when *mining_flag == false
    uint32_t iter_budget                 // unused, kept for call-site symmetry
)
{
    const uint32_t pad32     = 0x80000000u;  // SHA padding bit
    const uint32_t len_blk2  = 0x00000280u;  // 640 bits = first SHA input length (block1+block2)
    const uint32_t len_blk3  = 0x00000100u;  // 256 bits = digest1 length for second SHA
    (void)iter_budget;

    // Per-call peripheral kick to flush any leaked state from prior context
    // (TLS/mbedtls/other tasks sharing the engine). Uses the official
    // periph_module_reset() API (internally spinlock-protected) rather than
    // poking DPORT_PERI_CLK_EN_REG/DPORT_PERI_RST_EN_REG directly: those are
    // registers shared across many peripherals, and a raw, unprotected
    // read-modify-write on them here can race against anything else in the
    // system doing the same (e.g. the display driver enabling its own SPI
    // peripheral), corrupting an unrelated bit. That raced/corrupted-bit
    // scenario is the leading theory for an intermittent display freeze
    // observed during soak testing.
    periph_module_reset(PERIPH_SHA_MODULE);

    __asm__ __volatile__(
        "l32i.n   a2,  %[nonce], 0  \n"     // a2 = nonce_swapped (live nonce, post-incremented per iter)
        "addi     a5,  %[sb], 0x90  \n"     // a5 = SHA_256_START_REG
                                            //      a5+0 = START, a5+4 = CONT, a5+8 = LOAD, a5+12 = BUSY
        "movi.n   a8,  0            \n"     // a8 = 0 const for zero-stores

    "ml_start:                      \n"
        // ===== BLOCK-1 fill (16 stores TEXT[0..15] = header_swapped[0..15]) =====
        "l32i.n   a3,  %[in],  0    \n"     "s32i.n   a3,  %[sb],  0    \n"
        "l32i.n   a3,  %[in],  4    \n"     "s32i.n   a3,  %[sb],  4    \n"
        "l32i.n   a3,  %[in],  8    \n"     "s32i.n   a3,  %[sb],  8    \n"
        "l32i.n   a3,  %[in], 12    \n"     "s32i.n   a3,  %[sb], 12    \n"
        "l32i.n   a3,  %[in], 16    \n"     "s32i.n   a3,  %[sb], 16    \n"
        "l32i.n   a3,  %[in], 20    \n"     "s32i.n   a3,  %[sb], 20    \n"
        "l32i.n   a3,  %[in], 24    \n"     "s32i.n   a3,  %[sb], 24    \n"
        "l32i.n   a3,  %[in], 28    \n"     "s32i.n   a3,  %[sb], 28    \n"
        "l32i.n   a3,  %[in], 32    \n"     "s32i.n   a3,  %[sb], 32    \n"
        "l32i.n   a3,  %[in], 36    \n"     "s32i.n   a3,  %[sb], 36    \n"
        "l32i.n   a3,  %[in], 40    \n"     "s32i.n   a3,  %[sb], 40    \n"
        "l32i.n   a3,  %[in], 44    \n"     "s32i.n   a3,  %[sb], 44    \n"
        "l32i.n   a3,  %[in], 48    \n"     "s32i.n   a3,  %[sb], 48    \n"
        "l32i.n   a3,  %[in], 52    \n"     "s32i.n   a3,  %[sb], 52    \n"
        "l32i.n   a3,  %[in], 56    \n"     "s32i.n   a3,  %[sb], 56    \n"
        "l32i.n   a3,  %[in], 60    \n"     "s32i.n   a3,  %[sb], 60    \n"

        // START block-1
        "movi.n   a3, 1             \n"
        "s32i.n   a3, a5, 0         \n"
        "memw                       \n"

        // ===== BLOCK-2 fill OVERLAPPED with block-1 compute =====
        // TEXT[0..2] = header_swapped[16..18] (header bytes 64..75)
        "l32i     a3,  %[in], 64    \n"     "s32i.n   a3,  %[sb],  0    \n"
        "l32i     a3,  %[in], 68    \n"     "s32i.n   a3,  %[sb],  4    \n"
        "l32i     a3,  %[in], 72    \n"     "s32i.n   a3,  %[sb],  8    \n"
        // TEXT[3] = nonce_swapped (live in a2)
        "s32i.n   a2,  %[sb], 12    \n"
        // TEXT[4] = 0x80 padding bit
        "s32i.n   %[pad], %[sb], 16 \n"
        // TEXT[5..14] = 0
        "s32i.n   a8,  %[sb], 20    \n"
        "s32i.n   a8,  %[sb], 24    \n"
        "s32i.n   a8,  %[sb], 28    \n"
        "s32i.n   a8,  %[sb], 32    \n"
        "s32i.n   a8,  %[sb], 36    \n"
        "s32i.n   a8,  %[sb], 40    \n"
        "s32i.n   a8,  %[sb], 44    \n"
        "s32i.n   a8,  %[sb], 48    \n"
        "s32i.n   a8,  %[sb], 52    \n"
        "s32i.n   a8,  %[sb], 56    \n"
        // TEXT[15] = 0x280 (input length 640 bits for first SHA)
        "s32i.n   %[len2], %[sb], 60 \n"

        // ===== WAIT block-1 done (BUSY=0 poll) =====
    "ml_w1:                         \n"
        "l32i.n   a3, a5, 12        \n"
        "bnez.n   a3, ml_w1         \n"

        // ===== CONTINUE block-2 =====
        "movi.n   a3, 1             \n"
        "s32i.n   a3, a5, 4         \n"
        "memw                       \n"

    "ml_w2:                         \n"
        "l32i.n   a4, a5, 12        \n"
        "bnez.n   a4, ml_w2         \n"

        // ===== LOAD1 (digest1 -> TEXT[0..7]; preserves TEXT[8..15]) =====
        "movi.n   a4, 1             \n"
        "s32i.n   a4, a5, 8         \n"
        "memw                       \n"

        // OVERLAP: increment nonce during LOAD1
        "addi.n   a2, a2, 1         \n"

    "ml_w3:                         \n"
        "l32i.n   a4, a5, 12        \n"
        "bnez.n   a4, ml_w3         \n"

        // BLOCK-3 fill: only TEXT[8] (pad 0x80) and TEXT[15] (256-bit length).
        // TEXT[0..7] = digest1 from LOAD1; TEXT[9..14] persist as 0 from block-2.
        "s32i.n   %[pad],  %[sb], 32 \n"
        "s32i.n   %[len3], %[sb], 60 \n"

        // ===== START block-3 (second SHA over digest1) =====
        "movi.n   a4, 1             \n"
        "s32i.n   a4, a5, 0         \n"
        "memw                       \n"

        // OVERLAP: hash counter increment during block-3 compute
        "l32i.n   a3, %[hcnt], 0    \n"
        "addi.n   a3, a3, 1         \n"
        "s32i.n   a3, %[hcnt], 0    \n"

    "ml_w4:                         \n"
        "l32i.n   a4, a5, 12        \n"
        "bnez.n   a4, ml_w4         \n"

        // ===== LOAD2 final (digest2 -> TEXT[0..7]) =====
        "movi.n   a3, 1             \n"
        "s32i.n   a3, a5, 8         \n"
        "memw                       \n"

    "ml_w5:                         \n"
        "l32i.n   a4, a5, 12        \n"
        "bnez.n   a4, ml_w5         \n"

        // ===== Check mining flag (exit if mining stopped) =====
        "l8ui     a3, %[flag], 0    \n"
        "beqz.n   a3, ml_end        \n"

        // EARLY REJECT: low 16 bits of TEXT[7] == 0 = HIT, != 0 = MISS.
        "l16ui    a3, %[sb], 28     \n"
        "beqz.n   a3, ml_end        \n"

        // MISS: continue inner loop
        "j ml_start                 \n"

    "ml_end:                        \n"
        "s32i.n   a2, %[nonce], 0   \n"     // save current nonce_swapped (post-increment if hit)

        :
        : [sb]    "r"(sha_base),
          [in]    "r"(header_swapped),
          [hcnt]  "r"(hash_count_low),
          [nonce] "r"(nonce_swapped_inout),
          [flag]  "r"(mining_flag),
          [pad]   "r"(pad32),
          [len2]  "r"(len_blk2),
          [len3]  "r"(len_blk3)
        : "a2", "a3", "a4", "a5", "a8", "memory"
    );

    // Two exit paths from asm:
    //   1. mining_flag became false -> l8ui beqz -> ml_end. Returns false.
    //   2. l16ui != 0 (hit) -> fallthrough to ml_end. Returns true (mining_flag still true).
    return *mining_flag;
}

// Cycle SHA peripheral reset (drops any sticky H state). Call after every
// candidate hit to avoid stale-state duplicate-share rejects. See the
// comment in pipelined_hw_mine_classic() above for why this goes through
// periph_module_reset() instead of raw DPORT register pokes.
extern "C" IRAM_ATTR void pipelined_hw_mine_classic_reinit(void)
{
    periph_module_reset(PERIPH_SHA_MODULE);
}

#endif // PIPELINED_ASM_MINING
