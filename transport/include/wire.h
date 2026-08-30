// Wire format for the sender -> receiver network hop, plus the two small helpers
// that build and walk a datagram.
//
// A datagram carries one or more harness frames. Batching is *opportunistic*:
// the sender packs whatever is already sitting in the source ring and sends
// immediately. It never waits for a datagram to fill and never arms a timer, so
// at a low message rate a datagram holds a single frame (lowest latency) and
// under saturation it fills up by itself (highest throughput). There is no
// batch-size knob to get wrong.
//
// This first version carries frames byte-identical to what the producer
// published, so the receiver republishes them without interpreting anything.
// Field-level compaction is a later phase and changes only the bytes behind
// PktHeader -- Packer/Walker keep the same shape.
#pragma once

#include <cstdint>
#include <cstring>

#include "message.h"
#include "shm_ring.h"

namespace wire {

inline constexpr uint32_t kMagic = 0x57495231;  // "WIR1"
inline constexpr uint8_t kVersion = 1;

// Standard Ethernet MTU (1500) less the IPv4 (20) and UDP (8) headers.
//
// This transport deliberately assumes an MTU that can hold one whole message, and
// therefore does not fragment. The assumption is worth stating plainly because it
// is a design decision with consequences:
//
//   * The largest message is a 5-level order book, so any MTU from
//     ~600 bytes upward carries every message whole. 1500 is the near-universal
//     Ethernet default and gives room for two order books or seven quotes per
//     datagram.
//   * Not fragmenting means one lost datagram costs exactly the messages inside
//     it. Under fragmentation, losing any one fragment destroys the whole message,
//     so a message spread over six fragments is six times as likely to be lost.
//     Avoiding fragmentation is a loss-resilience decision, not just a simplifying
//     one.
//   * We also never let IP fragment for us: a datagram larger than the path MTU
//     would be split by the network with the same amplified-loss problem, minus
//     any visibility. Keeping every datagram inside one MTU keeps that control.
//
// Jumbo frames (ENA supports 9001) are the other direction and worth measuring
// separately, so this is a runtime setting rather than a compile-time constant.
inline constexpr uint32_t kDefaultDatagram = 1472;

// Marks the redundant second copy of a datagram. The receiver does not need this
// to behave correctly -- the monotonic gate rejects the copy on seq_id alone -- but
// it lets us measure how often a copy actually rescued a lost original, which is
// the only honest way to justify spending the bandwidth.
inline constexpr uint8_t kFlagDuplicate = 0x01;

struct PktHeader {
  uint32_t magic;
  uint8_t version;
  uint8_t flags;         // kFlagDuplicate
  uint16_t msg_count;    // frames following this header
  uint32_t payload_len;  // bytes of frames following this header
  uint32_t pad;
  uint64_t pkt_seq;      // datagram counter, starts at 1
  // Stamped immediately before the datagram is handed to the kernel. Lets the
  // receiver split end-to-end latency into "waited in the source ring" and "spent
  // getting here", which are different problems with different fixes. Costs 8 bytes
  // once per datagram rather than per message, so batching amortises it away.
  uint64_t sender_ts_ns;
};
static_assert(sizeof(PktHeader) == 32, "PktHeader must stay 32 bytes");

// Flip an already-built datagram into its duplicate copy, in place. The redundant
// send reuses the original bytes rather than rebuilding or copying them.
inline void mark_duplicate(uint8_t* pkt) {
  reinterpret_cast<PktHeader*>(pkt)->flags |= kFlagDuplicate;
}

// Stamp departure time into an already-built datagram, as late as possible. The
// duplicate copy is re-stamped when it goes out, so its wire time is its own rather
// than the original's.
inline void stamp_send_ts(uint8_t* pkt, uint64_t ts_ns) {
  reinterpret_cast<PktHeader*>(pkt)->sender_ts_ns = ts_ns;
}

inline bool is_duplicate(const PktHeader& h) {
  return (h.flags & kFlagDuplicate) != 0;
}

// The floor implied by the no-fragmentation decision: header plus one largest
// message. Configuring a smaller datagram is rejected at startup rather than
// silently stalling the relay on the first order book it cannot send.
inline constexpr uint32_t kMinDatagram = sizeof(PktHeader) + shm::kFrameCap;

// Builds one datagram. Owns no memory: the caller supplies the buffer, which
// lives for the whole run so the send path never allocates.
class Packer {
 public:
  Packer(uint8_t* buf, uint32_t cap) : buf_(buf), cap_(cap) {}

  // Start a fresh datagram. pkt_seq is the sender's datagram counter.
  void reset(uint64_t pkt_seq) {
    len_ = sizeof(PktHeader);
    count_ = 0;
    pkt_seq_ = pkt_seq;
  }

  bool has_room(uint32_t frame_len) const { return len_ + frame_len <= cap_; }

  bool empty() const { return count_ == 0; }
  uint16_t count() const { return count_; }

  void add(const void* frame, uint32_t frame_len) {
    std::memcpy(buf_ + len_, frame, frame_len);
    len_ += frame_len;
    ++count_;
  }

  // Stamp the header and hand back the bytes to put on the wire.
  const uint8_t* finish(uint32_t* out_len) {
    auto* h = reinterpret_cast<PktHeader*>(buf_);
    h->magic = kMagic;
    h->version = kVersion;
    h->flags = 0;
    h->msg_count = count_;
    h->pad = 0;
    h->payload_len = len_ - sizeof(PktHeader);
    h->pkt_seq = pkt_seq_;
    h->sender_ts_ns = 0;  // stamped by the caller immediately before sending
    *out_len = len_;
    return buf_;
  }

 private:
  uint8_t* buf_;
  uint32_t cap_;
  uint32_t len_ = sizeof(PktHeader);
  uint16_t count_ = 0;
  uint64_t pkt_seq_ = 0;
};

// Validates a received datagram and iterates the frames inside it.
//
// Everything here is treated as untrusted input: a truncated or corrupted
// datagram must be rejected, not walked off the end of the buffer. The checks
// are a handful of integer comparisons per datagram, not per byte.
class Walker {
 public:
  // Returns false if this is not a well-formed datagram of ours.
  bool begin(const uint8_t* pkt, uint32_t len) {
    if (len < sizeof(PktHeader)) return false;
    std::memcpy(&hdr_, pkt, sizeof(PktHeader));
    if (hdr_.magic != kMagic || hdr_.version != kVersion) return false;
    // The declared payload has to match what actually arrived.
    if (hdr_.payload_len != len - sizeof(PktHeader)) return false;
    body_ = pkt + sizeof(PktHeader);
    end_ = hdr_.payload_len;
    off_ = 0;
    seen_ = 0;
    return true;
  }

  uint64_t pkt_seq() const { return hdr_.pkt_seq; }
  uint16_t msg_count() const { return hdr_.msg_count; }
  uint64_t sender_ts_ns() const { return hdr_.sender_ts_ns; }
  bool duplicate() const { return is_duplicate(hdr_); }

  // Next frame, or nullptr at the end of the datagram. Sets *out_len to the
  // frame size. Returns nullptr and sets *malformed on a frame that does not
  // fit or declares an impossible length.
  const uint8_t* next(uint32_t* out_len, bool* malformed) {
    *malformed = false;
    if (off_ == end_) {
      // A frame count that disagrees with the bytes present means the datagram
      // was built or truncated wrongly, even though every frame parsed.
      if (seen_ != hdr_.msg_count) *malformed = true;
      return nullptr;
    }
    if (off_ + sizeof(msg::Header) > end_) {
      *malformed = true;
      return nullptr;
    }
    msg::Header h;
    std::memcpy(&h, body_ + off_, sizeof(h));
    // The frame length is implied by the message type rather than carried in every
    // header: each type is a fixed size. An unrecognised type yields 0, which is
    // treated as malformed rather than guessed at.
    const uint32_t frame_len = msg::frame_size(h.type);
    if (frame_len == 0 || off_ + frame_len > end_) {
      *malformed = true;
      return nullptr;
    }
    const uint8_t* frame = body_ + off_;
    off_ += frame_len;
    ++seen_;
    *out_len = frame_len;
    return frame;
  }

 private:
  PktHeader hdr_{};
  const uint8_t* body_ = nullptr;
  uint32_t end_ = 0;
  uint32_t off_ = 0;
  uint16_t seen_ = 0;
};

}  // namespace wire
