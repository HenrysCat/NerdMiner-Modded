// Host implementations behind test/native_shim (pio test -e native).
#include "Arduino.h"
#include "esp_timer.h"
#include "mbedtls/sha256.h"
#include <chrono>

HostSerial Serial;

unsigned long millis()
{
  using namespace std::chrono;
  static const auto t0 = steady_clock::now();
  return (unsigned long)duration_cast<milliseconds>(steady_clock::now() - t0).count();
}

int64_t esp_timer_get_time()
{
  return (int64_t)millis() * 1000;
}

// --- SHA-256 (FIPS 180-4), plain and small: a reference, not fast ---------
static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

static inline uint32_t ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static void sha256_block(uint32_t st[8], const uint8_t *p)
{
  uint32_t w[64];
  for (int i = 0; i < 16; i++)
    w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
  for (int i = 16; i < 64; i++)
  {
    const uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = st[0], b = st[1], c = st[2], d = st[3], e = st[4], f = st[5], g = st[6], h = st[7];
  for (int i = 0; i < 64; i++)
  {
    const uint32_t t1 = h + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
    const uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
    h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
  }
  st[0] += a; st[1] += b; st[2] += c; st[3] += d; st[4] += e; st[5] += f; st[6] += g; st[7] += h;
}

void mbedtls_sha256_init(mbedtls_sha256_context *ctx) { memset(ctx, 0, sizeof(*ctx)); }
void mbedtls_sha256_free(mbedtls_sha256_context *ctx) { memset(ctx, 0, sizeof(*ctx)); }

int mbedtls_sha256_starts_ret(mbedtls_sha256_context *ctx, int)
{
  static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  memcpy(ctx->state, iv, sizeof(iv));
  ctx->total = 0;
  ctx->used = 0;
  return 0;
}

int mbedtls_sha256_update_ret(mbedtls_sha256_context *ctx, const unsigned char *in, size_t len)
{
  ctx->total += len;
  while (len)
  {
    const size_t n = len < 64 - ctx->used ? len : 64 - ctx->used;
    memcpy(ctx->buf + ctx->used, in, n);
    ctx->used += n; in += n; len -= n;
    if (ctx->used == 64) { sha256_block(ctx->state, ctx->buf); ctx->used = 0; }
  }
  return 0;
}

int mbedtls_sha256_finish_ret(mbedtls_sha256_context *ctx, unsigned char out[32])
{
  const uint64_t bits = ctx->total * 8;
  uint8_t pad = 0x80;
  mbedtls_sha256_update_ret(ctx, &pad, 1);
  pad = 0;
  while (ctx->used != 56) mbedtls_sha256_update_ret(ctx, &pad, 1);
  uint8_t len[8];
  for (int i = 0; i < 8; i++) len[i] = (uint8_t)(bits >> (56 - 8 * i));
  mbedtls_sha256_update_ret(ctx, len, 8);
  for (int i = 0; i < 8; i++)
  {
    out[4 * i] = ctx->state[i] >> 24; out[4 * i + 1] = ctx->state[i] >> 16;
    out[4 * i + 2] = ctx->state[i] >> 8; out[4 * i + 3] = ctx->state[i];
  }
  return 0;
}
