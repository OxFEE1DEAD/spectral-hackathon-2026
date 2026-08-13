// Sliding-window duplicate suppression over seq_id.
//
// This is a hard correctness gate, not an optimisation. Every redundancy scheme
// we ship delivers the same message more than once, and consumer.cpp derives
// `expected = last_seq - first_seq + 1` while counting `received` per ring slot.
// Publishing a duplicate makes received exceed expected, drives `dropped`
// negative and computes every percentile over a doubled sample. See
// plans/00_TRADEOFFS.txt D-07.
//
// Out-of-order arrival is explicitly fine: consumer.cpp reads its ring by slot
// index rather than by seq_id, so a late copy is just a late copy. We never
// hold a message back to restore order (D-06).
#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

namespace dedup {

// Window is in messages. The default 2^20 covers ~1 s at 1M msg/s for 128 KB,
// which comfortably outlives both the retransmit buffer and any plausible
// reorder spread.
template <unsigned kWindowPow2 = 20>
class Window {
 public:
  static constexpr uint64_t kWindow = 1ull << kWindowPow2;
  static constexpr size_t kWords = static_cast<size_t>(kWindow) / 64;

  Window() : bits_(kWords, 0) {}

  // True when this seq_id has not been seen before and should be published.
  bool accept(uint64_t seq) {
    if (!started_) {
      started_ = true;
      base_ = seq >= kWindow ? seq - kWindow + 1 : 0;
    }

    if (seq < base_) {
      // Older than anything we still remember. Treat as duplicate: assuming it
      // is new risks a double publish, and at this age it is late enough that
      // the original almost certainly arrived.
      ++too_old_;
      return false;
    }

    if (seq >= base_ + kWindow) advance_to(seq - kWindow + 1);

    const uint64_t i = seq & (kWindow - 1);
    uint64_t& word = bits_[i >> 6];
    const uint64_t mask = 1ull << (i & 63);
    if (word & mask) {
      ++duplicates_;
      return false;
    }
    word |= mask;
    ++accepted_;
    return true;
  }

  // Whether `seq` has been seen, without recording it.
  bool seen(uint64_t seq) const {
    if (!started_ || seq < base_ || seq >= base_ + kWindow) return false;
    const uint64_t i = seq & (kWindow - 1);
    return (bits_[i >> 6] >> (i & 63)) & 1ull;
  }

  uint64_t accepted() const { return accepted_; }
  uint64_t duplicates() const { return duplicates_; }
  uint64_t too_old() const { return too_old_; }
  uint64_t base() const { return base_; }

  void reset() {
    std::memset(bits_.data(), 0, bits_.size() * sizeof(uint64_t));
    started_ = false;
    base_ = 0;
    accepted_ = duplicates_ = too_old_ = 0;
  }

 private:
  // Retire [base_, new_base). In steady state seq advances by one, so this
  // clears a single bit; a large jump is bounded by a full wipe.
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
  uint64_t accepted_ = 0;
  uint64_t duplicates_ = 0;
  uint64_t too_old_ = 0;
};

}  // namespace dedup
