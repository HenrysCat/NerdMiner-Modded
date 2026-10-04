// On-device bench for the ESP32-S3 SHA engine. Built only with
// -D SHA_BENCH_S3=1; it runs instead of the miner (see setup()) and prints to
// the serial port forever. Every result is checked against software.
//
//   T  what the CPU-side register traffic costs
//   A  how long START / CONTINUE keep the engine busy, and when the digest
//      can be read
//   B  whether a compute leaves TEXT untouched
//   C  when the engine samples each TEXT word (i.e. from when it may be
//      overwritten with the next block)
//   L  cycles per nonce of the mining loops, every hash checked
//
// The S3 mining loop in mining.cpp is derived from these numbers.
#include <Arduino.h> // for the target macros

#if defined(SHA_BENCH_S3) && defined(CONFIG_IDF_TARGET_ESP32S3)

#include <soc/hwcrypto_reg.h>
#include <driver/periph_ctrl.h>

#define TEXT ((volatile uint32_t *)SHA_TEXT_BASE)
#define HREG ((volatile uint32_t *)SHA_H_BASE)
#define R_MODE ((volatile uint32_t *)SHA_MODE_REG)
#define R_START ((volatile uint32_t *)SHA_START_REG)
#define R_CONT ((volatile uint32_t *)SHA_CONTINUE_REG)
#define R_BUSY ((volatile uint32_t *)SHA_BUSY_REG)

static inline uint32_t cc()
{
  uint32_t c;
  __asm__ __volatile__("rsr %0, ccount" : "=r"(c));
  return c;
}

// ---------------------------------------------------- software reference --

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sw_compress(uint32_t st[8], const uint8_t *p)
{
  uint32_t w[64];
  for (int i = 0; i < 16; ++i)
    w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) | ((uint32_t)p[4 * i + 2] << 8) | p[4 * i + 3];
  for (int i = 16; i < 64; ++i)
  {
    uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
    uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = st[0], b = st[1], c = st[2], d = st[3], e = st[4], f = st[5], g = st[6], h = st[7];
  for (int i = 0; i < 64; ++i)
  {
    uint32_t t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
    uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
    h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
  }
  st[0] += a; st[1] += b; st[2] += c; st[3] += d; st[4] += e; st[5] += f; st[6] += g; st[7] += h;
}

static void sw_sha256(const uint8_t *msg, size_t len, uint8_t out[32])
{
  uint32_t st[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  uint8_t buf[128];
  size_t full = len / 64;
  for (size_t i = 0; i < full; ++i)
    sw_compress(st, msg + 64 * i);
  size_t rem = len - 64 * full;
  memset(buf, 0, sizeof(buf));
  memcpy(buf, msg + 64 * full, rem);
  buf[rem] = 0x80;
  size_t tot = rem + 9 > 64 ? 128 : 64;
  uint64_t bits = (uint64_t)len * 8;
  for (int i = 0; i < 8; ++i)
    buf[tot - 1 - i] = (uint8_t)(bits >> (8 * i));
  sw_compress(st, buf);
  if (tot == 128)
    sw_compress(st, buf + 64);
  for (int i = 0; i < 8; ++i)
  {
    out[4 * i] = st[i] >> 24;
    out[4 * i + 1] = st[i] >> 16;
    out[4 * i + 2] = st[i] >> 8;
    out[4 * i + 3] = st[i];
  }
}

// ------------------------------------------------------------- test data --

static uint8_t hdr[80];
static uint32_t mid[8];   // engine state after the header's first 64 bytes
static uint32_t tailw[3]; // header words 16..18 as the engine wants them

// What the engine's H registers hold after each block, for one nonce.
static void expect(uint32_t nonce, uint32_t hA[8], uint32_t hB[8])
{
  uint8_t h[80], d1[32], d2[32];
  memcpy(h, hdr, 80);
  memcpy(h + 76, &nonce, 4);
  sw_sha256(h, 80, d1);
  sw_sha256(d1, 32, d2);
  memcpy(hA, d1, 32);
  memcpy(hB, d2, 32);
}

static inline void wait_idle()
{
  while (*R_BUSY)
  {
  }
}

static inline void fillA(uint32_t nonce)
{
  TEXT[0] = tailw[0];
  TEXT[1] = tailw[1];
  TEXT[2] = tailw[2];
  TEXT[3] = nonce;
  TEXT[4] = 0x00000080;
  for (int i = 5; i < 15; ++i)
    TEXT[i] = 0;
  TEXT[15] = 0x80020000;
}

static inline void loadMid()
{
  for (int i = 0; i < 8; ++i)
    HREG[i] = mid[i];
}

static inline void fillB()
{
  for (int i = 0; i < 8; ++i)
    TEXT[i] = HREG[i];
  TEXT[8] = 0x00000080;
  for (int i = 9; i < 15; ++i)
    TEXT[i] = 0;
  TEXT[15] = 0x00010000;
}

static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

static void setupData()
{
  for (int i = 0; i < 80; ++i)
    hdr[i] = (uint8_t)(0x9E * (i + 1) + (i >> 2));
  *R_MODE = 2; // SHA2_256
  const uint32_t *w = (const uint32_t *)hdr;
  for (int i = 0; i < 16; ++i)
    TEXT[i] = w[i];
  *R_START = 1;
  wait_idle();
  for (int i = 0; i < 8; ++i)
    mid[i] = HREG[i];
  tailw[0] = w[16];
  tailw[1] = w[17];
  tailw[2] = w[18];
}

// ---------------------------------------------------------------- tests --

static void testT()
{
  uint32_t t, tText = 0, tHw = 0, tHr = 0, tCopy = 0, tBusy = 0, tNop = 0;
  volatile uint32_t sink = 0;
  const int REPS = 32;
  portENTER_CRITICAL(&mux);
  for (int r = 0; r < REPS; ++r)
  {
    t = cc();
    for (int i = 0; i < 16; ++i)
      TEXT[i] = i;
    tText += cc() - t;
    t = cc();
    for (int i = 0; i < 8; ++i)
      HREG[i] = mid[i];
    tHw += cc() - t;
    t = cc();
    uint32_t s = 0;
    for (int i = 0; i < 8; ++i)
      s += HREG[i];
    tHr += cc() - t;
    sink = s;
    t = cc();
    for (int i = 0; i < 8; ++i)
      TEXT[i] = HREG[i];
    tCopy += cc() - t;
    t = cc();
    s = 0;
    for (int i = 0; i < 8; ++i)
      s += *R_BUSY;
    tBusy += cc() - t;
    sink = s;
    t = cc();
    tNop += cc() - t;
  }
  portEXIT_CRITICAL(&mux);
  Serial.printf("T cycles: TEXT store %.1f  H store %.1f  H load %.1f  H->TEXT copy %.1f  BUSY read %.1f  (empty %.1f)\n",
                tText / (16.0 * REPS), tHw / (8.0 * REPS), tHr / (8.0 * REPS), tCopy / (8.0 * REPS),
                tBusy / (8.0 * REPS), tNop / (1.0 * REPS));
}

static void testA()
{
  uint32_t hA[8], hB[8];
  const uint32_t nonce = 0x12345678;
  expect(nonce, hA, hB);

  for (int which = 0; which < 2; ++which) // 0 = CONTINUE (block A), 1 = START (block B)
  {
    uint32_t busyMin = 0xFFFFFFFF, busyMax = 0;
    int polls = 0;
    int lastBad = -1, firstGood = 100000;
    bool endOk = true;
    for (int d = 0; d < 700; d += 2)
    {
      portENTER_CRITICAL(&mux);
      loadMid();
      fillA(nonce);
      if (which == 1)
      {
        *R_CONT = 1;
        wait_idle();
        fillB();
      }
      uint32_t t0 = cc();
      if (which == 0)
        *R_CONT = 1;
      else
        *R_START = 1;
      while (cc() - t0 < (uint32_t)d)
      {
      }
      uint32_t ta = cc() - t0;
      uint32_t v = HREG[7];
      wait_idle();
      uint32_t fin = HREG[7];
      portEXIT_CRITICAL(&mux);
      const uint32_t want = which == 0 ? hA[7] : hB[7];
      if (fin != want)
        endOk = false;
      if (v == want)
      {
        if ((int)ta < firstGood)
          firstGood = ta;
      }
      else if ((int)ta > lastBad)
        lastBad = ta;
    }
    for (int r = 0; r < 8; ++r)
    {
      portENTER_CRITICAL(&mux);
      loadMid();
      fillA(nonce);
      if (which == 1)
      {
        *R_CONT = 1;
        wait_idle();
        fillB();
      }
      uint32_t t0 = cc();
      if (which == 0)
        *R_CONT = 1;
      else
        *R_START = 1;
      int n = 0;
      while (*R_BUSY)
        n++;
      uint32_t dt = cc() - t0;
      portEXIT_CRITICAL(&mux);
      polls = n;
      if (dt < busyMin)
        busyMin = dt;
      if (dt > busyMax)
        busyMax = dt;
    }
    Serial.printf("A %s: busy-poll exit %u..%u cycles (%d polls); digest wrong when read at <=%d, right from %d; final %s\n",
                  which == 0 ? "CONTINUE" : "START   ", busyMin, busyMax, polls, lastBad, firstGood, endOk ? "ok" : "BAD");
  }
}

static void testB()
{
  uint32_t before[16], after[16];
  const uint32_t nonce = 0x0BADF00D;
  loadMid();
  fillA(nonce);
  for (int i = 0; i < 16; ++i)
    before[i] = TEXT[i];
  *R_CONT = 1;
  wait_idle();
  for (int i = 0; i < 16; ++i)
    after[i] = TEXT[i];
  int same = 0;
  for (int i = 0; i < 16; ++i)
    same += before[i] == after[i];
  Serial.printf("B TEXT after a compute: %d of 16 words unchanged (readback before: %08x %08x .. %08x)\n", same,
                before[0], before[3], before[15]);
  if (same != 16)
  {
    Serial.print("B   after:");
    for (int i = 0; i < 16; ++i)
      Serial.printf(" %08x", after[i]);
    Serial.println();
  }
}

static void testC()
{
  uint32_t hA[8], hB[8];
  const uint32_t nonce = 0x55AA1234;
  expect(nonce, hA, hB);
  Serial.print("C TEXT[i] may be overwritten from (cycles after CONTINUE; -1 = any time):");
  for (int i = 0; i < 16; ++i)
  {
    int lastBad = -1;
    for (int d = 0; d < 500; d += 2)
    {
      portENTER_CRITICAL(&mux);
      loadMid();
      fillA(nonce);
      uint32_t t0 = cc();
      *R_CONT = 1;
      while (cc() - t0 < (uint32_t)d)
      {
      }
      uint32_t ta = cc() - t0;
      TEXT[i] = 0xDEADBEEF ^ i;
      wait_idle();
      bool ok = true;
      for (int k = 0; k < 8; ++k)
        ok &= HREG[k] == hA[k];
      portEXIT_CRITICAL(&mux);
      if (!ok && (int)ta > lastBad)
        lastBad = ta;
    }
    Serial.printf(" %d", lastBad < 0 ? -1 : lastBad + 1);
  }
  Serial.println();
}

// The loop mining.cpp runs today.
static uint32_t __attribute__((noinline)) loopStock(uint32_t nonce, uint32_t count, uint32_t *sum)
{
  uint32_t s = 0;
  for (uint32_t n = nonce; n != nonce + count; ++n)
  {
    loadMid();
    fillA(n);
    *R_CONT = 1;
    wait_idle();
    fillB();
    *R_START = 1;
    wait_idle();
    s += HREG[7];
  }
  *sum = s;
  return count;
}

// Overlapped loop: the engine latches TEXT at the trigger and leaves it alone,
// so the next block's words are written while the current one computes, and
// only the words that differ between the two blocks are written at all.
//   poll = 1: wait for BUSY to clear before reading the digest
//   poll = 0: rely on the register traffic taking longer than the compute
static uint32_t __attribute__((noinline, optimize("O2"))) loopFast(uint32_t nonce, uint32_t count, uint32_t *sum,
                                                                    int poll)
{
  uint32_t s = 0;
  const uint32_t m0 = mid[0], m1 = mid[1], m2 = mid[2], m3 = mid[3], m4 = mid[4], m5 = mid[5], m6 = mid[6],
                 m7 = mid[7];
  const uint32_t t0 = tailw[0], t1 = tailw[1], t2 = tailw[2];
  fillA(nonce);
  for (uint32_t n = nonce; n != nonce + count; ++n)
  {
    HREG[0] = m0;
    HREG[1] = m1;
    HREG[2] = m2;
    HREG[3] = m3;
    HREG[4] = m4;
    HREG[5] = m5;
    HREG[6] = m6;
    HREG[7] = m7;
    *R_CONT = 1; // first hash, second block
    TEXT[8] = 0x00000080;
    TEXT[15] = 0x00010000;
    while (*R_BUSY)
    {
    }
    TEXT[0] = HREG[0];
    TEXT[1] = HREG[1];
    TEXT[2] = HREG[2];
    TEXT[3] = HREG[3];
    TEXT[4] = HREG[4];
    TEXT[5] = HREG[5];
    TEXT[6] = HREG[6];
    TEXT[7] = HREG[7];
    *R_START = 1; // second hash
    TEXT[0] = t0;
    TEXT[1] = t1;
    TEXT[2] = t2;
    TEXT[3] = n + 1;
    TEXT[4] = 0x00000080;
    TEXT[5] = 0;
    TEXT[6] = 0;
    TEXT[7] = 0;
    TEXT[8] = 0;
    TEXT[15] = 0x80020000;
    if (poll)
      while (*R_BUSY)
      {
      }
    s += HREG[7];
  }
  wait_idle();
  *sum = s;
  return count;
}

static volatile bool stressRun = false;
static void stressTask(void *)
{
  // Bus traffic and interrupts from the other core while the loop runs
  volatile uint32_t sink = 0;
  for (;;)
  {
    if (stressRun)
    {
      for (int i = 0; i < 2000; ++i)
        sink += *R_BUSY + (uint32_t)micros();
      Serial.print("");
    }
    vTaskDelay(1);
  }
}

static void testL()
{
  const uint32_t N = 4000, first = 0x1000;
  uint32_t want = 0;
  for (uint32_t n = first; n != first + N; ++n)
  {
    uint32_t hA[8], hB[8];
    expect(n, hA, hB);
    want += hB[7];
  }
  for (int v = 0; v < 3; ++v)
    for (int pass = 0; pass < 3; ++pass) // 0 = interrupts off, 1 = on, 2 = on with traffic from the other core
    {
      uint32_t sum = 0;
      stressRun = pass == 2;
      if (pass == 0)
        portENTER_CRITICAL(&mux);
      uint32_t t0 = cc();
      if (v == 0)
        loopStock(first, N, &sum);
      else
        loopFast(first, N, &sum, v == 1);
      uint32_t dt = cc() - t0;
      if (pass == 0)
        portEXIT_CRITICAL(&mux);
      stressRun = false;
      Serial.printf("L %-14s pass %d: %.1f cycles per nonce (%.0f KH/s at 240 MHz), %s\n",
                    v == 0 ? "stock" : (v == 1 ? "fast, polled" : "fast, no poll"), pass, dt / (double)N,
                    240000.0 * N / dt, sum == want ? "all hashes ok" : "HASHES WRONG");
    }
}

void sha_bench_s3_run()
{
  delay(2500);
  periph_module_enable(PERIPH_SHA_MODULE);
  xTaskCreatePinnedToCore(stressTask, "stress", 4096, NULL, 2, NULL, xPortGetCoreID() ^ 1);
  for (;;)
  {
    Serial.printf("\n=== ESP32-S3 SHA bench, CPU %u MHz, APB %u Hz ===\n", getCpuFrequencyMhz(), getApbFrequency());
    setupData();
    testT();
    testA();
    testB();
    testC();
    testL();
    Serial.println("=== end ===");
    delay(4000);
  }
}

#endif
