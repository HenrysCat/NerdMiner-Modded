#ifndef SUBMIT_FILTER_H
#define SUBMIT_FILTER_H

#include <stdint.h>

// Nonces already sent to the pool for the current job.
//
// On the classic ESP32 the hardware miner walks the nonces in byte-swapped
// order (what makes its pipelined loop fast) while the software miner takes
// plain consecutive chunks: about 1.3 % of the software miner's nonces are
// hashed by both, and a share among them was sent twice, the second time
// refused by the pool as a duplicate. The stratum task asks here before
// sending a share and skips one it already sent.
//
// A share is a rare event (a few per minute at most), so a small ring of the
// last ones is plenty; it is emptied when the job changes.
class SubmitFilter
{
public:
  static const unsigned SIZE = 64;

  // True if (job, nonce) is new: remember it and send the share.
  // False if it was already sent for this job: skip it.
  bool firstTime(uint32_t job, uint32_t nonce)
  {
    if (job != job_)
    {
      job_ = job;
      count_ = 0;
      head_ = 0;
    }
    for (unsigned i = 0; i < count_; ++i)
      if (nonces_[i] == nonce)
        return false;
    nonces_[head_] = nonce;
    head_ = (head_ + 1) % SIZE;
    if (count_ < SIZE)
      count_++;
    return true;
  }

private:
  uint32_t job_ = 0xFFFFFFFF;
  uint32_t nonces_[SIZE];
  unsigned count_ = 0;
  unsigned head_ = 0;
};

#endif // SUBMIT_FILTER_H
