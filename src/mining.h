
#ifndef MINING_API_H
#define MINING_API_H

#include <Arduino.h>
#include <mutex>

// Serializes classic-ESP32 (CONFIG_IDF_TARGET_ESP32) hardware SHA register
// access (SHA_TEXT_BASE) against mbedTLS's own hardware-accelerated TLS
// handshake hashing. Confirmed via a decoded crash backtrace: the classic
// ESP32 SHA engine's esp_sha_lock_engine() locks *per algorithm type* (see
// sha_parallel_engine.h), so our mining code locking SHA2_256 does NOT block
// mbedTLS locking SHA2_384/512 for a TLS "Finished" message hash, even
// though both share the same physical SHA_TEXT_BASE registers -- causing
// either silently wrong hashes or a hard abort() inside sha_hal_read_digest()
// depending on timing. Any code that touches SHA_TEXT_BASE directly (the
// plain-C and pipelined-asm classic-ESP32 HW mining paths) must hold this
// while doing so; any code that can trigger a TLS handshake with hardware
// SHA active (e.g. the pool-stats HTTPS fetch) must hold it too.
extern std::mutex g_hwShaMutex;

// Bumped by the stratum task when the pool accepts a 32-bit share (difficulty
// >= 1, the "32BITS SHARES" counter -- not every pool share, those come in
// more than once a second) / when a valid block is found. Displays watch them to flash a LED or beep (M5StickC Plus2).
extern volatile uint32_t g_shareFoundEvents;
extern volatile uint32_t g_blockFoundEvents;

// 32-bit shares (difficulty >= 1) the pool refused; the accepted ones are
// the existing "shares" counter. Saved with the other stats.
extern volatile uint32_t shares32Rejected;

// ESP32-S3 overlapped HW loop only (PIPELINED_S3_MINING). On the S3 every
// mbedTLS hash (the coinbase and merkle hashes of a new job, TLS handshakes)
// goes through the one SHA engine, which that loop holds almost all the time:
// a task that wants the engine would otherwise wait for the end of the
// miner's current chunk for every single hash, and a new job would take
// seconds to prepare. A task that is about to use the engine puts
// HW_SHA_REQUEST() in the scope doing so; the miner looks at the count every
// 256 nonces and stays off the engine until it is back to zero. Expands to
// nothing in every other build.
#if defined(PIPELINED_S3_MINING)
#include <atomic>
extern std::atomic<int> g_hwShaWanted;

struct HwShaRequest
{
  HwShaRequest() { g_hwShaWanted++; }
  ~HwShaRequest() { g_hwShaWanted--; }
};
#define HW_SHA_REQUEST() HwShaRequest hwShaRequest_
#else
#define HW_SHA_REQUEST()
#endif

// Mining
#define MAX_NONCE_STEP  5000000U
#define MAX_NONCE       25000000U
#define TARGET_NONCE    471136297U
#define DEFAULT_DIFFICULTY  0.00015
#define KEEPALIVE_TIME_ms       30000
#define POOLINACTIVITY_TIME_ms  60000

//#if defined(CONFIG_IDF_TARGET_ESP32S2) || defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32C3)
#define HARDWARE_SHA265
//#endif

// Build flag to A/B test: classic ESP32 (D0WD, e.g. CYD boards) has a SHA256
// engine with no way to load an external midstate, so its "hardware" path
// re-hashes the fixed first block on every nonce attempt (3 HW blocks/nonce)
// instead of reusing the cached midstate like the software path does (S2/S3/C3
// don't have this limitation). Define FORCE_SW_MINING to skip the hardware
// path entirely and run the optimized software miner on both cores, to compare
// real measured hashrate against the current hybrid HW+SW default.
#ifdef FORCE_SW_MINING
#undef HARDWARE_SHA265
#endif

// Build flag: pipelined classic-ESP32 HW mining loops that overlap the CPU's
// register traffic with the SHA engine's busy time instead of idle-spinning
// through it. The worker picks the fastest one that passes a known-answer
// test at power-on (see pl_select_loop in mining.cpp):
//   src/pipelined_hw_sha_classic_v2.S   schedule measured on the chip, no
//                                       status polling
//   src/pipelined_hw_sha_classic.cpp    the original BUSY-polling loop, ported
//                                       (MIT license, same upstream) from
//                                       dwespl/nerdminer-axehub
//   pl_mine_plain (mining.cpp)          the non-pipelined loop; last fallback,
//                                       and the only one used on chips before
//                                       revision 3
// Every candidate is still recomputed with the software nerd_sha256d_baked()
// path before being trusted, so a bug in the asm can only cost a missed
// share, never a bad submission. Only affects CONFIG_IDF_TARGET_ESP32
// (classic ESP32 / D0WD, e.g. CYD boards) when HARDWARE_SHA265 is active.
//#define PIPELINED_ASM_MINING

#define TARGET_BUFFER_SIZE 64

void runMonitor(void *name);

void runStratumWorker(void *name);

// Pool address, port, wallet and pool password from the LAN settings page:
// the stratum task swaps them into Settings between connections and
// reconnects to the pool, no restart needed. readPoolSettings() gives the
// ones in use, or the ones waiting to be swapped in.
struct TSettings;
void requestPoolChange(const TSettings &s);
void readPoolSettings(TSettings &s);

// Backup pool: after POOL_FAILOVER_TRIES failed attempts in a row on the pool
// in use, the stratum task moves to the other one (when a backup is set).
// While on the backup it checks every POOL_FAILBACK_ms whether the main pool
// takes a connection again, and goes back if it does. The pool in use:
#define POOL_FAILOVER_TRIES 3
#ifndef POOL_FAILBACK_ms
#define POOL_FAILBACK_ms    (10 * 60 * 1000UL)
#endif
bool onBackupPool(void);
const String &activePoolAddress(void);
int activePoolPort(void);

// Live miner state for the LAN status endpoint (/api/status).
struct MinerStatus
{
  float hashrateKHs;    // last 10 s
  float avgHashrateKHs; // since boot
  uint32_t accepted;    // shares the pool accepted since boot
  uint32_t rejected;    // ...and refused
  bool subscribed;      // connected and subscribed to the pool
  double poolDifficulty;
};
MinerStatus getMinerStatus(void);

void runMiner(void *name);

void minerWorkerSw(void * task_id);
void minerWorkerHw(void * task_id);

String printLocalTime(void);

void resetStat();
void updateBestDiff(double diff); // keep the best hash difficulty seen (any core)
double getBestDiff();
void saveStat();   // write the stats to NVS now (before a planned restart / power off)

typedef struct{
  uint8_t bytearray_target[32];
  uint8_t bytearray_pooltarget[32];
  uint8_t merkle_result[32];
  uint8_t bytearray_blockheader[128];
} miner_data;


#endif // UTILS_API_H