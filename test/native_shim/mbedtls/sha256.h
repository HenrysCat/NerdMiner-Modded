// Host SHA-256 with the mbedTLS 2.x API used by src/utils.cpp
// (implementation in test/native_shim/host_shim.cpp).
#pragma once
#include <stdint.h>
#include <stddef.h>
typedef struct
{
  uint32_t state[8];
  uint64_t total;
  uint8_t buf[64];
  size_t used;
} mbedtls_sha256_context;
void mbedtls_sha256_init(mbedtls_sha256_context *ctx);
void mbedtls_sha256_free(mbedtls_sha256_context *ctx);
int mbedtls_sha256_starts_ret(mbedtls_sha256_context *ctx, int is224);
int mbedtls_sha256_update_ret(mbedtls_sha256_context *ctx, const unsigned char *in, size_t len);
int mbedtls_sha256_finish_ret(mbedtls_sha256_context *ctx, unsigned char out[32]);
