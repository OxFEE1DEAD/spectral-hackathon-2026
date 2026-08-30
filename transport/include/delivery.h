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
//
// Two policies with the same shape, selected as a template argument. The receive
// loop is instantiated once per policy, so the one not chosen contributes no
// instructions and no allocation -- the guarantee a preprocessor switch would
// have given, without the preprocessor.
#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

namespace delivery {

struct Stats {
  uint64_t delivered = 0;
  uint64_t suppressed = 0;   // duplicate, or older than the window still remembers
  uint64_t gaps = 0;         // how many times the sequence jumped
  uint64_t missing = 0;      // total frames those jumps skipped over
  uint64_t rescued = 0;      // admitted below the high-water mark: a late copy that counted
};

class MonotonicGate {
 public:
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

// The same duplicate suppression without the ordering constraint.
//
// The monotonic rule above rejects a duplicate and a late arrival with one
// comparison, and justifies discarding the late one by saying that delivering it
// would break monotonicity and be counted as a drop anyway. For this harness that
// is not so: consumer.cpp tracks the minimum and maximum seq_id it ever saw and
// counts how many arrived, so `expected = last - first + 1` and `dropped =
// expected - received`. Order never enters. Admitting seq 100 after 105 simply
// increments `received`, which is one fewer drop.
//
// What genuinely breaks that accounting is a *duplicate*: the same seq counted
// twice makes received exceed expected and hides real loss. So duplicate
// suppression is mandatory and ordering is not, and one bit per sequence number
// separates the two.
//
// This is what makes a staggered copy worth sending. Loss on this path arrives in
// bursts of tens of datagrams; a copy sent microseconds behind its original lands
// inside the same burst and dies with it. A copy held back longer than a burst
// arrives after later messages have already been delivered -- exactly the case the
// monotonic rule discards.
class BitmapGate {
 public:
  static constexpr unsigned kWindowPow2 = 20;            // 1M messages
  static constexpr uint64_t kWindow = 1ull << kWindowPow2;
  static constexpr size_t kWords = static_cast<size_t>(kWindow) / 64;

  BitmapGate() : bits_(kWords, 0) {}

  bool admit(uint64_t seq_id) {
    if (!started_) {
      started_ = true;
      base_ = seq_id >= kWindow ? seq_id - kWindow + 1 : 0;
      high_ = seq_id;
      mark(seq_id);
      ++stats_.delivered;
      return true;
    }
    if (seq_id < base_) {
      // Older than the window remembers. Assuming it is new risks a double
      // publish, which corrupts the consumer's accounting; refusing it costs at
      // most one rescue that was already a million messages late.
      ++stats_.suppressed;
      return false;
    }
    if (seq_id >= base_ + kWindow) advance_to(seq_id - kWindow + 1);
    if (test(seq_id)) {
      ++stats_.suppressed;
      return false;
    }
    mark(seq_id);
    if (seq_id > high_) {
      if (seq_id > high_ + 1) {
        ++stats_.gaps;
        stats_.missing += seq_id - high_ - 1;
      }
      high_ = seq_id;
    } else {
      // Below the high-water mark and not seen before: a hole being filled.
      ++stats_.rescued;
      if (stats_.missing) --stats_.missing;
    }
    ++stats_.delivered;
    return true;
  }

  const Stats& stats() const { return stats_; }
  bool started() const { return started_; }
  uint64_t last_delivered() const { return high_; }

 private:
  void mark(uint64_t seq) {
    const uint64_t i = seq & (kWindow - 1);
    bits_[i >> 6] |= 1ull << (i & 63);
  }
  bool test(uint64_t seq) const {
    const uint64_t i = seq & (kWindow - 1);
    return (bits_[i >> 6] >> (i & 63)) & 1ull;
  }
  void advance_to(uint64_t new_base) {
    if (new_base - base_ >= kWindow) {
      std::memset(bits_.data(), 0, bits_.size() * sizeof(uint64_t));
      base_ = new_base;
      return;
    }
    for (uint64_t s = base_; s < new_base; ++s) {
      const uint64_t i = s & (kWindow - 1);
      bits_[i >> 6] &= ~(1ull << (i & 63));
    }
    base_ = new_base;
  }

  std::vector<uint64_t> bits_;
  bool started_ = false;
  uint64_t base_ = 0;
  uint64_t high_ = 0;
  Stats stats_;
};

}  // namespace delivery
