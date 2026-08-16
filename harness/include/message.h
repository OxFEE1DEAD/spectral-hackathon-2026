// Wire messages for the fan-out harness.
//
// Every frame begins with a common Header carrying the sequence id and send
// timestamp the consumer uses to measure delivery; the rest of the frame describes
// a market-data event -- a trade, a top-of-book (BBO) update, or a 5-level order
// book snapshot.
//
// ---------------------------------------------------------------------------
// This format has been reworked from the original harness definition. The two
// framing fields the consumer measures from, `seq_id` and `send_ts_ns`, are
// unchanged and must stay that way. Everything else was examined field by field
// and either kept with a reason, narrowed, or removed. Two principles drove it:
//
//   1. Do not send the same information twice. The original carried every price
//      and size both as a double and as a scaled integer, plus `notional` which is
//      simply price x quantity, plus 85 bytes of `reserved` padding across the
//      three types, plus the symbol and venue as 32 bytes of text on every single
//      message.
//
//   2. Make a lost message survivable. BBO and OrderBook are *state snapshots*, so
//      a consumer that misses one recovers completely from the next. A trade is an
//      *event*, so a lost trade is lost information -- unless the message also
//      carries the running aggregates a consumer would otherwise accumulate. It
//      does now (`cum_*` below), which is what lets a consumer tracking volume or
//      VWAP re-derive correct totals from the very next trade after a gap.
//
// Principle 2 also rules out something tempting. Consecutive order books differ by
// only a level or two, so encoding each book as a delta against the previous one
// would be the largest remaining saving available -- and it is the wrong trade,
// because a lost delta is unrecoverable where a lost snapshot self-corrects. For
// the same reason nothing here is encoded relative to a previous message: every
// frame is self-contained, so a gap cannot corrupt what follows it.
//
// Result: 320 -> 101 bytes on average for a mixed stream, and a largest frame of
// 160 rather than 576 bytes, which shrinks every shared-memory ring slot as well.
// ---------------------------------------------------------------------------
#pragma once

#include <cstdint>

namespace msg {

inline constexpr uint32_t kBookDepth = 5;

enum class Type : uint16_t {
  Trade = 1,
  Bbo = 2,
  OrderBook = 3,
};

// ---- reference data --------------------------------------------------------
// Symbol, venue and currency names are *reference data*: static for the session
// and identical on every message. Sending 48 bytes of text per message to say so
// is waste, and no real venue does it -- they publish an instrument list out of
// band and put a numeric code on the wire. The currencies are not sent at all,
// because they are a property of the instrument rather than of the event.
struct Instrument {
  const char* symbol;
  const char* venue;
  const char* base_currency;
  const char* quote_currency;
};

inline constexpr Instrument kInstruments[] = {
    {"BTCUSDT", "BINANCE", "BTC", "USDT"},
};
inline constexpr uint16_t kInstrumentCount =
    static_cast<uint16_t>(sizeof(kInstruments) / sizeof(kInstruments[0]));

// ---- scaling ---------------------------------------------------------------
// Prices and sizes are carried only as scaled integers. That is the canonical,
// exactly representable form; the doubles the original format also carried were
// the same numbers a second time, and floating-point prices are a well-known
// source of comparison and rounding bugs. Consumers that want a double multiply.
inline constexpr double kPriceTickSize = 0.01;  // price = price_ticks * 0.01
inline constexpr double kQtyLotSize = 0.001;    // qty   = quantity_lots * 0.001

// ---- flags -----------------------------------------------------------------
// The original spent a whole byte on each of several booleans. Each of them
// carries real meaning and none is dropped; they are simply bits now. The
// aggressor side tells you which way the trade lifted, and block / RPI /
// liquidation each change how a print should be read as signal.
inline constexpr uint8_t kFlagAggressorSell = 1 << 0;
inline constexpr uint8_t kFlagBlockTrade = 1 << 1;
inline constexpr uint8_t kFlagRpi = 1 << 2;
inline constexpr uint8_t kFlagLiquidation = 1 << 3;
inline constexpr uint8_t kFlagSnapshot = 1 << 4;
inline constexpr uint8_t kTickDirShift = 5;  // 2 bits: 4 possible directions
inline constexpr uint8_t kTickDirMask = 0x3 << kTickDirShift;

// Common framing header at the start of every message.
struct Header {
  // These two are the harness contract with the consumer. Do not change them.
  uint64_t seq_id;      // monotonic, starts at 1
  uint64_t send_ts_ns;  // producer send timestamp, ns since epoch

  uint16_t instrument;  // index into kInstruments
  uint8_t type;         // Type
  uint8_t flags;        // kFlag* above

  // Venue timestamps, carried as offsets from this message's own send_ts_ns so
  // that each frame stays self-contained. Nanosecond offsets of this magnitude fit
  // an int32 comfortably, and the values are recovered exactly.
  //
  // `body_len` and `version` are gone: the first is fully derivable from `type`
  // (see frame_size), and the second describes the protocol rather than the
  // message, so it belongs in the datagram header instead of on every frame.
  int32_t exch_ts_delta_ns;   // send_ts_ns    - exchange_ts_ns
  int32_t match_ts_delta_ns;  // exchange_ts_ns - match_engine_ts_ns
  uint32_t reserved;          // keeps the following int64s 8-byte aligned
};
static_assert(sizeof(Header) == 32, "Header layout changed");

struct alignas(8) Trade {
  Header header;
  int64_t price_ticks;
  int64_t quantity_lots;
  uint64_t trade_id;  // the venue's identity for this print; not derivable

  // Running aggregates over the trade stream. These are what make a lost trade
  // survivable: a consumer that accumulates per-trade quantity is permanently
  // wrong after one missed message and cannot detect it, whereas a consumer that
  // reads these is exactly correct again on the next trade it receives. Wrapping
  // is harmless -- differences stay exact modulo 2^64.
  uint64_t cum_quantity_lots;
  uint64_t cum_notional_ticks;
  uint64_t cum_trade_count;
};
static_assert(sizeof(Trade) == 80, "Trade layout changed");

struct alignas(8) Bbo {
  Header header;
  uint64_t update_id;      // absolute: a cross-message delta would desync on loss
  int64_t bid_price_ticks;
  int32_t spread_ticks;    // ask = bid + spread; a spread is small by nature
  int32_t bid_size_lots;
  int32_t ask_size_lots;
  uint16_t bid_order_count;
  uint16_t ask_order_count;
};
static_assert(sizeof(Bbo) == 64, "Bbo layout changed");

// One side of the book. The top level is absolute and the levels beneath it are
// offsets from that top -- safe because they travel inside the same frame, so loss
// cannot separate them.
struct Side {
  int64_t top_price_ticks;
  int32_t price_offset_ticks[kBookDepth - 1];
  int32_t size_lots[kBookDepth];
  uint16_t order_count[kBookDepth];
  uint16_t reserved;
};
static_assert(sizeof(Side) == 56, "Side layout changed");

struct alignas(8) OrderBook {
  Header header;
  uint64_t update_id;
  uint16_t prev_update_gap;  // update_id - prev_update_id, normally 1
  uint16_t reserved;
  uint32_t checksum;         // the snapshot's own integrity check: worth its bytes
  Side bids;
  Side asks;
};
static_assert(sizeof(OrderBook) == 160, "OrderBook layout changed");

inline constexpr uint32_t kMaxFrame = sizeof(OrderBook);
static_assert(sizeof(Trade) <= kMaxFrame, "kMaxFrame must fit every message");
static_assert(sizeof(Bbo) <= kMaxFrame, "kMaxFrame must fit every message");

// Frame size for a message type. This is why `body_len` no longer needs to be sent
// on every message: each type is a fixed size, so the length is implied by the two
// bits that say which type it is. Returns 0 for an unknown type, which callers
// parsing untrusted input must treat as malformed.
inline constexpr uint32_t frame_size(uint8_t type) {
  switch (static_cast<Type>(type)) {
    case Type::Trade: return sizeof(Trade);
    case Type::Bbo: return sizeof(Bbo);
    case Type::OrderBook: return sizeof(OrderBook);
  }
  return 0;
}

// Helpers for reconstructing the absolute venue timestamps a consumer may want.
inline uint64_t exchange_ts_ns(const Header& h) {
  return h.send_ts_ns - static_cast<uint64_t>(h.exch_ts_delta_ns);
}
inline uint64_t match_engine_ts_ns(const Header& h) {
  return exchange_ts_ns(h) - static_cast<uint64_t>(h.match_ts_delta_ns);
}

}  // namespace msg
