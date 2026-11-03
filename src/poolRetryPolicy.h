#pragma once
#include <stdint.h>
constexpr uint32_t poolRetryDelayMs(unsigned failures) {
  return failures<=1?1000:failures==2?2000:failures==3?4000:
    failures==4?8000:failures==5?16000:30000;
}
static_assert(poolRetryDelayMs(0)==1000 && poolRetryDelayMs(1)==1000,"first retry");
static_assert(poolRetryDelayMs(3)==4000 && poolRetryDelayMs(5)==16000,"backoff");
static_assert(poolRetryDelayMs(6)==30000 && poolRetryDelayMs(255)==30000,"bounded retry");
