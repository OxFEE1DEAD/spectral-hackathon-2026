// Histogram: bucket layout, the error bound we claim, and percentiles against
// an exactly-sorted reference.
#include "hist.h"

#include <algorithm>
#include <cstdint>
#include <random>
#include <vector>

#include "check.h"

namespace {

using H = hist::Latency;  // 7 sub-bits, 0.78% claimed error

void test_layout() {
  // Values below kSub are exact.
  for (uint64_t v = 0; v < H::kSub; ++v) {
    CHECK_EQ(H::bucket_of(v), static_cast<size_t>(v));
    CHECK_EQ(H::bucket_lower(H::bucket_of(v)), v);
    CHECK_EQ(H::bucket_upper(H::bucket_of(v)), v);
  }

  // Every value lands inside the bounds reported for its bucket, and the
  // mapping never goes backwards.
  size_t prev = 0;
  for (uint64_t v = 1; v < (1ull << 24); v += (v < 4096 ? 1 : v / 512)) {
    const size_t b = H::bucket_of(v);
    CHECK(H::bucket_lower(b) <= v);
    CHECK(v <= H::bucket_upper(b));
    CHECK(b >= prev);
    prev = b;
  }

  // The claimed relative error: bucket width over its lower bound must not
  // exceed 1/kSub. This is the accuracy the notebook quotes.
  for (uint64_t v = H::kSub; v < (1ull << 26); v = v + 1 + v / 97) {
    const size_t b = H::bucket_of(v);
    const uint64_t lo = H::bucket_lower(b);
    const uint64_t width = H::bucket_upper(b) - lo + 1;
    CHECK(width * H::kSub <= lo + H::kSub);
  }

  // Out-of-range values saturate rather than wrapping into a wrong bucket.
  CHECK_EQ(H::bucket_of(~0ull), H::kBuckets - 1);
}

void test_stats() {
  H h;
  CHECK_EQ(h.count(), 0u);
  CHECK_EQ(h.percentile(0.5), 0u);

  for (uint64_t v = 1; v <= 100; ++v) h.record(v);
  CHECK_EQ(h.count(), 100u);
  CHECK_EQ(h.min(), 1u);
  CHECK_EQ(h.max(), 100u);
  CHECK(h.mean() > 50.0 && h.mean() < 51.0);
  // Below kSub every bucket is exact, so nearest-rank must match exactly.
  CHECK_EQ(h.percentile(0.5), 50u);
  CHECK_EQ(h.percentile(0.99), 99u);
  CHECK_EQ(h.percentile(1.0), 100u);
}

void test_percentiles_vs_exact() {
  std::mt19937_64 rng(12345);
  // Long-tailed on purpose: the whole point is the far right of the curve.
  std::lognormal_distribution<double> body(std::log(26000.0), 0.25);

  std::vector<uint64_t> exact;
  H h;
  for (int i = 0; i < 200000; ++i) {
    uint64_t v = static_cast<uint64_t>(body(rng));
    if (i % 1000 == 0) v *= 8;  // injected tail
    exact.push_back(v);
    h.record(v);
  }
  std::sort(exact.begin(), exact.end());

  auto exact_pct = [&](double p) {
    size_t rank = static_cast<size_t>(p * static_cast<double>(exact.size()));
    if (static_cast<double>(rank) < p * static_cast<double>(exact.size())) ++rank;
    if (rank < 1) rank = 1;
    if (rank > exact.size()) rank = exact.size();
    return exact[rank - 1];
  };

  for (double p : {0.5, 0.9, 0.99, 0.999, 0.9999}) {
    const uint64_t e = exact_pct(p);
    const uint64_t a = h.percentile(p);
    // Upper-bound reporting: never optimistic, and never more than one bucket
    // width pessimistic.
    CHECK(a >= e);
    CHECK(a <= e + e / H::kSub + 2);
  }
  CHECK_EQ(h.max(), exact.back());
  CHECK_EQ(h.min(), exact.front());
}

void test_merge_and_reset() {
  H a, b;
  for (uint64_t v = 1; v <= 50; ++v) a.record(v);
  for (uint64_t v = 51; v <= 100; ++v) b.record(v);
  a.merge(b);
  CHECK_EQ(a.count(), 100u);
  CHECK_EQ(a.min(), 1u);
  CHECK_EQ(a.max(), 100u);
  CHECK_EQ(a.percentile(0.5), 50u);

  a.reset();
  CHECK_EQ(a.count(), 0u);
  CHECK_EQ(a.max(), 0u);

  // Merging into an empty histogram must adopt the other's min, not keep 0.
  H empty;
  empty.merge(b);
  CHECK_EQ(empty.min(), 51u);
  CHECK_EQ(empty.max(), 100u);
}

}  // namespace

int main() {
  test_layout();
  test_stats();
  test_percentiles_vs_exact();
  test_merge_and_reset();
  return check_report("hist");
}
