// Host unit tests: pio test -e native
// Stratum parsing and requests, the header built from a job, the share checks
// and the software SHA-256d, against vectors computed independently in Python
// (gen_vectors.py, hashlib).
#include <unity.h>
#include <ArduinoJson.h>
#include "stratum.h"
#include "utils.h"
#include "ShaTests/nerdSHA256plus.h"
#include "mbedtls/sha256.h"
#include "vectors.h"

void setUp() {}
void tearDown() {}

static void hostSha256d(const uint8_t *in, size_t len, uint8_t out[32])
{
  mbedtls_sha256_context c;
  uint8_t mid[32];
  mbedtls_sha256_init(&c);
  mbedtls_sha256_starts_ret(&c, 0);
  mbedtls_sha256_update_ret(&c, in, len);
  mbedtls_sha256_finish_ret(&c, mid);
  mbedtls_sha256_starts_ret(&c, 0);
  mbedtls_sha256_update_ret(&c, mid, 32);
  mbedtls_sha256_finish_ret(&c, out);
}

// ---------------------------------------------------------------- stratum --

static void test_subscribe_reply()
{
  mining_subscribe s = init_mining_subscribe();
  TEST_ASSERT_TRUE(parse_mining_subscribe(
      "{\"id\":1,\"result\":[[[\"mining.notify\",\"ae6812eb4cd7735a302a8a9dd95cf71f\"]],\"f8002c90\",4],\"error\":null}", s));
  TEST_ASSERT_EQUAL_STRING("f8002c90", s.extranonce1.c_str());
  TEST_ASSERT_EQUAL_INT(4, s.extranonce2_size);
}

static void test_method_and_replies()
{
  TEST_ASSERT_EQUAL(MINING_NOTIFY, parse_mining_method(empty_notify));
  TEST_ASSERT_EQUAL(MINING_SET_DIFFICULTY, parse_mining_method("{\"id\":null,\"method\":\"mining.set_difficulty\",\"params\":[0.5]}"));
  TEST_ASSERT_EQUAL(STRATUM_SUCCESS, parse_mining_method("{\"id\":5,\"result\":true,\"error\":null}"));
  // "error": false is not an error (some pools send it)
  TEST_ASSERT_EQUAL(STRATUM_SUCCESS, parse_mining_method("{\"id\":5,\"result\":true,\"error\":false}"));
  // both error forms, and an error without a message (used to crash, #4)
  TEST_ASSERT_EQUAL(STRATUM_PARSE_ERROR, parse_mining_method("{\"id\":6,\"result\":null,\"error\":[23,\"Low difficulty share\",null]}"));
  TEST_ASSERT_EQUAL(STRATUM_PARSE_ERROR, parse_mining_method("{\"id\":6,\"result\":null,\"error\":{\"code\":21,\"message\":\"Stale\"}}"));
  TEST_ASSERT_EQUAL(STRATUM_PARSE_ERROR, parse_mining_method("{\"id\":6,\"result\":null,\"error\":[21]}"));
  TEST_ASSERT_EQUAL(STRATUM_PARSE_ERROR, parse_mining_method("not json"));
  TEST_ASSERT_EQUAL(7, parse_extract_id("{\"id\":7,\"result\":false,\"error\":null}"));
  TEST_ASSERT_FALSE(parse_extract_result("{\"id\":7,\"result\":false,\"error\":null}"));
  TEST_ASSERT_TRUE(parse_extract_result("{\"id\":7,\"result\":true,\"error\":null}"));
}

static void test_notify_fields()
{
  mining_job j;
  TEST_ASSERT_TRUE(parse_mining_notify(empty_notify, j));
  TEST_ASSERT_EQUAL_STRING("4f", j.job_id.c_str());
  TEST_ASSERT_EQUAL_STRING("17034219", j.nbits.c_str());
  TEST_ASSERT_EQUAL_UINT(0, j.merkle_branch.size());
  TEST_ASSERT_TRUE(j.clean_jobs);
}

// A notify built from empty_notify with one field replaced.
static String withParam(int index, const char *json)
{
  StaticJsonDocument<4096> d;
  deserializeJson(d, empty_notify);
  d["params"][index] = serialized(json);
  String out;
  serializeJson(d, out);
  return out;
}

static void test_notify_refuses_malformed_jobs()
{
  mining_job j;
  TEST_ASSERT_FALSE(parse_mining_notify(withParam(1, "\"00ff\""), j));       // prevhash too short
  TEST_ASSERT_FALSE(parse_mining_notify(withParam(5, "\"2000000\""), j));    // version: 7 digits
  TEST_ASSERT_FALSE(parse_mining_notify(withParam(6, "\"ff034219\""), j));   // nbits exponent out of range
  TEST_ASSERT_FALSE(parse_mining_notify(withParam(6, "\"1703421\""), j));    // nbits: 7 digits
  TEST_ASSERT_FALSE(parse_mining_notify(withParam(7, "\"6717a9bz\""), j));   // ntime not hex
  TEST_ASSERT_FALSE(parse_mining_notify(withParam(2, "\"abc\""), j));        // coinb1 odd length
  TEST_ASSERT_FALSE(parse_mining_notify(withParam(4, "[\"00\"]"), j));       // short merkle branch
  TEST_ASSERT_FALSE(parse_mining_notify(withParam(0, "\"\""), j));           // empty job id
  String many = "[";
  for (int i = 0; i < 33; i++)
    many += String(i ? ",\"" : "\"") + "1111111111111111111111111111111111111111111111111111111111111111\"";
  many += "]";
  TEST_ASSERT_FALSE(parse_mining_notify(withParam(4, many.c_str()), j));     // more than 32 branches
}

static void test_set_difficulty()
{
  double d = 1.0;
  TEST_ASSERT_TRUE(parse_mining_set_difficulty("{\"id\":null,\"method\":\"mining.set_difficulty\",\"params\":[0.0015]}", d));
  TEST_ASSERT_EQUAL_DOUBLE(0.0015, d);
  TEST_ASSERT_TRUE(parse_mining_set_difficulty("{\"id\":null,\"method\":\"mining.set_difficulty\",\"params\":[2]}", d));
  TEST_ASSERT_EQUAL_DOUBLE(2.0, d);
  // 0, negative or not a number: ignored, every hash would be a share
  TEST_ASSERT_FALSE(parse_mining_set_difficulty("{\"id\":null,\"method\":\"mining.set_difficulty\",\"params\":[0]}", d));
  TEST_ASSERT_FALSE(parse_mining_set_difficulty("{\"id\":null,\"method\":\"mining.set_difficulty\",\"params\":[-1]}", d));
  TEST_ASSERT_FALSE(parse_mining_set_difficulty("{\"id\":null,\"method\":\"mining.set_difficulty\",\"params\":[\"x\"]}", d));
  TEST_ASSERT_EQUAL_DOUBLE(2.0, d);
}

static void test_authorize_escapes_and_returns_id()
{
  WiFiClient c;
  unsigned long authId = 0;
  const char *user = "bc1qexample.\"worker\\1";
  const char *pass = "x,d=0.001,mn=a-very-long-option-string";
  tx_mining_auth(c, user, pass, authId);
  TEST_ASSERT_NOT_EQUAL(0, authId);
  StaticJsonDocument<1024> d;
  TEST_ASSERT_EQUAL(DeserializationError::Ok, deserializeJson(d, c.sent.c_str()).code());
  TEST_ASSERT_EQUAL_STRING("mining.authorize", d["method"]);
  TEST_ASSERT_EQUAL_STRING(user, d["params"][0]);
  TEST_ASSERT_EQUAL_STRING(pass, d["params"][1]);
  TEST_ASSERT_EQUAL((long)authId, d["id"].as<long>());
}

static void test_submit_nonce_has_8_digits()
{
  WiFiClient c;
  mining_subscribe w = init_mining_subscribe();
  strlcpy(w.wName, "bc1qexample", sizeof(w.wName));
  w.extranonce2 = "00000001";
  mining_job j;
  TEST_ASSERT_TRUE(parse_mining_notify(empty_notify, j));
  unsigned long id = 0;
  tx_mining_submit(c, w, j, 0x00000abcUL, id);
  StaticJsonDocument<1024> d;
  TEST_ASSERT_EQUAL(DeserializationError::Ok, deserializeJson(d, c.sent.c_str()).code());
  TEST_ASSERT_EQUAL_STRING("00000abc", d["params"][4]); // "abc" was refused by pools
  TEST_ASSERT_EQUAL_STRING("4f", d["params"][1]);
  TEST_ASSERT_EQUAL_STRING("6717a9b1", d["params"][3]);
}

static void test_password_fits()
{
  // the password from the settings (79 characters + 0) must fit
  mining_subscribe w;
  TEST_ASSERT_TRUE(sizeof(w.wPass) >= 80);
  TEST_ASSERT_TRUE(sizeof(w.wName) >= 80);
}

// ------------------------------------------------------------------ utils --

static void test_to_byte_array()
{
  uint8_t out[4];
  TEST_ASSERT_EQUAL_INT(4, to_byte_array("00ff10Ab", 8, out));
  const uint8_t want[4] = {0x00, 0xff, 0x10, 0xab};
  TEST_ASSERT_EQUAL_HEX8_ARRAY(want, out, 4);
  char inPlace[] = "deadbeef";
  TEST_ASSERT_EQUAL_INT(4, to_byte_array(inPlace, 8, (uint8_t *)inPlace)); // decoded in place
  const uint8_t want2[4] = {0xde, 0xad, 0xbe, 0xef};
  TEST_ASSERT_EQUAL_HEX8_ARRAY(want2, (uint8_t *)inPlace, 4);
}

static void checkHeader(const char *notify, const char *en2, const uint8_t *want)
{
  mining_subscribe w = init_mining_subscribe();
  w.extranonce1 = "f8002c90";
  w.extranonce2_size = 4;
  mining_job j;
  TEST_ASSERT_TRUE(parse_mining_notify(notify, j));
  miner_data m = calculateMiningData(w, j);
  TEST_ASSERT_EQUAL_STRING(en2, w.extranonce2.c_str());
  TEST_ASSERT_EQUAL_HEX8_ARRAY(want, m.bytearray_blockheader, 80);
}

// Run in this order: extranonce2 counts up from 1, one per job.
static void test_header_empty_block() { checkHeader(empty_notify, empty_en2, empty_header); }
static void test_header_big_coinbase_and_branches() { checkHeader(big_notify, big_en2, big_header); }

static void test_check_valid()
{
  // a real block meets its own target (and the 0x18 one, 2^16 times harder: its
  // hash starts with 8 zero bytes)...
  TEST_ASSERT_TRUE(checkValid(block125552_hash, block125552_nbits));
  TEST_ASSERT_TRUE(checkValid(block125552_hash, "1844b9f2"));
  // ...but not a target with 9 leading zero bytes, and a hash with its top
  // byte set never does
  TEST_ASSERT_FALSE(checkValid(block125552_hash, "1744b9f2"));
  uint8_t h[32];
  memcpy(h, block125552_hash, 32);
  h[31] = 0x01;
  TEST_ASSERT_FALSE(checkValid(h, block125552_nbits));
  TEST_ASSERT_FALSE(checkValid(block125552_hash, "00000000"));
  TEST_ASSERT_FALSE(checkValid(nullptr, block125552_nbits));
}

static void test_difficulty_of_a_hash()
{
  // difficulty-1 target: 0x00000000FFFF0000...0
  uint8_t t[32] = {0};
  t[26] = 0xff;
  t[27] = 0xff;
  TEST_ASSERT_DOUBLE_WITHIN(1e-9, 1.0, diff_from_target(t));
  // block 125552: difficulty about 244112 for its target, and its hash is
  // well above that
  TEST_ASSERT_TRUE(diff_from_target((void *)block125552_hash) > 244112.0);
}

// -------------------------------------------------------------- sha-256d --

static void test_sha256d_baked_on_a_real_block()
{
  uint32_t mid[8], bake[16];
  uint8_t hash[32];
  uint8_t hdr[80];
  memcpy(hdr, block125552_header, 80);
  nerd_mids(mid, hdr);
  nerd_sha256_bake(mid, hdr + 64, bake);
  TEST_ASSERT_TRUE(nerd_sha256d_baked(mid, hdr + 64, bake, hash));
  TEST_ASSERT_EQUAL_HEX8_ARRAY(block125552_hash, hash, 32);
  // next nonce: the 16-bit early exit says no
  hdr[76]++;
  TEST_ASSERT_FALSE(nerd_sha256d_baked(mid, hdr + 64, bake, hash));
}

static void test_sha256d_baked_matches_reference()
{
  // every nonce whose double hash ends with 16 zero bits must pass, with the
  // right hash; the others must not
  uint8_t hdr[80];
  memcpy(hdr, big_header, 80);
  uint32_t mid[8], bake[16];
  nerd_mids(mid, hdr);
  nerd_sha256_bake(mid, hdr + 64, bake);
  int passes = 0;
  for (uint32_t n = 0; n < 300000 && passes < 2; n++)
  {
    memcpy(hdr + 76, &n, 4);
    uint8_t got[32], ref[32];
    const bool p = nerd_sha256d_baked(mid, hdr + 64, bake, got);
    hostSha256d(hdr, 80, ref);
    const bool want = ref[31] == 0 && ref[30] == 0;
    TEST_ASSERT_EQUAL(want, p);
    if (p)
    {
      TEST_ASSERT_EQUAL_HEX8_ARRAY(ref, got, 32);
      passes++;
    }
  }
  TEST_ASSERT_TRUE(passes > 0);
}

int main(int, char **)
{
  UNITY_BEGIN();
  RUN_TEST(test_subscribe_reply);
  RUN_TEST(test_method_and_replies);
  RUN_TEST(test_notify_fields);
  RUN_TEST(test_notify_refuses_malformed_jobs);
  RUN_TEST(test_set_difficulty);
  RUN_TEST(test_authorize_escapes_and_returns_id);
  RUN_TEST(test_submit_nonce_has_8_digits);
  RUN_TEST(test_password_fits);
  RUN_TEST(test_to_byte_array);
  RUN_TEST(test_header_empty_block);
  RUN_TEST(test_header_big_coinbase_and_branches);
  RUN_TEST(test_check_valid);
  RUN_TEST(test_difficulty_of_a_hash);
  RUN_TEST(test_sha256d_baked_on_a_real_block);
  RUN_TEST(test_sha256d_baked_matches_reference);
  return UNITY_END();
}
