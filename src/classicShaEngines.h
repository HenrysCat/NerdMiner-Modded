#pragma once
#include <Arduino.h>
#if defined(CONFIG_IDF_TARGET_ESP32)
#include <sha/sha_parallel_engine.h>

// Direct register access and peripheral reset bypass the driver's shared
// memory lock. Reserve ALL three engine slots before touching either.
// Nonblocking attempts never retain a partial reservation while waiting.
#if __cplusplus >= 201402L
#define NM_SHA_TESTABLE constexpr
#else
#define NM_SHA_TESTABLE inline
#endif
template<class Ops> static NM_SHA_TESTABLE bool tryClassicShaReservation(Ops &ops) {
  if(ops.tryLock(SHA1)) {
    if(ops.tryLock(SHA2_256)) {
      if(ops.tryLock(SHA2_512))return true;
      ops.unlock(SHA2_256);
    }
    ops.unlock(SHA1);
  }
  return false;
}
template<class Ops> static NM_SHA_TESTABLE void releaseClassicShaReservation(Ops &ops) {
  ops.unlock(SHA2_512);
  ops.unlock(SHA2_256);
  ops.unlock(SHA1);
}
#undef NM_SHA_TESTABLE
struct ClassicShaOps {
  bool tryLock(esp_sha_type type){return esp_sha_try_lock_engine(type);}
  void unlock(esp_sha_type type){esp_sha_unlock_engine(type);}
};
static inline void lockClassicShaEngines() {
  ClassicShaOps ops;
  while(!tryClassicShaReservation(ops))vTaskDelay(1);
}
static inline void unlockClassicShaEngines() {
  ClassicShaOps ops;
  releaseClassicShaReservation(ops);
}
#endif
