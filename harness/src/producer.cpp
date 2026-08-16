// Producer: generates a stream of market-data events (trade, BBO, or order-book
// snapshots), stamping each with a monotonic sequence id and a send timestamp,
// and publishes them into a shared-memory broadcast ring for one or more
// consumers. Fixed end of the benchmark harness -- a candidate replaces the
// transport, not this.
//
// Usage: producer [--shm NAME] [--slots N] [--count N] [--rate MSGS_PER_SEC]
//                 [--type trade|bbo|book|mixed]
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "message.h"
#include "shm_ring.h"
#include "shm_segment.h"
#include "util.h"

namespace {

enum class Kind { Trade, Bbo, Book, Mixed };

struct Config {
  std::string shm_name = "/fanout_ring";
  uint32_t slots = 1024;
  uint64_t count = 1000000;
  double rate = 0.0;
  Kind kind = Kind::Mixed;
};

bool is_power_of_two(uint32_t x) { return x != 0 && (x & (x - 1)) == 0; }

Kind parse_kind(const std::string& s) {
  if (s == "trade") return Kind::Trade;
  if (s == "bbo") return Kind::Bbo;
  if (s == "book") return Kind::Book;
  if (s == "mixed") return Kind::Mixed;
  fprintf(stderr, "--type must be trade|bbo|book|mixed (got %s)\n", s.c_str());
  std::exit(2);
}

Config parse_args(int argc, char** argv) {
  Config c;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) {
        fprintf(stderr, "missing value for %s\n", a.c_str());
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--shm") c.shm_name = next();
    else if (a == "--slots") c.slots = static_cast<uint32_t>(std::stoul(next()));
    else if (a == "--count") c.count = std::stoull(next());
    else if (a == "--rate") c.rate = std::stod(next());
    else if (a == "--type") c.kind = parse_kind(next());
    else {
      fprintf(stderr, "unknown arg: %s\n", a.c_str());
      std::exit(2);
    }
  }
  if (!is_power_of_two(c.slots)) {
    fprintf(stderr, "--slots must be a power of two (got %u)\n", c.slots);
    std::exit(2);
  }
  return c;
}

// Running aggregates over the trade stream. The producer is the right place to
// compute these: it is the source of truth, so the totals describe every trade that
// was generated rather than only the ones that happened to be delivered. That is
// what lets a consumer recover the correct total after a gap.
struct Totals {
  uint64_t quantity_lots = 0;
  uint64_t notional_ticks = 0;
  uint64_t trade_count = 0;
};

// Venue timestamp offsets. Real feeds show a gap between when the matching engine
// saw the event and when the gateway published it; these stand in for that.
inline constexpr int32_t kExchangeLagNs = 4200;
inline constexpr int32_t kMatchEngineLagNs = 130;

void fill_header(msg::Header& h, uint64_t seq, msg::Type type, uint8_t flags) {
  h.seq_id = seq;
  h.instrument = 0;
  h.type = static_cast<uint8_t>(type);
  h.flags = flags;
  h.exch_ts_delta_ns = kExchangeLagNs;
  h.match_ts_delta_ns = kMatchEngineLagNs;
  h.reserved = 0;
  h.send_ts_ns = util::now_ns();  // stamp as late as possible before publish
}

uint32_t build_trade(void* buf, uint64_t seq, Totals* totals) {
  auto& m = *reinterpret_cast<msg::Trade*>(buf);
  m.trade_id = 100000 + seq;
  m.price_ticks = 6500000 + static_cast<int64_t>(seq % 500) * 50;
  m.quantity_lots = 1 + static_cast<int64_t>(seq % 100) * 10;

  totals->quantity_lots += static_cast<uint64_t>(m.quantity_lots);
  totals->notional_ticks += static_cast<uint64_t>(m.price_ticks) *
                            static_cast<uint64_t>(m.quantity_lots);
  ++totals->trade_count;
  m.cum_quantity_lots = totals->quantity_lots;
  m.cum_notional_ticks = totals->notional_ticks;
  m.cum_trade_count = totals->trade_count;

  uint8_t flags = 0;
  if (seq & 1) flags |= msg::kFlagAggressorSell;
  flags |= static_cast<uint8_t>((seq % 4) << msg::kTickDirShift);
  fill_header(m.header, seq, msg::Type::Trade, flags);
  return sizeof(msg::Trade);
}

uint32_t build_bbo(void* buf, uint64_t seq) {
  auto& m = *reinterpret_cast<msg::Bbo*>(buf);
  m.update_id = 900000 + seq;
  const int64_t mid_ticks = 6500000 + static_cast<int64_t>(seq % 500) * 50;
  m.bid_price_ticks = mid_ticks - 50;
  m.spread_ticks = 100;
  m.bid_size_lots = static_cast<int32_t>(1500 + (seq % 50) * 100);
  m.ask_size_lots = static_cast<int32_t>(1500 + ((seq + 7) % 50) * 100);
  m.bid_order_count = static_cast<uint16_t>(3 + seq % 10);
  m.ask_order_count = static_cast<uint16_t>(3 + (seq + 3) % 10);
  fill_header(m.header, seq, msg::Type::Bbo, 0);
  return sizeof(msg::Bbo);
}

uint32_t build_book(void* buf, uint64_t seq) {
  auto& m = *reinterpret_cast<msg::OrderBook*>(buf);
  m.update_id = 900000 + seq;
  m.prev_update_gap = 1;
  m.reserved = 0;
  const int64_t mid_ticks = 6500000 + static_cast<int64_t>(seq % 500) * 50;

  m.bids.top_price_ticks = mid_ticks - 50;
  m.asks.top_price_ticks = mid_ticks + 50;
  m.bids.reserved = 0;
  m.asks.reserved = 0;
  for (uint32_t i = 0; i < msg::kBookDepth; ++i) {
    // Levels below the top are offsets from it: bids step down, asks step up.
    if (i > 0) {
      m.bids.price_offset_ticks[i - 1] = -static_cast<int32_t>(i) * 100;
      m.asks.price_offset_ticks[i - 1] = static_cast<int32_t>(i) * 100;
    }
    m.bids.size_lots[i] = static_cast<int32_t>(1000 + ((seq + i) % 40) * 100);
    m.bids.order_count[i] = static_cast<uint16_t>(2 + (seq + i) % 8);
    m.asks.size_lots[i] = static_cast<int32_t>(1000 + ((seq + i + 5) % 40) * 100);
    m.asks.order_count[i] = static_cast<uint16_t>(2 + (seq + i + 5) % 8);
  }
  m.checksum = static_cast<uint32_t>(seq * 2654435761u);
  fill_header(m.header, seq, msg::Type::OrderBook, msg::kFlagSnapshot);
  return sizeof(msg::OrderBook);
}

uint32_t build(Kind kind, uint64_t seq, void* buf, Totals* totals) {
  Kind k = kind;
  if (k == Kind::Mixed) {
    switch (seq % 3) {
      case 0: k = Kind::Trade; break;
      case 1: k = Kind::Bbo; break;
      default: k = Kind::Book; break;
    }
  }
  switch (k) {
    case Kind::Trade: return build_trade(buf, seq, totals);
    case Kind::Bbo: return build_bbo(buf, seq);
    default: return build_book(buf, seq);
  }
}

const char* kind_name(Kind k) {
  switch (k) {
    case Kind::Trade: return "trade";
    case Kind::Bbo: return "bbo";
    case Kind::Book: return "book";
    default: return "mixed";
  }
}

}  // namespace

int main(int argc, char** argv) {
  Config cfg = parse_args(argc, argv);

  shm::Segment seg =
      shm::Segment::open(cfg.shm_name, shm::region_size(cfg.slots), /*create=*/true);
  shm::Ring ring;
  ring.attach(seg.base(), cfg.slots, /*init=*/true);

  alignas(64) uint8_t frame[shm::kFrameCap];

  const uint64_t interval_ns =
      cfg.rate > 0.0 ? static_cast<uint64_t>(1e9 / cfg.rate) : 0;
  uint64_t next_send = util::now_ns();

  fprintf(stderr, "producer: shm=%s slots=%u count=%llu rate=%.0f type=%s\n",
          cfg.shm_name.c_str(), cfg.slots,
          static_cast<unsigned long long>(cfg.count), cfg.rate,
          kind_name(cfg.kind));

  uint64_t seq = 0;
  Totals totals;
  while (cfg.count == 0 || seq < cfg.count) {
    if (interval_ns) {
      while (util::now_ns() < next_send) {
      }
      next_send += interval_ns;
    }
    ++seq;
    const uint32_t len = build(cfg.kind, seq, frame, &totals);
    ring.publish(frame, len);
  }

  fprintf(stderr, "producer: sent %llu messages\n",
          static_cast<unsigned long long>(seq));
  seg.unlink();
  return 0;
}
