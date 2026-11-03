#ifndef NM_HASHRATE_HISTORY_H
#define NM_HASHRATE_HISTORY_H
#include <stdint.h>
#include <stddef.h>

// Single owner: webConfigProcess and HTTP handlers run on the Arduino task.
// 65535 denotes a missing time bucket; valid rates saturate at 6553.4 KH/s.
template <size_t N, uint32_t Period> struct HashrateHistory {
  uint16_t values[N] = {};
  size_t head = 0, count = 0;
  uint64_t bucket = 0;
  uint32_t sum = 0, samples = 0;
  void push(uint16_t value) {
    values[head] = value; head = (head + 1) % N;
    if (count < N) ++count;
  }
  void sample(uint64_t seconds, uint16_t value) {
    const uint64_t next = seconds / Period;
    if (!count || next != bucket) {
      if (count && next > bucket + 1) {
        uint64_t missing = next - bucket - 1;
        if (missing >= N) { count = 0; head = 0; missing = N - 1; }
        while (missing--) push(65535);
      }
      bucket = next; sum = 0; samples = 0; push(65535);
    }
    sum += value; ++samples;
    values[(head + N - 1) % N] = (sum + samples / 2) / samples;
  }
  uint16_t at(size_t i) const { return values[(head + N - count + i) % N]; }
};
#endif
