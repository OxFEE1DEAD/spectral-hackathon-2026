// Standalone assertion-based tests for the metrics accumulator and the shm ring.
// No test framework -- just asserts, so this stays dependency-free and builds
// with a single g++ invocation.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "message.h"
#include "metrics.h"
#include "shm_ring.h"

static void test_metrics_basic() {
  metrics::Accumulator acc;
  // seq 1..100 all delivered, latency == seq nanoseconds.
  for (uint64_t i = 1; i <= 100; ++i) acc.record(i, i);
  metrics::Report r = acc.report();

  assert(r.received == 100);
  assert(r.expected == 100);
  assert(r.dropped == 0);
  assert(r.drop_rate == 0.0);
  assert(r.lat_min == 1);
  assert(r.lat_max == 100);
  // Nearest-rank: p50 of 1..100 -> rank ceil(0.5*100)=50 -> value 50.
  assert(r.p50 == 50);
  assert(r.p99 == 99);
  assert(r.lat_mean > 50.0 && r.lat_mean < 51.0);
  printf("test_metrics_basic OK\n");
}

static void test_metrics_drops() {
  metrics::Accumulator acc;
  // Deliver only even sequence ids 2,4,...,100 -> 50 received, 100 expected.
  for (uint64_t i = 2; i <= 100; i += 2) acc.record(i, 10);
  metrics::Report r = acc.report();

  assert(r.received == 50);
  assert(r.expected == 99);  // last(100) - first(2) + 1
  assert(r.dropped == 49);
  assert(r.drop_rate > 0.49 && r.drop_rate < 0.50);
  printf("test_metrics_drops OK\n");
}

static void test_ring_roundtrip() {
  const uint32_t slots = 8;
  std::vector<uint8_t> mem(shm::region_size(slots));
  shm::Ring prod;
  prod.attach(mem.data(), slots, /*init=*/true);
  shm::Ring cons;
  cons.attach(mem.data(), slots, /*init=*/false);

  // Publish 5 frames (fits in the ring, no lapping).
  for (uint32_t i = 0; i < 5; ++i) {
    uint8_t frame[16];
    std::memset(frame, static_cast<int>(i), sizeof(frame));
    prod.publish(frame, sizeof(frame));
  }

  uint64_t read_index = 0;
  for (uint32_t i = 0; i < 5; ++i) {
    uint8_t out[64];
    uint32_t len = 0;
    uint64_t resume = 0;
    auto st = cons.read(read_index, out, &len, &resume);
    assert(st == shm::Ring::FrameStatus::kOk);
    assert(len == 16);
    assert(out[0] == static_cast<uint8_t>(i));
    ++read_index;
  }
  // Next read is empty (nothing published yet).
  uint8_t out[64];
  uint32_t len = 0;
  uint64_t resume = 0;
  assert(cons.read(read_index, out, &len, &resume) ==
         shm::Ring::FrameStatus::kEmpty);
  printf("test_ring_roundtrip OK\n");
}

static void test_ring_lapping() {
  const uint32_t slots = 4;
  std::vector<uint8_t> mem(shm::region_size(slots));
  shm::Ring prod;
  prod.attach(mem.data(), slots, /*init=*/true);
  shm::Ring cons;
  cons.attach(mem.data(), slots, /*init=*/false);

  // Publish 10 frames into a 4-slot ring -> reader sitting at index 0 is lapped.
  for (uint32_t i = 0; i < 10; ++i) {
    uint8_t frame[8];
    std::memset(frame, static_cast<int>(i), sizeof(frame));
    prod.publish(frame, sizeof(frame));
  }

  uint8_t out[64];
  uint32_t len = 0;
  uint64_t resume = 0;
  auto st = cons.read(0, out, &len, &resume);
  assert(st == shm::Ring::FrameStatus::kLapped);
  // Producer wrote 10, ring holds 4 -> safe resume position is 10 - 4 = 6.
  assert(resume == 6);

  // Reading from the resume point yields the frame published at index 6.
  st = cons.read(resume, out, &len, &resume);
  assert(st == shm::Ring::FrameStatus::kOk);
  assert(out[0] == 6);
  printf("test_ring_lapping OK\n");
}

// ---- message format --------------------------------------------------------

static void test_frame_size_follows_type() {
  // Every type is a fixed size, which is why the header no longer carries a
  // body_len. An unknown type must report 0 rather than a guess.
  assert(msg::frame_size(static_cast<uint8_t>(msg::Type::Trade)) == sizeof(msg::Trade));
  assert(msg::frame_size(static_cast<uint8_t>(msg::Type::Bbo)) == sizeof(msg::Bbo));
  assert(msg::frame_size(static_cast<uint8_t>(msg::Type::OrderBook)) ==
         sizeof(msg::OrderBook));
  assert(msg::frame_size(0) == 0);
  assert(msg::frame_size(99) == 0);
  printf("test_frame_size_follows_type OK\n");
}

static void test_venue_timestamps_recover_exactly() {
  // Venue timestamps travel as offsets from the message's own send_ts_ns so each
  // frame stays self-contained. They must come back exact, not approximate.
  msg::Header h{};
  h.send_ts_ns = 1700000000123456789ull;
  h.exch_ts_delta_ns = 4200;
  h.match_ts_delta_ns = 130;
  assert(msg::exchange_ts_ns(h) == h.send_ts_ns - 4200);
  assert(msg::match_engine_ts_ns(h) == h.send_ts_ns - 4200 - 130);
  printf("test_venue_timestamps_recover_exactly OK\n");
}

// The reason Trade carries running totals. A consumer tracking traded volume is
// simulated two ways over a stream with one message lost: one accumulates the
// per-trade quantity, the other reads the running total off each message.
static void test_cumulative_totals_survive_loss() {
  const uint64_t kCount = 10;
  const uint64_t kLost = 4;

  uint64_t truth_qty = 0;
  std::vector<msg::Trade> stream;
  for (uint64_t seq = 1; seq <= kCount; ++seq) {
    msg::Trade m{};
    m.header.seq_id = seq;
    m.header.type = static_cast<uint8_t>(msg::Type::Trade);
    m.price_ticks = 6500000 + static_cast<int64_t>(seq % 500) * 50;
    m.quantity_lots = 1 + static_cast<int64_t>(seq % 100) * 10;
    truth_qty += static_cast<uint64_t>(m.quantity_lots);
    // The producer counts every trade it generates, whether or not it survives.
    m.cum_quantity_lots = truth_qty;
    m.cum_trade_count = seq;
    stream.push_back(m);
  }

  uint64_t accumulating = 0;   // consumer that sums increments
  uint64_t reading_total = 0;  // consumer that reads the running total
  uint64_t seen = 0;
  for (uint64_t i = 0; i < kCount; ++i) {
    if (i == kLost) continue;  // the network drops this one
    accumulating += static_cast<uint64_t>(stream[i].quantity_lots);
    reading_total = stream[i].cum_quantity_lots;
    ++seen;
  }

  assert(seen == kCount - 1);
  // The accumulating consumer is permanently short and cannot detect it.
  assert(accumulating < truth_qty);
  // The one reading the running total is exactly right on the next message.
  assert(reading_total == truth_qty);
  printf("test_cumulative_totals_survive_loss OK "
         "(accumulated=%llu, cumulative=%llu, truth=%llu)\n",
         (unsigned long long)accumulating, (unsigned long long)reading_total,
         (unsigned long long)truth_qty);
}

static void test_format_is_smaller_than_original() {
  // The original harness format was Trade/Bbo 192 and OrderBook 576, so a mixed
  // stream averaged 320 bytes. These assertions pin the improvement so a future
  // change cannot quietly undo it.
  assert(sizeof(msg::Trade) == 80);
  assert(sizeof(msg::Bbo) == 64);
  assert(sizeof(msg::OrderBook) == 160);
  const double avg = (sizeof(msg::Trade) + sizeof(msg::Bbo) +
                      sizeof(msg::OrderBook)) / 3.0;
  assert(avg < 320.0 / 3.0 * 1.0);  // strictly better than the original average
  printf("test_format_is_smaller_than_original OK "
         "(mixed average %.1f vs 320 bytes; largest frame %u vs 576)\n",
         avg, msg::kMaxFrame);
}

int main() {
  test_frame_size_follows_type();
  test_venue_timestamps_recover_exactly();
  test_cumulative_totals_survive_loss();
  test_format_is_smaller_than_original();
  test_metrics_basic();
  test_metrics_drops();
  test_ring_roundtrip();
  test_ring_lapping();
  printf("ALL TESTS PASSED\n");
  return 0;
}
