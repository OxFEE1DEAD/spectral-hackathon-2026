// On-the-wire datagram format between sender and receiver.
//
// One datagram carries a batch of whole messages. Messages never straddle a
// datagram boundary: a lost datagram must cost us exactly the messages inside
// it and nothing else. Straddling would make one loss corrupt its neighbours,
// which is the property the task calls out as the current format's weakness.
//
// Size: we stay inside 1472 bytes (1500 MTU - 20 IP - 8 UDP) by default even
// though a VPC supports 9001. IP fragmentation would make a datagram survive
// only if *every* fragment survives, multiplying the effective loss rate by the
// fragment count -- exactly backwards for a design whose whole thesis is loss
// tolerance. Jumbo frames stay an opt-in for same-subnet runs.
#pragma once

#include <cstdint>
#include <cstring>

namespace wire {

inline constexpr uint32_t kMagic = 0x53504331;  // "SPC1"

// Payload encoding. RAW ships harness frames verbatim and is what v0 uses;
// CODEC is the compact representation added later (plans/08_CODEC.txt).
enum Encoding : uint8_t {
  kEncRaw = 0,
  kEncCodec = 1,
};

// What this datagram is for. Redundant copies are kRoleData too -- a copy is
// indistinguishable from the original by design, and the receiver's dedup is
// what collapses them (plans/00_TRADEOFFS.txt D-07).
enum Role : uint8_t {
  kRoleData = 0,
  kRoleParity = 1,  // XOR/FEC parity over a window
  kRoleRepair = 2,  // retransmission answering a NACK
};

inline constexpr uint8_t kFlagEncodingMask = 0x0f;
inline constexpr uint8_t kFlagRoleShift = 4;

#pragma pack(push, 1)
struct DgramHeader {
  uint32_t magic;          // kMagic; cheap guard against cross-talk on a port
  uint32_t dgram_id;       // monotonic per stream; correlation + FEC window key
  uint16_t stream_id;      // which redundancy copy / path this is
  uint16_t n_msgs;         // messages in this datagram
  uint8_t flags;           // low nibble Encoding, high nibble Role
  uint8_t reserved[3];
  uint64_t first_msg_seq;  // base for delta-coded seq_id
  uint64_t base_ts_ns;     // base for delta-coded send_ts_ns
};
#pragma pack(pop)

static_assert(sizeof(DgramHeader) == 32, "DgramHeader must stay 32 bytes");

inline constexpr uint32_t kMaxDatagram = 1472;
inline constexpr uint32_t kMaxPayload = kMaxDatagram - sizeof(DgramHeader);

inline uint8_t make_flags(Encoding enc, Role role) {
  return static_cast<uint8_t>((static_cast<uint8_t>(enc) & kFlagEncodingMask) |
                              (static_cast<uint8_t>(role) << kFlagRoleShift));
}
inline Encoding encoding_of(uint8_t flags) {
  return static_cast<Encoding>(flags & kFlagEncodingMask);
}
inline Role role_of(uint8_t flags) {
  return static_cast<Role>(flags >> kFlagRoleShift);
}

// Per-message record in kEncRaw mode: a length followed by the harness frame
// verbatim. seq_id and send_ts_ns stay inside the frame, so v0 reproduces the
// framing contract trivially; the codec later delta-codes them against the
// header's bases and reconstructs them bit-for-bit.
#pragma pack(push, 1)
struct RawRecord {
  uint16_t len;
};
#pragma pack(pop)

// Builds one datagram into a caller-owned buffer. No allocation, no growth:
// append() refuses rather than reallocating, and the caller flushes and starts
// a new datagram. That refusal is what bounds a datagram to one MTU.
class Builder {
 public:
  Builder(uint8_t* buf, uint32_t cap) : buf_(buf), cap_(cap) {}

  void begin(uint32_t dgram_id, uint16_t stream_id, Encoding enc, Role role) {
    len_ = sizeof(DgramHeader);
    auto* h = reinterpret_cast<DgramHeader*>(buf_);
    h->magic = kMagic;
    h->dgram_id = dgram_id;
    h->stream_id = stream_id;
    h->n_msgs = 0;
    h->flags = make_flags(enc, role);
    std::memset(h->reserved, 0, sizeof(h->reserved));
    h->first_msg_seq = 0;
    h->base_ts_ns = 0;
    first_ = true;
  }

  bool empty() const { return header()->n_msgs == 0; }
  uint32_t size() const { return len_; }
  const uint8_t* data() const { return buf_; }

  // Two-step append, so a producer can write the frame body straight into the
  // transmit buffer instead of staging it somewhere first. The sender reads a
  // shared-memory slot directly into this pointer, which removes one 576-byte
  // copy per message from the hot path. Nothing is committed until
  // append_commit, so a read that turns out to have been lapped mid-copy is
  // simply abandoned.
  //
  // Returns null when the remaining room is smaller than `need`.
  uint8_t* append_ptr(uint32_t need) {
    if (len_ + sizeof(RawRecord) + need > cap_) return nullptr;
    return buf_ + len_ + sizeof(RawRecord);
  }

  void append_commit(uint32_t frame_len, uint64_t seq, uint64_t send_ts_ns) {
    auto* h = header();
    if (first_) {
      h->first_msg_seq = seq;
      h->base_ts_ns = send_ts_ns;
      first_ = false;
    }
    RawRecord rec{static_cast<uint16_t>(frame_len)};
    std::memcpy(buf_ + len_, &rec, sizeof(rec));
    len_ += sizeof(RawRecord) + frame_len;
    ++h->n_msgs;
  }

  // Returns false when the frame does not fit; the caller flushes and retries.
  bool append_raw(const void* frame, uint32_t frame_len, uint64_t seq,
                  uint64_t send_ts_ns) {
    const uint32_t need = sizeof(RawRecord) + frame_len;
    if (len_ + need > cap_) return false;

    auto* h = header();
    if (first_) {
      h->first_msg_seq = seq;
      h->base_ts_ns = send_ts_ns;
      first_ = false;
    }
    RawRecord rec{static_cast<uint16_t>(frame_len)};
    std::memcpy(buf_ + len_, &rec, sizeof(rec));
    std::memcpy(buf_ + len_ + sizeof(rec), frame, frame_len);
    len_ += need;
    ++h->n_msgs;
    return true;
  }

 private:
  DgramHeader* header() { return reinterpret_cast<DgramHeader*>(buf_); }
  const DgramHeader* header() const {
    return reinterpret_cast<const DgramHeader*>(buf_);
  }

  uint8_t* buf_;
  uint32_t cap_;
  uint32_t len_ = 0;
  bool first_ = true;
};

// Bounds-checked walk over a received datagram. Everything here is untrusted
// input off the network; a truncated or hostile datagram must fail the parse,
// never read past the buffer.
class Reader {
 public:
  bool reset(const uint8_t* buf, uint32_t len) {
    buf_ = buf;
    len_ = len;
    off_ = sizeof(DgramHeader);
    n_seen_ = 0;
    if (len < sizeof(DgramHeader)) return false;
    if (header()->magic != kMagic) return false;
    return true;
  }

  const DgramHeader* header() const {
    return reinterpret_cast<const DgramHeader*>(buf_);
  }
  uint16_t n_msgs() const { return header()->n_msgs; }
  Encoding encoding() const { return encoding_of(header()->flags); }
  Role role() const { return role_of(header()->flags); }

  // Yields the next raw frame. False means "no more" -- which includes
  // malformed, deliberately: a partially-parsed datagram gives up whatever it
  // decoded so far rather than throwing the good messages away.
  bool next_raw(const uint8_t** frame, uint32_t* frame_len) {
    if (n_seen_ >= header()->n_msgs) return false;
    if (off_ + sizeof(RawRecord) > len_) return false;
    RawRecord rec;
    std::memcpy(&rec, buf_ + off_, sizeof(rec));
    const uint32_t body = off_ + sizeof(RawRecord);
    if (body + rec.len > len_) return false;
    *frame = buf_ + body;
    *frame_len = rec.len;
    off_ = body + rec.len;
    ++n_seen_;
    return true;
  }

  // True when the datagram parsed exactly as advertised.
  bool complete() const { return n_seen_ == header()->n_msgs && off_ == len_; }

 private:
  const uint8_t* buf_ = nullptr;
  uint32_t len_ = 0;
  uint32_t off_ = 0;
  uint32_t n_seen_ = 0;
};

}  // namespace wire
