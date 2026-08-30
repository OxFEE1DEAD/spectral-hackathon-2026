// A receive path whose polling never stops, however busy the rest of the relay is.
//
// The problem this solves is a feedback loop the measurements exposed rather than one
// predicted in advance. The single-threaded receiver runs
//
//     recv -> parse -> gate -> publish -> recv -> ...
//
// and kernel busy-polling only happens *while the thread is inside recv*. So whenever the
// relay falls behind -- a batch to publish, a burst to drain -- it stops polling, and
// packets fall back to being delivered by a softirq on a housekeeping core, which measured
// 24 us worse at the median and 3.4x worse at p99.9 than busy-poll on the measured path.
// Falling behind therefore makes delivery slower, which makes it fall further behind.
//
// The data fits that shape closely. At 200k msg/s the receiving kernel's delivery leg has a
// median of 908 ns but rises to 762 us during a latency spike -- an 800-fold jump, and 36%
// of the spike's total excess. At 1M msg/s, where the receiver is saturated and therefore
// always inside recv, spikes carry no delivery excess at all: the leg is actually *below*
// its median during them.
//
// So: one thread does nothing but receive, and hands frames to a second thread over a
// single-producer single-consumer ring. The poller is never absent from recv for longer
// than a memcpy, so the loop cannot close.
//
// The costs are stated plainly, because they are real. One extra core per receiver. One
// extra copy of each datagram into the ring, which at these sizes is on the order of a
// hundred nanoseconds against a 34 us path. And a queue, which means a place for datagrams
// to sit invisibly -- so its depth and its high-water mark are both reported.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

namespace poll_split {

// One datagram in the handoff ring: its length, and the arrival stamps taken by the poller
// at the moment it came off the socket. Those stamps must be taken in the polling thread,
// not the consuming one, or the queueing this design introduces would be silently folded
// into the latency it is supposed to protect.
struct Slot {
  uint32_t len = 0;
  uint64_t arrival_ns = 0;
  uint64_t rx_delivery_ns = 0;
};

// A bounded single-producer single-consumer queue of datagrams, with the payloads held in
// one flat buffer so a slot costs no allocation.
//
// The two cursors are the only shared state, and each is written by exactly one side, so
// acquire/release on them is the whole synchronisation. There is no lock and no atomic
// read-modify-write on the hot path.
class Ring {
 public:
  Ring(uint32_t slots, uint32_t frame_bytes)
      : mask_(slots - 1), frame_bytes_(frame_bytes), meta_(slots),
        data_(static_cast<size_t>(slots) * frame_bytes) {
    // Power of two so the cursors can be masked rather than divided.
    // A caller passing something else is a programming error, not a runtime condition.
    if ((slots & (slots - 1)) != 0 || slots == 0) std::abort();
    // Touch it now: a page fault on the first burst would be indistinguishable from the
    // stall this class exists to prevent.
    std::memset(data_.data(), 0, data_.size());
  }

  uint32_t capacity() const { return mask_ + 1; }
  uint64_t high_water() const { return high_water_; }
  uint64_t dropped() const { return dropped_; }

  // Producer side. Returns false when the ring is full, which the caller must count rather
  // than retry: blocking here would stop the polling and reinstate the very feedback loop
  // this design removes.
  bool push(const uint8_t* payload, uint32_t len, uint64_t arrival_ns,
            uint64_t rx_delivery_ns) {
    const uint64_t head = head_.load(std::memory_order_relaxed);
    const uint64_t tail = tail_.load(std::memory_order_acquire);
    const uint64_t used = head - tail;
    if (used > high_water_) high_water_ = used;
    if (used > mask_) {
      ++dropped_;
      return false;
    }
    if (len > frame_bytes_) {
      ++dropped_;
      return false;
    }
    const uint32_t i = static_cast<uint32_t>(head) & mask_;
    std::memcpy(&data_[static_cast<size_t>(i) * frame_bytes_], payload, len);
    meta_[i].len = len;
    meta_[i].arrival_ns = arrival_ns;
    meta_[i].rx_delivery_ns = rx_delivery_ns;
    head_.store(head + 1, std::memory_order_release);
    return true;
  }

  // Consumer side. Returns a pointer into the ring's own storage, valid until release().
  const uint8_t* peek(Slot* out) {
    const uint64_t tail = tail_.load(std::memory_order_relaxed);
    if (head_.load(std::memory_order_acquire) == tail) return nullptr;
    const uint32_t i = static_cast<uint32_t>(tail) & mask_;
    *out = meta_[i];
    return &data_[static_cast<size_t>(i) * frame_bytes_];
  }

  void release() {
    tail_.store(tail_.load(std::memory_order_relaxed) + 1, std::memory_order_release);
  }

  bool empty() const {
    return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
  }

 private:
  // Kept on separate cache lines: the producer spins on head_ and reads tail_, the consumer
  // the other way round, and sharing a line would make every handoff a coherence miss.
  alignas(64) std::atomic<uint64_t> head_{0};
  alignas(64) std::atomic<uint64_t> tail_{0};
  alignas(64) uint32_t mask_;
  uint32_t frame_bytes_;
  std::vector<Slot> meta_;
  std::vector<uint8_t> data_;
  uint64_t high_water_ = 0;
  uint64_t dropped_ = 0;
};

}  // namespace poll_split
