# CLAUDE.md — NerdMiner-Modded

Context for Claude Code sessions in this repo. Read this before touching the mining path.

## What this project is

A fork of [NerdMiner_v2](https://github.com/BitMaker-hub/NerdMiner_v2) (BitMaker-hub), tuned for the
**ESP32-2432S028R "Cheap Yellow Display" (CYD)** — classic ESP32 (Xtensa LX6, dual core, 240 MHz).
It is a solo Bitcoin lottery miner and a desk gadget; nothing here changes the odds meaningfully.

Owner: Leigh (GitHub `HenrysCat`). Only the CYD is tested. Other boards build from the same tree
but are untested with these changes.

Headline change: the classic-ESP32 SHA-256d path drives the **hardware SHA engine in a pipelined
assembly loop**, sustained ~990 kH/s total (~970 kH/s HW loop on core 1 + ~25–30 kH/s software on
core 0), every hash validated against software on the bench.

Also: "Pulse" UI theme (MINE / MARKET / CLOCK screens), classic theme still buildable, BTC price
currency selection, improved web settings page.

## Build / flash

```
pio run -e ESP32-2432S028R -t upload           # Pulse theme, pipelined asm miner
pio run -e ESP32-2432S028R-Classic -t upload   # original look
```

Both CYD envs set `-D PIPELINED_ASM_MINING=1` in `platformio.ini`.
Web flasher for users: https://henryscat.github.io/NerdMiner-Modded/ (ESP Web Tools, full install
or firmware-only update).

## Key files

| File | What it is |
|---|---|
| `src/pipelined_hw_sha_classic_v2.S` | **The hot loop.** `PL_MINE` macro + three instances: `pipelined_hw_mine_classic_v2` (fast, delays at threshold), `_margin` (+2 on every trigger delay, first fallback) and `_wide` (+14, second fallback). Read its header comment before editing. |
| `src/pipelined_hw_sha_classic.cpp` | C side of the pipelined path. |
| `src/mining.cpp` | Workers. `minerWorkerHw` (classic ESP32 variant ~L1521) runs the asm loop; `minerWorkerSw` runs software SHA. `pl_selftest()` (~L1441) known-answer test at power-on; loop table `s_pl_loops` (~L1427) falls back fast → +2 margins → wide → polled → plain (`PL_FORCE_LOOP=<n>` pins one for A/B tests). |
| `src/NerdMinerV2.ino.cpp` | Task creation. HW miner pinned to core 1 at priority 19; SW miner on core 0 at priority 1 alongside WiFi/stratum/monitor. |
| `src/ShaTests/nerdSHA256plus.cpp` | Software SHA: `nerd_sha256d_baked()` — midstate + "bake" precompute of rounds 0–3, round-60 early exit on `(a7 & 0xFFFF) != 0x32E7`. Used on core 0 and to re-verify every HW candidate. |
| `src/sha_bench*.cpp`, `src/sha_bench_variants.{h,inc}` | On-device bench (see below). |
| `tools/sha_bench/gen_variants.py` | Generates `sha_bench_variants.inc/.h` from a sweep file. |
| `tools/sha_bench/sweep_default.py` | Default sweep: production schedule (delays at threshold), both fallbacks, each delay one cycle below (must fail), two-cycle store spacing (must fail). |
| `tools/sha_bench/summarize.py` | Summarises bench serial output. |

## The bench (non-negotiable for any hot-loop change)

Build with `-D SHA_BENCH=<mode>`; the bench runs instead of the miner and prints over serial.

- `SHA_BENCH=1` — `sha_bench.cpp`: engine behaviour cycle by cycle.
- `SHA_BENCH=2` — `sha_bench_var.cpp`: every variant in `sha_bench_variants.inc` checked
  **hash by hash against software**, under each core-0 stress mode.
- `SHA_BENCH=3` — `sha_bench_x.cpp`: what the other core's bus traffic breaks.

Workflow for a loop change:
1. Write a sweep file (`base(...)`, `add(tag, **overrides)`; `iram=1` to match production;
   `chk=0` drops the checksum = timing-only, reported BAD).
2. `python tools/sha_bench/gen_variants.py my_sweep.py`
3. Build/flash with `SHA_BENCH=2`, capture serial, run `summarize.py`.
4. Only promote a variant that is **zero wrong hashes in every stress mode, every pass**.

A hashrate counter going up proves nothing. Corrupted hashes still get counted. The bench and
accepted share rate are the only evidence.

## Hardware facts learnt on this chip (don't relearn them the hard way)

Cycles are 240 MHz CPU cycles.

- Classic ESP32 SHA engine **cannot load an external midstate**. Every nonce costs 3 hardware
  blocks (block 1 recomputed every nonce, block 2 with nonce, block 3 = second SHA) + 2 `LOAD`s.
- `START`/`CONTINUE` keep the engine busy ~68 cycles (one round/cycle). `LOAD` ~12 cycles.
  → engine floor ≈ 3×68 + 2×12 = **228 cycles/nonce ≈ 1.05 MH/s ceiling**.
- Engine samples `TEXT[i]` about i+1 cycles after the trigger and never reads TEXT again, so TEXT
  can be rewritten from ~cycle 17.
- A compute leaves TEXT untouched; `LOAD` writes only `TEXT[0..7]`, one word/cycle, and **a CPU
  store to TEXT while LOAD runs damages the transfer**.
- Register offsets from base `0x3FF03000`: TEXT `0x00–0x3C`, `START 0x90`, `CONTINUE 0x94`,
  `LOAD 0x98`.

### Rule 1 — store spacing ≥ 3 cycles
Stores to SHA registers must be at least three cycles apart (store + two other instructions).
At two-cycle spacing, a store is **silently dropped** whenever core 0 touches any DPORT register at
that moment: **30–60% wrong hashes** under core-0 DPORT load; none at three.
In the macros this is `wgap >= 1` (`COPY` = `l32i; s32i; NOPS wgap`).

### Rule 2 — never poll BUSY in the hot path
A poll costs ~10 cycles/wait and bought no safety (a fully polled variant still produced wrong
hashes under core-0 APB reads; cause not pinned down). Counted delays are safe as minimums:
interrupts and contention only stall the CPU, never the engine.

### Rule 3 — more store/read facts (bench, 2026-10-09)
- Refill stores **back to back** (one cycle apart, from registers) give wrong hashes on a **quiet**
  chip, every pass. Two cycles apart: clean quiet, wrong under every core-0 DPORT stress mode.
- A read of TEXT **one cycle after a store** comes back wrong under core-0 DPORT traffic; two
  cycles after is fine, and a store one cycle after the read is fine.
- The digest word (TEXT[7]) is readable from **LOAD + 15** instruction cycles under stress (14
  quiet); the first store to TEXT may go at LOAD + 12.
- **START before the last header store** (TEXT[7] three cycles after START) fails in most stress
  modes and some quiet passes, whatever p1 is. CPU stores during the engine's read phase are out.

### Measured delay thresholds (quiet / worst-case core-0 bus traffic)
`p1 3/4, p2 39/40, pl1 9/10, p3 26/27, pl2 9/10, d2 12, d3 8`.
Production fast loop = worst-case + 0: `p1 4, d2 16, p2 40, pl1 10, d3 14, p3 27, pl2 10, lpos 2, wgap 1`.
First fallback (`_margin`, the loop up to V1.2.0) = +2 on the five trigger delays; wide = +14.

## Performance budget

- Current loop: **247 cycles/nonce** (~972 kH/s) vs 228 engine floor → 19 cycles of slack.
  Measured mining 2026-10-09: HW 974.5 kH/s ±1% from 10,685 candidates in 12 min, mismatch 0,
  counter total 987–991 kH/s.
- Each cycle removed ≈ **+3.9 kH/s**. What is left is the idle header refill (8 stores at
  rule-1 spacing after LOAD, ~37 cycles LOAD→START) and it does not shrink within the rules above.

## Optimisation backlog — outcome (2026-10-09, all through SHA_BENCH=2)

1. **Trim delay margins — done.** +1 (252 cycles) and +0 (247) both ran 196 passes, ~23 million
   hashes, all nine stress modes, zero wrong; one cycle lower fails. +0 promoted.
2. **Header words from registers — no gain.** At rule-1 spacing it saves one cycle (256 vs 257 at
   +2 margins) and showed stray bad passes at +1/+0 margins that the copying refill does not.
   Not kept in the macro; patch and logs in `C:\nb\tight_experiment\`.
3. **Sampling the digest earlier — not the bottleneck.** See rule 3: the read needs LOAD + 15,
   later than the first store, so it already sits inside a store gap.
4. **Stores after START — fails.** See rule 3.

Left untried: folding the digest read into a store gap of the copying refill (one cycle).
Core 0 software worker already has baking + round-60 early exit and shares the core with WiFi at
priority 1 — low expected gain there.

## NMMiner comparison (closed source, claims ~1,040 kH/s on CYD)

Repo: https://github.com/NMminer1024/NMMiner — binaries only, in
`firmware/esp32-2432s028r-ili9341/`. Analysed **v2.0.04** `firmware.bin`:

- Plain unencrypted ESP-IDF 4.4.6 / Arduino app image. Segments:
  DROM `0x3F400020`, DRAM `0x3FFBDB60`, IRAM `0x40080000` and `0x40084E20`, IROM `0x400D0020`.
- Default pool is their own (`solobtc.nmminer.com`), so "pool stats confirm it" largely means
  their own dashboard.
- SHA register literals at `0x400D17FC–0x400D1844`. Miner function entry `0x400F22E8`.
  **Hot loop `0x400F24BB–0x400F2687`, executed from flash cache (IROM), not IRAM.**
  Fast path only taken when a local flag byte `== 3`; otherwise a slower variant with `memw`
  barriers and a safe DPORT read helper (gate not identified — maybe chip revision).
- Same structure as ours (3 blocks, 2 LOADs). Their per-nonce schedule:

```
count++; nonce += 2               ; two-core stride
TEXT[0..15] = block 2             ; l32i/s32i pairs -> 1 instr between stores (2-cycle spacing)
13 nop; CONTINUE
59 nop; TEXT[8]=0x80000000; TEXT[15]=0x100
1 nop; LOAD
9 nop; START (block 3)
46 nop; TEXT[8..15] = hdr         ; pairs
LOAD                              ; immediately after last TEXT store
15 nop; l16ui TEXT[7]             ; exit if low 16 bits == 0
TEXT[0..5] = a9..a14              ; 6 back-to-back stores, header preloaded in registers
TEXT[6..7] = hdr; START           ; START immediately after last store
8 nop; j top
```

- ~227 instruction slots/nonce ≈ 1.06 MH/s before stalls — consistent with their claim. Counter
  increments once per iteration, so it isn't inflated.
- **Versus our rules:** 2-cycle store spacing everywhere (≈ our `wgap=0`), zero-gap header stores,
  triggers issued right after a TEXT store, and block-3 START→LOAD ≈ 62 slots vs our ~64–65
  threshold (static slot count, not measured cycles — a lead, not a verdict).
- Unknown: whether HW candidates are re-verified in software (their caller not traced).

### Open experiments
1. **Tighter store spacing on our board — done 2026-10-09, it does not survive.** Our own bench
   variants with two-cycle and back-to-back refill stores fail (rule 3 above); flash vs IRAM made
   no difference to the production loop. A ~230-cycle schedule is not reachable on this chip
   without wrong hashes.
2. **Black-box share test:** run stock NMMiner against a local stratum server on an isolated LAN,
   low share difficulty (e.g. 1/65536 ⇒ ~2^16 hashes/share; real 1,040 kH/s ≈ 15.9 shares/s,
   real 950 ≈ 14.5/s). Compare accepted shares/s with displayed hashrate. Log DNS/connections.
3. Given 1, the black-box share test is what would show whether their *useful* hashrate matches
   the display.

### Licensing
Reverse engineering for understanding is fine for our purposes. **Do not copy NMMiner code or
disassembly into this repo.** Reimplement ideas independently and validate them with our bench.

## Conventions

- Any hot-loop change: bench first, numbers in the commit message (variant, stress modes, wrong
  hashes = 0, cycles/nonce, sustained kH/s).
- Keep the wide fallback and the power-on self-test working.
- README "Why this hashrate is real" section must stay true — update it if the numbers change.
- Credit upstream NerdMiner_v2 for Stratum and multi-board support.
