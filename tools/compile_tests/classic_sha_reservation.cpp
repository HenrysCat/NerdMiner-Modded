#include "classicShaEngines.h"
struct Fake {
  int blocked, owned=0, invalidUnlocks=0, attempts=0;
  constexpr Fake(int b):blocked(b){}
  constexpr bool tryLock(esp_sha_type e) {
    ++attempts;
    if((int)e==blocked)return false;
    owned|=1<<(int)e;return true;
  }
  constexpr void unlock(esp_sha_type e) {
    if(!(owned&(1<<(int)e)))++invalidUnlocks;
    owned&=~(1<<(int)e);
  }
};
constexpr bool failCleanly(int engine,int expectedAttempts) {
  Fake f(engine);
  return !tryClassicShaReservation(f) && f.owned==0 && f.invalidUnlocks==0 && f.attempts==expectedAttempts;
}
constexpr bool succeedsAndReleases() {
  Fake f(-1);
  if(!tryClassicShaReservation(f) || f.owned!=7)return false;
  releaseClassicShaReservation(f);
  return f.owned==0 && f.invalidUnlocks==0;
}
constexpr bool retryAfterContention() {
  Fake f(SHA2_512);
  if(tryClassicShaReservation(f) || f.owned)return false;
  f.blocked=-1;
  if(!tryClassicShaReservation(f) || f.owned!=7)return false;
  releaseClassicShaReservation(f);
  return f.owned==0 && f.invalidUnlocks==0;
}
static_assert(failCleanly(SHA1,1),"busy SHA1 must retain nothing");
static_assert(failCleanly(SHA2_256,2),"busy SHA256 must release SHA1");
static_assert(failCleanly(SHA2_512,3),"busy SHA512 must release both earlier slots");
static_assert(succeedsAndReleases(),"success must reserve and release all three slots");
static_assert(retryAfterContention(),"retry must succeed after contention clears");
