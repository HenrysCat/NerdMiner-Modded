// Pipelined HW SHA256d mining loop for classic ESP32 (D0WD), single inline-asm
// block per inner loop. Ported (MIT license, same upstream) from
// dwespl/nerdminer-axehub's axehub_hw_pipelined_mine_classic(), which itself
// is a fork of BitMaker-hub/NerdMiner_v2 (this project's upstream).
//
// SUPERSEDED by src/pipelined_hw_sha_classic_v2.S, which is ~37% faster and
// explains the corruption this file could only work around: it isn't TEXT
// stores in busy windows as such, it is stores less than three cycles apart,
// which get dropped when the other core is on the DPORT bus. This loop has
// such stores (rarely fatal: ~1 wrong hash in a few thousand with a busy
// core 0), so it is only kept as the last fallback for a chip on which the
// measured schedule of the new loop fails its self-test.
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
// Further refinements over the ported original: the hash counter lives in
// a register (a7) and is stored back only at exit (the caller only reads it
// after this function returns), the budget decrement / mining-flag load
// hide in the block-3 compute window (register/DRAM work only -- safe),
// and the serial block-1 fill batches its load/store pairs two-at-a-time.
// Do NOT try to hide TEXT stores in engine-busy windows beyond what the
// loop already does: every attempted variant measurably corrupted hashes
// (silently losing shares); see the measured-corruption table at the
// block-3 overlap comment inside the loop.
//
// Returns on candidate hit, iter_budget exhaustion, or mining-flag drop --
// the latter two both signal "no hit" via *mining_flag so the caller can't
// mistake a budget stop for a found candidate (important: without budget
// enforcement this used to run ~65536 iterations on average before
// returning, happily overshooting whatever nonce range the caller meant to
// allocate for one job chunk and re-scanning ground another queued chunk
// would cover later -- deterministically rediscovering, and resubmitting,
// the same nonce twice instead of searching new ground).
//
// The caller MUST also treat a "hit" as unverified and re-check it with the
// existing software nerd_sha256d_baked() path before trusting/submitting it
// -- this function only checks the early-reject 16 bits inline and does not
// compute or return the full digest.
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
    uint32_t iter_budget                 // max iterations this call; enforced in asm (see a6)
)
{
    // pad32 (0x80000000), len_blk2 (640) and len_blk3 (256) used to be
    // passed in as register operands, but combined with the budget operand
    // added below that's more distinct registers than Xtensa's window has
    // room for ("can't find a register in class 'RL_REGS'"). They're
    // computed inline from immediates instead (see a3 uses below) --  one
    // fewer register per call is worth 1-2 extra cheap instructions.

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
        "or       a6,  %[budget], %[budget] \n"  // a6 = remaining-iterations budget (copy; %[budget]'s
                                                  //      own register isn't safe to keep clobbering directly)
        "l32i.n   a7,  %[hcnt], 0   \n"     // a7 = hash counter, kept in-register for the whole call
                                            //      (caller only reads *hash_count_low after we return;
                                            //      stored back once at ml_end)

    "ml_start:                      \n"
        // ===== BLOCK-1 fill (16 stores TEXT[0..15] = header_swapped[0..15]).
        // This MUST stay on the serial path: hardware measurement (see the
        // integrity comment at the block-3 overlap below) shows any TEXT
        // store issued while the engine is BUSY -- compute OR load -- can
        // corrupt hashes or the digest transfer. Load/store pairs are
        // batched two-at-a-time (a3+a4) so the second load's latency hides
        // behind the first store instead of serializing with it. a4 is free
        // here: the mining-flag preload it carries is consumed in the exit
        // checks before the loop re-enters. =====
        "l32i.n   a3,  %[in],  0    \n"     "l32i.n   a4,  %[in],  4    \n"
        "s32i.n   a3,  %[sb],  0    \n"     "s32i.n   a4,  %[sb],  4    \n"
        "l32i.n   a3,  %[in],  8    \n"     "l32i.n   a4,  %[in], 12    \n"
        "s32i.n   a3,  %[sb],  8    \n"     "s32i.n   a4,  %[sb], 12    \n"
        "l32i.n   a3,  %[in], 16    \n"     "l32i.n   a4,  %[in], 20    \n"
        "s32i.n   a3,  %[sb], 16    \n"     "s32i.n   a4,  %[sb], 20    \n"
        "l32i.n   a3,  %[in], 24    \n"     "l32i.n   a4,  %[in], 28    \n"
        "s32i.n   a3,  %[sb], 24    \n"     "s32i.n   a4,  %[sb], 28    \n"
        "l32i.n   a3,  %[in], 32    \n"     "l32i.n   a4,  %[in], 36    \n"
        "s32i.n   a3,  %[sb], 32    \n"     "s32i.n   a4,  %[sb], 36    \n"
        "l32i.n   a3,  %[in], 40    \n"     "l32i.n   a4,  %[in], 44    \n"
        "s32i.n   a3,  %[sb], 40    \n"     "s32i.n   a4,  %[sb], 44    \n"
        "l32i.n   a3,  %[in], 48    \n"     "l32i.n   a4,  %[in], 52    \n"
        "s32i.n   a3,  %[sb], 48    \n"     "s32i.n   a4,  %[sb], 52    \n"
        "l32i.n   a3,  %[in], 56    \n"     "l32i.n   a4,  %[in], 60    \n"
        "s32i.n   a3,  %[sb], 56    \n"     "s32i.n   a4,  %[sb], 60    \n"

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
        // TEXT[4] = 0x80 padding bit (1 << 31, computed: movi immediate
        // can't hold 0x80000000 directly)
        "movi.n   a3, 1             \n"
        "slli     a3, a3, 31        \n"
        "s32i.n   a3, %[sb], 16     \n"
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
        // TEXT[15] = 640 (input length in bits for first SHA)
        "movi     a3, 640           \n"
        "s32i.n   a3, %[sb], 60     \n"

        // ===== WAIT block-1 done (BUSY=0 poll) =====
    "ml_w1:                         \n"
        "l32i.n   a3, a5, 12        \n"
        "bnez.n   a3, ml_w1         \n"

        // ===== CONTINUE block-2 =====
        "movi.n   a3, 1             \n"
        "s32i.n   a3, a5, 4         \n"
        "memw                       \n"

    "ml_w2:                         \n"
        "l32i.n   a3, a5, 12        \n"
        "bnez.n   a3, ml_w2         \n"

        // ===== LOAD1 (digest1 -> TEXT[0..7]; preserves TEXT[8..15]) =====
        "movi.n   a3, 1             \n"
        "s32i.n   a3, a5, 8         \n"
        "memw                       \n"

        // OVERLAP: increment nonce during LOAD1
        "addi.n   a2, a2, 1         \n"

    "ml_w3:                         \n"
        "l32i.n   a3, a5, 12        \n"
        "bnez.n   a3, ml_w3         \n"

        // BLOCK-3 fill: only TEXT[8] (pad 0x80) and TEXT[15] (256-bit length).
        // TEXT[0..7] = digest1 from LOAD1; TEXT[9..14] persist as 0 from block-2.
        "movi.n   a3, 1             \n"
        "slli     a3, a3, 31        \n"
        "s32i.n   a3, %[sb], 32     \n"
        "movi     a3, 256           \n"
        "s32i.n   a3, %[sb], 60     \n"

        // ===== START block-3 (second SHA over digest1) =====
        "movi.n   a3, 1             \n"
        "s32i.n   a3, a5, 0         \n"
        "memw                       \n"

        // OVERLAP (block-3 compute window): hash counter / budget / flag
        // work. Register/DRAM-only -- deliberately NO TEXT stores here or
        // in any other engine-BUSY window beyond what the original loop
        // already did. Measured on hardware (PLDBG counters, 150s runs
        // each), attempts to hide the next block-1's TEXT[8..15] fill in
        // busy windows all corrupted hashes:
        //   - right after START-3:            mismatch/hits = 119/1593 (~7.5%)
        //   - after a 19-cycle delay:         mismatch/hits = 181/1671 (~10.8%)
        //   - during the LOAD2 window:        real-hits/hits = 74/1554 (~5%!!)
        //   - baseline (no busy-window fill): mismatch 0/1431
        // Explanation consistent with all four: the engine reads TEXT word
        // t as compute round t reaches it, and a LOAD's H->TEXT transfer is
        // also disruptable mid-flight. The long-standing block-2 fill
        // during block-1 compute survives only because it starts at TEXT[0]
        // *behind* the engine's read pointer and never overtakes it; a fill
        // starting mid-array at TEXT[8] starts *ahead* of the pointer and
        // loses the race on DPORT bus jitter. Acceptance bar for touching
        // any of this scheduling: re-run with DEBUG_MINING and require
        // mismatch=0 AND real==hits.
        "addi.n   a7, a7, 1         \n"     // hash counter (in-register)
        "addi     a6, a6, -1        \n"     // budget decrement (tested after w5)
        "l8ui     a4, %[flag], 0    \n"     // a4 = *mining_flag preload (tested after w5)

    "ml_w4:                         \n"
        "l32i.n   a3, a5, 12        \n"
        "bnez.n   a3, ml_w4         \n"

        // ===== LOAD2 final (digest2 -> TEXT[0..7]) =====
        "movi.n   a3, 1             \n"
        "s32i.n   a3, a5, 8         \n"
        "memw                       \n"

    "ml_w5:                         \n"
        "l32i.n   a3, a5, 12        \n"
        "bnez.n   a3, ml_w5         \n"

        // ===== Budget check: stop after iter_budget iterations even with
        // no hit. Without this, a single call runs until the ~1/65536
        // early-reject fires (tens of thousands of iterations), which can
        // overshoot the nonce range the caller allocated for this job chunk
        // and wander into a range another queued chunk will scan later --
        // deterministically rediscovering (and resubmitting) the same
        // nonce twice instead of searching new ground. On exhaustion this
        // signals "no hit" via *mining_flag, distinctly from a real hit, so
        // the caller can't mistake a budget stop for a found candidate.
        // (Decrement itself happened in the block-3 overlap window above.) =====
        "bnez.n   a6, ml_budget_ok  \n"
        "s8i      a8, %[flag], 0    \n"     // *mining_flag = 0 (false): signals "no hit" to caller
        "j        ml_end            \n"
    "ml_budget_ok:                  \n"

        // ===== Check mining flag (preloaded into a4 in the w2 window) =====
        "beqz.n   a4, ml_end        \n"

        // EARLY REJECT: low 16 bits of TEXT[7] == 0 = HIT, != 0 = MISS.
        "l16ui    a3, %[sb], 28     \n"
        "beqz.n   a3, ml_end        \n"

        // MISS: continue inner loop
        "j ml_start                 \n"

    "ml_end:                        \n"
        "s32i.n   a2, %[nonce], 0   \n"     // save current nonce_swapped (post-increment if hit)
        "s32i.n   a7, %[hcnt], 0    \n"     // write back in-register hash counter

        :
        : [sb]    "r"(sha_base),
          [in]    "r"(header_swapped),
          [hcnt]  "r"(hash_count_low),
          [nonce] "r"(nonce_swapped_inout),
          [flag]  "r"(mining_flag),
          [budget] "r"(iter_budget)
        : "a2", "a3", "a4", "a5", "a6", "a7", "a8", "memory"
    );

    // Three exit paths from asm, all converging on ml_end:
    //   1. Budget exhausted -> asm itself sets *mining_flag=0 -> returns false.
    //   2. mining_flag became false (checked, not set, by the asm) -> false.
    //   3. l16ui != 0 (hit) -> fallthrough to ml_end. Returns true.
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
