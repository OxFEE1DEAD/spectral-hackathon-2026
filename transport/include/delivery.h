// Enforces strictly monotonic delivery of seq_id to the consumer.
//
// The requirement is that the consumer sees a strictly increasing sequence, and
// that anything outside it counts as a drop. One rule satisfies that and three
// other things at the same time:
//
//     deliver a frame if and only if its seq_id is greater than the highest
//     seq_id already delivered.
//
// That single comparison gives us:
//
//   * de-duplication -- the redundant copy of a datagram carries the same seq_ids
//     as the original, so every frame in it is rejected once the original landed.
//   * reordering protection -- a datagram that overtakes another cannot rewind the
//     stream; the straggler's frames are simply not admitted.
//   * late-arrival policy -- a frame that shows up after we have moved past it is
//     dropped rather than delivered out of order, because delivering it would
//     break monotonicity and be counted as a drop anyway.
//
// It also explains why proactive duplication is the right recovery mechanism here
// and retransmission is not. A retransmitted frame arrives after we have delivered
// later seq_ids, so the gate must reject it -- the round trip was wasted. A
// duplicate copy sent immediately after the original arrives *before* the next
// message exists, so it still fits through the gate and genuinely rescues the loss.
#pragma once

#include <cstdint>

namespace delivery {

class MonotonicGate {
 public:
  struct Stats {
    uint64_t delivered = 0;
    uint64_t suppressed = 0;   // duplicate or late: seq_id <= what we already sent
    uint64_t gaps = 0;         // how many times the sequence jumped
    uint64_t missing = 0;      // total frames those jumps skipped over
  };

  // True when this frame should be published to the consumer.
  bool admit(uint64_t seq_id) {
    if (!started_) {
      // The first frame we ever see defines the origin. We cannot know whether
      // anything preceded it, and counting the unseen prefix as loss would be
      // wrong -- the consumer's own accounting starts from the first seq_id too.
      started_ = true;
      last_ = seq_id;
      ++stats_.delivered;
      return true;
    }
    if (seq_id <= last_) {
      ++stats_.suppressed;
      return false;
    }
    if (seq_id > last_ + 1) {
      ++stats_.gaps;
      stats_.missing += seq_id - last_ - 1;
    }
    last_ = seq_id;
    ++stats_.delivered;
    return true;
  }

  const Stats& stats() const { return stats_; }
  bool started() const { return started_; }
  uint64_t last_delivered() const { return last_; }

 private:
  bool started_ = false;
  uint64_t last_ = 0;
  Stats stats_;
};

}  // namespace delivery
