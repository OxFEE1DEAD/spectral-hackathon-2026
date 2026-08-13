// Log-linear latency histogram, cheap enough to leave switched on.
//
// Values below 2^kSubBits are recorded exactly. Above that, every octave is cut
// into 2^kSubBits linear sub-buckets, so the relative error is bounded by
// 2^-kSubBits -- 0.78% at the default 7 bits, which resolves 26.0 from 26.2 us.
// Recording is a count-leading-zeros, a shift and an increment: ~1-2 ns. At
// eight stages and 1M msg/s that is ~1% of one core, which is what makes
// always-on per-stage attribution affordable (plans/00_TRADEOFFS.txt D-16).
//
// Reported percentiles are the *upper* bound of the containing bucket. We never
// claim a latency better than the data supports; exact min/max/sum are tracked
// alongside so the bias is visible.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace hist {

template <unsigned kSubBits = 7, unsigned kMaxPow2 = 32>
class Histogram {
 public:
  static_assert(kSubBits >= 1 && kSubBits <= 16, "kSubBits out of range");
  static_assert(kMaxPow2 > kSubBits, "kMaxPow2 must exceed kSubBits");

  static constexpr uint64_t kSub = 1ull << kSubBits;
  // Octave index e runs 1..kMaxOctave; bucket 0..kSub-1 is the exact region.
  static constexpr unsigned kMaxOctave = kMaxPow2 - kSubBits + 1;
  static constexpr size_t kBuckets = static_cast<size_t>(kMaxOctave + 1) * kSub;

  Histogram() : counts_(kBuckets, 0) {}

  void record(uint64_t value) {
    counts_[bucket_of(value)]++;
    if (count_ == 0 || value < min_) min_ = value;
    if (value > max_) max_ = value;
    sum_ += value;
    ++count_;
  }

  void reset() {
    std::memset(counts_.data(), 0, counts_.size() * sizeof(uint64_t));
    count_ = 0;
    sum_ = 0;
    min_ = 0;
    max_ = 0;
  }

  void merge(const Histogram& other) {
    for (size_t i = 0; i < kBuckets; ++i) counts_[i] += other.counts_[i];
    if (other.count_ != 0) {
      if (count_ == 0 || other.min_ < min_) min_ = other.min_;
      if (other.max_ > max_) max_ = other.max_;
      sum_ += other.sum_;
      count_ += other.count_;
    }
  }

  uint64_t count() const { return count_; }
  uint64_t min() const { return count_ ? min_ : 0; }
  uint64_t max() const { return count_ ? max_ : 0; }
  double mean() const {
    return count_ ? static_cast<double>(sum_) / static_cast<double>(count_) : 0.0;
  }

  // p in [0,1]. Nearest-rank over the bucket counts, returning the bucket's
  // upper bound.
  uint64_t percentile(double p) const {
    if (count_ == 0) return 0;
    if (p <= 0.0) return min_;
    if (p >= 1.0) return max_;

    uint64_t rank = static_cast<uint64_t>(p * static_cast<double>(count_));
    if (static_cast<double>(rank) < p * static_cast<double>(count_)) ++rank;
    if (rank < 1) rank = 1;

    uint64_t cum = 0;
    for (size_t i = 0; i < kBuckets; ++i) {
      cum += counts_[i];
      if (cum >= rank) {
        const uint64_t hi = bucket_upper(i);
        // The last populated bucket can extend past the largest value we
        // actually saw; do not over-report in that case.
        return hi > max_ ? max_ : hi;
      }
    }
    return max_;
  }

  // Lowest value that lands in `index`.
  static uint64_t bucket_lower(size_t index) {
    if (index < kSub) return index;
    const uint64_t e = index / kSub;
    const uint64_t mant = index % kSub;
    return (kSub + mant) << (e - 1);
  }

  // Highest value that lands in `index`.
  static uint64_t bucket_upper(size_t index) {
    if (index < kSub) return index;
    const uint64_t e = index / kSub;
    return bucket_lower(index) + (1ull << (e - 1)) - 1;
  }

  static size_t bucket_of(uint64_t value) {
    if (value < kSub) return static_cast<size_t>(value);
    unsigned k = 63u - static_cast<unsigned>(__builtin_clzll(value));
    if (k > kMaxPow2) return kBuckets - 1;  // saturate rather than corrupt
    const uint64_t e = k - kSubBits + 1;
    const uint64_t mant = (value >> (k - kSubBits)) - kSub;
    return static_cast<size_t>(e * kSub + mant);
  }

  uint64_t bucket_count(size_t index) const { return counts_[index]; }

  // `label,bucket_upper_ns,count` for every non-empty bucket. The notebook
  // rebuilds the full distribution from this, not just the percentiles --
  // the task asks for the distribution as a whole.
  void write_csv(FILE* f, const char* label) const {
    for (size_t i = 0; i < kBuckets; ++i) {
      if (counts_[i] == 0) continue;
      std::fprintf(f, "%s,%llu,%llu\n", label,
                   static_cast<unsigned long long>(bucket_upper(i)),
                   static_cast<unsigned long long>(counts_[i]));
    }
  }

 private:
  std::vector<uint64_t> counts_;
  uint64_t count_ = 0;
  uint64_t sum_ = 0;
  uint64_t min_ = 0;
  uint64_t max_ = 0;
};

using Latency = Histogram<7, 32>;

}  // namespace hist
