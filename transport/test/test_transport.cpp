// Assertion tests for the wire packer/walker. Same style as the harness tests:
// no framework, one g++ invocation, no dependencies.
//
// The walker parses data that arrived off the network, so most of these tests are
// about rejecting malformed input rather than about the happy path.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "delivery.h"
#include "iou_backend.h"
#include "wire.h"

namespace {

// Build a frame that looks like something the producer published: a valid
// msg::Header and a recognisable byte pattern behind it. The frame length follows
// from the type, so the caller passes a type rather than a size.
uint32_t make_frame(uint8_t* buf, msg::Type type, uint64_t seq, uint8_t fill) {
  const uint32_t len = msg::frame_size(static_cast<uint8_t>(type));
  std::memset(buf, fill, len);
  msg::Header h{};
  h.seq_id = seq;
  h.send_ts_ns = 1000 + seq;
  h.type = static_cast<uint8_t>(type);
  std::memcpy(buf, &h, sizeof(h));
  return len;
}

void test_roundtrip_mixed_sizes() {
  std::vector<uint8_t> pkt(wire::kDefaultDatagram);
  wire::Packer packer(pkt.data(), wire::kDefaultDatagram);
  packer.reset(/*pkt_seq=*/7);

  // One of each type, which is what a mixed stream carries.
  const msg::Type types[] = {msg::Type::Trade, msg::Type::Bbo,
                             msg::Type::OrderBook};
  uint32_t sizes[3];
  uint8_t frame[shm::kFrameCap];
  for (uint32_t i = 0; i < 3; ++i) {
    sizes[i] = make_frame(frame, types[i], 100 + i, static_cast<uint8_t>(0xa0 + i));
    assert(packer.has_room(sizes[i]));
    packer.add(frame, sizes[i]);
  }
  assert(packer.count() == 3);

  uint32_t dlen = 0;
  const uint8_t* dgram = packer.finish(&dlen);
  assert(dlen == sizeof(wire::PktHeader) + sizes[0] + sizes[1] + sizes[2]);

  wire::Walker walker;
  assert(walker.begin(dgram, dlen));
  assert(walker.pkt_seq() == 7);
  assert(walker.msg_count() == 3);

  for (uint32_t i = 0; i < 3; ++i) {
    uint32_t len = 0;
    bool malformed = false;
    const uint8_t* got = walker.next(&len, &malformed);
    assert(got != nullptr && !malformed);
    assert(len == sizes[i]);
    // The frame must come back byte-identical: the consumer measures from these
    // bytes, so any mutation here would corrupt the measurement.
    const auto* h = reinterpret_cast<const msg::Header*>(got);
    assert(h->seq_id == 100 + i);
    assert(h->send_ts_ns == 1000 + 100 + i);
    assert(got[sizeof(msg::Header)] == static_cast<uint8_t>(0xa0 + i));
  }
  uint32_t len = 0;
  bool malformed = false;
  assert(walker.next(&len, &malformed) == nullptr);
  assert(!malformed);
  printf("test_roundtrip_mixed_sizes OK\n");
}

void test_capacity_is_respected() {
  // A datagram just big enough for one largest frame must accept exactly one.
  std::vector<uint8_t> pkt(wire::kMinDatagram);
  wire::Packer packer(pkt.data(), wire::kMinDatagram);
  packer.reset(1);

  uint8_t frame[shm::kFrameCap];
  const uint32_t len = make_frame(frame, msg::Type::OrderBook, 1, 0x11);
  assert(len == shm::kFrameCap);
  assert(packer.has_room(len));
  packer.add(frame, len);
  // No room for a second frame of any size that carries a header.
  assert(!packer.has_room(shm::kFrameCap));
  assert(!packer.has_room(sizeof(msg::Header)));
  printf("test_capacity_is_respected OK\n");
}

void test_reject_bad_magic_and_version() {
  std::vector<uint8_t> pkt(wire::kDefaultDatagram);
  wire::Packer packer(pkt.data(), wire::kDefaultDatagram);
  packer.reset(1);
  uint8_t frame[shm::kFrameCap];
  const uint32_t len22 = make_frame(frame, msg::Type::Trade, 1, 0x22);
  packer.add(frame, len22);
  uint32_t dlen = 0;
  const uint8_t* dgram = packer.finish(&dlen);

  std::vector<uint8_t> copy(dgram, dgram + dlen);
  wire::Walker walker;

  auto* h = reinterpret_cast<wire::PktHeader*>(copy.data());
  const uint32_t good_magic = h->magic;
  h->magic = 0xdeadbeef;
  assert(!walker.begin(copy.data(), dlen));

  h->magic = good_magic;
  h->version = wire::kVersion + 1;
  assert(!walker.begin(copy.data(), dlen));

  h->version = wire::kVersion;
  assert(walker.begin(copy.data(), dlen));
  printf("test_reject_bad_magic_and_version OK\n");
}

void test_reject_truncated_datagram() {
  std::vector<uint8_t> pkt(wire::kDefaultDatagram);
  wire::Packer packer(pkt.data(), wire::kDefaultDatagram);
  packer.reset(1);
  uint8_t frame[shm::kFrameCap];
  const uint32_t len33 = make_frame(frame, msg::Type::OrderBook, 1, 0x33);
  packer.add(frame, len33);
  uint32_t dlen = 0;
  const uint8_t* dgram = packer.finish(&dlen);

  wire::Walker walker;
  // Short of the header entirely.
  assert(!walker.begin(dgram, 8));
  // Header intact but the payload is cut: declared length no longer matches.
  assert(!walker.begin(dgram, dlen - 100));
  printf("test_reject_truncated_datagram OK\n");
}

void test_reject_unknown_and_oversized_type() {
  // Frame length now comes from the type, so the adversarial input is a bad type
  // rather than a bad length.
  std::vector<uint8_t> pkt(wire::kDefaultDatagram);
  wire::Packer packer(pkt.data(), wire::kDefaultDatagram);
  packer.reset(1);
  uint8_t frame[shm::kFrameCap];
  const uint32_t len = make_frame(frame, msg::Type::Trade, 1, 0x44);
  packer.add(frame, len);
  uint32_t dlen = 0;
  const uint8_t* dgram = packer.finish(&dlen);
  std::vector<uint8_t> copy(dgram, dgram + dlen);

  auto* mh = reinterpret_cast<msg::Header*>(copy.data() + sizeof(wire::PktHeader));
  wire::Walker walker;
  uint32_t got_len = 0;
  bool malformed = false;

  // A type nobody defined: length unknowable, so reject rather than guess.
  mh->type = 99;
  assert(walker.begin(copy.data(), dlen));
  assert(walker.next(&got_len, &malformed) == nullptr);
  assert(malformed);

  // A valid type whose frame is larger than the bytes actually present.
  mh->type = static_cast<uint8_t>(msg::Type::OrderBook);
  assert(walker.begin(copy.data(), dlen));
  assert(walker.next(&got_len, &malformed) == nullptr);
  assert(malformed);

  // Restored, it parses again.
  mh->type = static_cast<uint8_t>(msg::Type::Trade);
  assert(walker.begin(copy.data(), dlen));
  assert(walker.next(&got_len, &malformed) != nullptr);
  assert(!malformed);
  printf("test_reject_unknown_and_oversized_type OK\n");
}

void test_detect_count_mismatch() {
  std::vector<uint8_t> pkt(wire::kDefaultDatagram);
  wire::Packer packer(pkt.data(), wire::kDefaultDatagram);
  packer.reset(1);
  uint8_t frame[shm::kFrameCap];
  const uint32_t len55 = make_frame(frame, msg::Type::Trade, 1, 0x55);
  packer.add(frame, len55);
  uint32_t dlen = 0;
  const uint8_t* dgram = packer.finish(&dlen);
  std::vector<uint8_t> copy(dgram, dgram + dlen);

  // Claim two frames while carrying one: the bytes parse, the count does not.
  reinterpret_cast<wire::PktHeader*>(copy.data())->msg_count = 2;
  wire::Walker walker;
  assert(walker.begin(copy.data(), dlen));
  uint32_t len = 0;
  bool malformed = false;
  assert(walker.next(&len, &malformed) != nullptr);
  assert(!malformed);
  assert(walker.next(&len, &malformed) == nullptr);
  assert(malformed);
  printf("test_detect_count_mismatch OK\n");
}

// ---- delivery gate ----------------------------------------------------------
// This is where the delivery guarantee lives, so the cases that matter are the
// adversarial ones: duplicates, stragglers, and gaps.

void test_gate_passes_increasing_sequence() {
  delivery::MonotonicGate gate;
  for (uint64_t i = 100; i < 110; ++i) assert(gate.admit(i));
  assert(gate.stats().delivered == 10);
  assert(gate.stats().suppressed == 0);
  assert(gate.stats().gaps == 0);
  assert(gate.stats().missing == 0);
  printf("test_gate_passes_increasing_sequence OK\n");
}

void test_gate_suppresses_exact_duplicates() {
  // The redundant copy of a datagram carries the same seq_ids as the original.
  delivery::MonotonicGate gate;
  assert(gate.admit(1));
  assert(gate.admit(2));
  assert(gate.admit(3));
  assert(!gate.admit(1));
  assert(!gate.admit(2));
  assert(!gate.admit(3));
  assert(gate.stats().delivered == 3);
  assert(gate.stats().suppressed == 3);
  // A duplicate is not a gap: nothing was missing, we just saw it twice.
  assert(gate.stats().gaps == 0);
  printf("test_gate_suppresses_exact_duplicates OK\n");
}

void test_gate_rescues_via_duplicate_when_original_lost() {
  // Original of 4,5 is lost; its copy arrives before anything later exists, so the
  // copy still gets through. This is the case the whole duplication scheme exists
  // for, and it only works because the gate had not yet advanced past 5.
  delivery::MonotonicGate gate;
  assert(gate.admit(1));
  assert(gate.admit(2));
  assert(gate.admit(3));
  assert(gate.admit(4));  // arrived only in the duplicate
  assert(gate.admit(5));
  assert(gate.stats().gaps == 0);
  assert(gate.stats().missing == 0);
  assert(gate.stats().delivered == 5);
  printf("test_gate_rescues_via_duplicate_when_original_lost OK\n");
}

void test_gate_drops_straggler_that_arrives_too_late() {
  // A retransmission that turns up after we have moved on cannot be inserted
  // without breaking monotonicity, so it is dropped and stays counted as missing.
  delivery::MonotonicGate gate;
  assert(gate.admit(1));
  assert(gate.admit(4));           // 2 and 3 never arrived
  assert(gate.stats().gaps == 1);
  assert(gate.stats().missing == 2);
  assert(!gate.admit(2));          // far too late to help
  assert(!gate.admit(3));
  assert(gate.stats().suppressed == 2);
  assert(gate.stats().missing == 2);  // still missing; a late copy does not repair it
  assert(gate.admit(5));
  assert(gate.stats().delivered == 3);
  printf("test_gate_drops_straggler_that_arrives_too_late OK\n");
}

void test_gate_rejects_reordering() {
  // Out-of-order arrival must not rewind the published stream.
  delivery::MonotonicGate gate;
  assert(gate.admit(10));
  assert(gate.admit(12));
  assert(!gate.admit(11));  // overtaken; delivering it now would break monotonicity
  assert(gate.last_delivered() == 12);
  printf("test_gate_rejects_reordering OK\n");
}

void test_gate_first_frame_defines_origin() {
  // Attaching mid-stream must not report the unseen prefix as loss.
  delivery::MonotonicGate gate;
  assert(!gate.started());
  assert(gate.admit(500000));
  assert(gate.stats().gaps == 0);
  assert(gate.stats().missing == 0);
  printf("test_gate_first_frame_defines_origin OK\n");
}

void test_duplicate_flag_roundtrip() {
  std::vector<uint8_t> pkt(wire::kDefaultDatagram);
  wire::Packer packer(pkt.data(), wire::kDefaultDatagram);
  packer.reset(42);
  uint8_t frame[shm::kFrameCap];
  const uint32_t len66 = make_frame(frame, msg::Type::Trade, 7, 0x66);
  packer.add(frame, len66);
  uint32_t dlen = 0;
  uint8_t* dgram = const_cast<uint8_t*>(packer.finish(&dlen));

  wire::Walker w;
  assert(w.begin(dgram, dlen));
  assert(!w.duplicate());

  // Marking in place must not disturb anything else in the datagram.
  wire::mark_duplicate(dgram);
  assert(w.begin(dgram, dlen));
  assert(w.duplicate());
  assert(w.pkt_seq() == 42);
  assert(w.msg_count() == 1);
  uint32_t len = 0;
  bool malformed = false;
  const uint8_t* got = w.next(&len, &malformed);
  assert(got != nullptr && !malformed && len == len66);
  assert(reinterpret_cast<const msg::Header*>(got)->seq_id == 7);
  printf("test_duplicate_flag_roundtrip OK\n");
}

// ---- io_uring backend -----------------------------------------------------
//
// The ABI declared in iou_abi.h is checked at compile time by static_asserts on every
// structure size and field offset, so including the header already covers that. What a
// test has to cover is the part those cannot reach: that the ring is driven correctly.
// A round trip over loopback exercises submission, completion reaping, the multishot
// receive, and buffer recycling in one go.
//
// Skipped rather than failed where io_uring is unavailable -- the kernel may have it
// disabled, and a build machine is not necessarily the measurement machine.
void test_iouring_roundtrip() {
  iou::Options ropts;
  ropts.poll = iou::PollMode::kPolled;  // loopback: no NAPI instance to poll
  ropts.recv_buffers = 256;
  iou::Receiver rx;
  if (!rx.open("127.0.0.1", 53999, ropts)) {
    printf("test_iouring_roundtrip SKIPPED (io_uring unavailable)\n");
    return;
  }
  iou::Sender tx;
  if (!tx.open({"127.0.0.1:53999"}, 53999, iou::Options{})) {
    printf("test_iouring_roundtrip SKIPPED (io_uring send unavailable)\n");
    return;
  }

  // More datagrams than the buffer pool holds, so recycling has to work rather than
  // the pool merely being large enough to hide a leak.
  const int kCount = 2000;
  uint8_t out[512];
  int received = 0;
  for (int i = 0; i < kCount; ++i) {
    const uint32_t len = 64 + (i % 400);
    std::memset(out, static_cast<uint8_t>(i), len);
    assert(tx.send_all(out, len) == 1);
    for (;;) {
      const uint8_t* got = nullptr;
      const int n = rx.borrow(&got);
      if (n < 0) break;
      assert(n >= 64 && n <= 464);
      assert(got[0] == static_cast<uint8_t>(received));
      ++received;
      rx.release();
    }
  }
  // Loopback does not reorder or drop, so everything sent must arrive.
  for (int spin = 0; spin < 1000000 && received < kCount; ++spin) {
    const uint8_t* got = nullptr;
    const int n = rx.borrow(&got);
    if (n < 0) continue;
    assert(got[0] == static_cast<uint8_t>(received));
    ++received;
    rx.release();
  }
  assert(received == kCount);
  assert(tx.completion_errors() == 0);
  // One arm at startup and no more: the multishot receive should never have stopped.
  assert(rx.rearms() == 1);
  assert(rx.no_buffer_events() == 0);
  printf("test_iouring_roundtrip OK (%d datagrams, pool of %u)\n", received,
         ropts.recv_buffers);
}

}  // namespace

int main() {
  test_roundtrip_mixed_sizes();
  test_capacity_is_respected();
  test_reject_bad_magic_and_version();
  test_reject_truncated_datagram();
  test_reject_unknown_and_oversized_type();
  test_detect_count_mismatch();
  test_duplicate_flag_roundtrip();
  test_gate_passes_increasing_sequence();
  test_gate_suppresses_exact_duplicates();
  test_gate_rescues_via_duplicate_when_original_lost();
  test_gate_drops_straggler_that_arrives_too_late();
  test_gate_rejects_reordering();
  test_gate_first_frame_defines_origin();
  test_iouring_roundtrip();
  printf("ALL TESTS PASSED\n");
  return 0;
}
