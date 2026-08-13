// Dedup window. This is the correctness gate that keeps redundancy from
// corrupting the consumer's metrics, so the duplicate and out-of-order cases
// are the point, not edge cases.
#include "dedup.h"

#include <algorithm>
#include <cstdint>
#include <random>
#include <vector>

#include "check.h"

namespace {

// Small window keeps the slide/eviction paths cheap to exercise.
using W = dedup::Window<10>;  // 1024 messages

void test_basic() {
  W w;
  CHECK(w.accept(1));
  CHECK(!w.accept(1));  // duplicate
  CHECK(w.accept(2));
  CHECK(!w.accept(2));
  CHECK_EQ(w.accepted(), 2u);
  CHECK_EQ(w.duplicates(), 2u);
  CHECK(w.seen(1));
  CHECK(w.seen(2));
  CHECK(!w.seen(3));
}

void test_out_of_order() {
  // Out-of-order delivery is expected and must not be mistaken for a duplicate:
  // consumer.cpp reads by ring slot, not by seq, so we never reorder.
  W w;
  CHECK(w.accept(10));
  CHECK(w.accept(7));
  CHECK(w.accept(9));
  CHECK(w.accept(8));
  CHECK(!w.accept(9));
  CHECK_EQ(w.accepted(), 4u);
  CHECK_EQ(w.duplicates(), 1u);
}

void test_window_slide() {
  W w;
  for (uint64_t s = 1; s <= W::kWindow; ++s) CHECK(w.accept(s));
  CHECK_EQ(w.accepted(), W::kWindow);

  // Pushing past the window retires the oldest entries.
  CHECK(w.accept(W::kWindow + 1));
  CHECK(w.base() > 0);

  // A seq that fell out of the window counts as too-old, not as new: accepting
  // it would risk a second publish of a message the consumer already has.
  CHECK(!w.accept(1));
  CHECK_EQ(w.too_old(), 1u);

  // Everything still inside the window is remembered.
  CHECK(!w.accept(W::kWindow));
  CHECK(w.duplicates() >= 1u);
}

void test_large_jump() {
  W w;
  CHECK(w.accept(5));
  // A jump far beyond the window must wipe cleanly, not leave stale bits that
  // would reject fresh sequence numbers.
  const uint64_t far = 5 + W::kWindow * 10;
  CHECK(w.accept(far));
  CHECK(!w.accept(far));
  CHECK(w.accept(far + 1));
  CHECK(!w.seen(5));
  for (uint64_t s = far + 2; s < far + 100; ++s) CHECK(w.accept(s));
}

void test_start_high() {
  // The stream may start at an arbitrary seq (a receiver attaching mid-flight
  // with from-edge semantics), which must not be treated as too-old.
  W w;
  const uint64_t start = 1000000;
  CHECK(w.accept(start));
  CHECK(!w.accept(start));
  CHECK(w.accept(start + 1));
  CHECK_EQ(w.too_old(), 0u);
}

void test_duplicate_storm() {
  // What redundancy actually produces: every message twice, interleaved and
  // slightly staggered. Exactly one publish per seq must survive.
  W w;
  std::mt19937_64 rng(7);
  std::vector<uint64_t> order;
  for (uint64_t s = 1; s <= 500; ++s) {
    order.push_back(s);
    order.push_back(s);
  }
  std::shuffle(order.begin(), order.end(), rng);

  uint64_t published = 0;
  for (uint64_t s : order)
    if (w.accept(s)) ++published;

  CHECK_EQ(published, 500u);
  CHECK_EQ(w.accepted(), 500u);
  CHECK_EQ(w.duplicates(), 500u);
  CHECK_EQ(w.too_old(), 0u);
}

void test_reset() {
  W w;
  CHECK(w.accept(42));
  w.reset();
  CHECK_EQ(w.accepted(), 0u);
  CHECK(w.accept(42));  // forgotten, so acceptable again
}

}  // namespace

int main() {
  test_basic();
  test_out_of_order();
  test_window_slide();
  test_large_jump();
  test_start_high();
  test_duplicate_storm();
  test_reset();
  return check_report("dedup");
}
