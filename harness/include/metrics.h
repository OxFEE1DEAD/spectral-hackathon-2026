// Latency + delivery metrics.
//
// The collection path is deliberately allocation-free and syscall-free while a
// measurement is running. That is not a micro-optimisation: the consumer records
// from inside the same loop that timestamps arrivals, so anything that can block
// there -- a write(), a realloc, a page fault on a freshly grown buffer -- lands in
// the very tail we are trying to measure. Writing one CSV line per message used to
// cost about 1 ms at p99.99 on a disk-backed filesystem, and even on tmpfs a write
// can occasionally stall.
//
// So: capacity is reserved and *touched* up front, record() is a couple of stores,
// and samples reach the filesystem only after the run has finished.
#pragma once

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace metrics {

// Percentiles reported by default. p99.999 is included because it is where the
// interesting failures live, but it needs roughly a hundred thousand samples per
// observation -- see samples_for_percentile() and the write-up.
struct Report {
  uint64_t received = 0;
  uint64_t expected = 0;  // last_seq - first_seq + 1
  uint64_t dropped = 0;
  double drop_rate = 0.0;
  uint64_t overflow = 0;  // samples not stored because capacity ran out

  uint64_t lat_min = 0;
  uint64_t lat_max = 0;
  double lat_mean = 0.0;
  uint64_t p50 = 0, p90 = 0, p99 = 0, p999 = 0, p9999 = 0, p99999 = 0;
};

// How many samples a percentile needs before it means anything. p99.999 estimated
// from 100k samples rests on a single observation, which is not a measurement.
inline uint64_t samples_for_percentile(double p, uint64_t observations = 100) {
  return static_cast<uint64_t>(observations / (1.0 - p / 100.0));
}

class Accumulator {
 public:
  // capacity is a hard bound: nothing is allocated after construction, and samples
  // beyond it are counted rather than stored.
  explicit Accumulator(size_t capacity = 1u << 20) {
    lat_.resize(capacity);
    seq_.resize(capacity);
    // resize() value-initialises, which touches every page now rather than during
    // the run. Without this the first pass over the buffer would fault page by page
    // while we are timing.
    std::memset(lat_.data(), 0, lat_.size() * sizeof(uint32_t));
    std::memset(seq_.data(), 0, seq_.size() * sizeof(uint64_t));
  }

  // Hot path. Two stores and some arithmetic; no branches that allocate.
  void record(uint64_t seq_id, uint64_t latency_ns) {
    if (received_ == 0) {
      first_seq_ = seq_id;
      last_seq_ = seq_id;
    } else {
      if (seq_id < first_seq_) first_seq_ = seq_id;
      if (seq_id > last_seq_) last_seq_ = seq_id;
    }
    ++received_;
    sum_ += static_cast<double>(latency_ns);
    if (n_ < lat_.size()) {
      // uint32 spans 4.29 s, far beyond anything meaningful here; saturate rather
      // than wrap so a pathological sample cannot masquerade as a fast one.
      lat_[n_] = latency_ns > 0xffffffffull
                     ? 0xffffffffu
                     : static_cast<uint32_t>(latency_ns);
      seq_[n_] = seq_id;
      ++n_;
    } else {
      ++overflow_;
    }
  }

  size_t stored() const { return n_; }
  uint64_t overflow() const { return overflow_; }

  Report report() const {
    Report r;
    r.received = received_;
    r.overflow = overflow_;
    if (received_ == 0) return r;

    r.expected = last_seq_ - first_seq_ + 1;
    r.dropped = r.expected > r.received ? r.expected - r.received : 0;
    r.drop_rate = r.expected ? static_cast<double>(r.dropped) /
                                   static_cast<double>(r.expected)
                             : 0.0;
    r.lat_mean = sum_ / static_cast<double>(received_);

    std::vector<uint32_t> s(lat_.begin(), lat_.begin() + n_);
    std::sort(s.begin(), s.end());
    r.lat_min = s.front();
    r.lat_max = s.back();
    r.p50 = percentile(s, 50.0);
    r.p90 = percentile(s, 90.0);
    r.p99 = percentile(s, 99.0);
    r.p999 = percentile(s, 99.9);
    r.p9999 = percentile(s, 99.99);
    r.p99999 = percentile(s, 99.999);
    return r;
  }

  // Write every stored sample, once, after the run. Chunked through a fixed buffer
  // so the dump itself does not allocate a copy of the whole dataset.
  bool dump_csv(const std::string& path) const {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;
    std::string buf;
    buf.reserve(1u << 23);  // 8 MiB
    buf += "seq,latency_ns\n";
    char line[48];
    bool ok = true;
    for (size_t i = 0; i < n_ && ok; ++i) {
      const int len = std::snprintf(line, sizeof(line), "%llu,%u\n",
                                    (unsigned long long)seq_[i], lat_[i]);
      buf.append(line, static_cast<size_t>(len));
      if (buf.size() >= (1u << 23)) ok = flush(fd, buf);
    }
    if (ok && !buf.empty()) ok = flush(fd, buf);
    ::close(fd);
    return ok;
  }

  // Nearest-rank percentile, p in [0,100].
  static uint64_t percentile(const std::vector<uint32_t>& sorted, double p) {
    if (sorted.empty()) return 0;
    const size_t n = sorted.size();
    size_t rank = static_cast<size_t>(p / 100.0 * static_cast<double>(n));
    if (static_cast<double>(rank) < p / 100.0 * static_cast<double>(n)) ++rank;
    if (rank < 1) rank = 1;
    if (rank > n) rank = n;
    return sorted[rank - 1];
  }

 private:
  static bool flush(int fd, std::string& buf) {
    size_t off = 0;
    while (off < buf.size()) {
      const ssize_t w = ::write(fd, buf.data() + off, buf.size() - off);
      if (w <= 0) return false;
      off += static_cast<size_t>(w);
    }
    buf.clear();
    return true;
  }

  std::vector<uint32_t> lat_;
  std::vector<uint64_t> seq_;
  size_t n_ = 0;
  uint64_t overflow_ = 0;
  uint64_t received_ = 0;
  uint64_t first_seq_ = 0;
  uint64_t last_seq_ = 0;
  double sum_ = 0.0;
};

// Per-stage timings for one message, recorded by the receiver. Same preallocated,
// dump-at-the-end discipline as above.
//
//   shm_ns     producer timestamp -> sender about to hand it to the kernel
//              (how long the event sat in the source ring, plus sender work)
//   wire_ns    sender pre-send -> arrival at the receiver
//              (kernel TX, NIC, wire, NIC RX, kernel RX; spans two clocks)
//   publish_ns arrival -> published into the receiver's ring
class StageAccumulator {
 public:
  explicit StageAccumulator(size_t capacity = 0) {
    if (capacity == 0) return;
    seq_.resize(capacity);
    shm_.resize(capacity);
    wire_.resize(capacity);
    pub_.resize(capacity);
    rxdel_.resize(capacity);
    std::memset(seq_.data(), 0, seq_.size() * sizeof(uint64_t));
    std::memset(shm_.data(), 0, shm_.size() * sizeof(uint32_t));
    std::memset(wire_.data(), 0, wire_.size() * sizeof(uint32_t));
    std::memset(pub_.data(), 0, pub_.size() * sizeof(uint32_t));
    std::memset(rxdel_.data(), 0, rxdel_.size() * sizeof(uint32_t));
  }

  bool enabled() const { return !seq_.empty(); }
  size_t stored() const { return n_; }

  // rx_delivery_ns splits the wire leg at the receiver's own kernel: the gap between the
  // timestamp the kernel took when the packet entered its receive path and the moment our
  // recv returned. Both readings come from one clock on one host, so unlike the rest of
  // the wire leg this needs no cross-host synchronisation and is exact. Zero means the
  // caller did not ask for receive timestamping.
  void record(uint64_t seq, uint64_t shm_ns, uint64_t wire_ns, uint64_t publish_ns,
              uint64_t rx_delivery_ns = 0) {
    if (n_ >= seq_.size()) {
      ++overflow_;
      return;
    }
    seq_[n_] = seq;
    shm_[n_] = clamp32(shm_ns);
    wire_[n_] = clamp32(wire_ns);
    pub_[n_] = clamp32(publish_ns);
    rxdel_[n_] = clamp32(rx_delivery_ns);
    ++n_;
  }

  bool dump_csv(const std::string& path) const {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;
    std::string buf;
    buf.reserve(1u << 23);
    buf += "seq,shm_ns,wire_ns,publish_ns,rx_delivery_ns\n";
    char line[96];
    bool ok = true;
    for (size_t i = 0; i < n_ && ok; ++i) {
      const int len = std::snprintf(line, sizeof(line), "%llu,%u,%u,%u,%u\n",
                                    (unsigned long long)seq_[i], shm_[i],
                                    wire_[i], pub_[i], rxdel_[i]);
      buf.append(line, static_cast<size_t>(len));
      if (buf.size() >= (1u << 23)) ok = flush(fd, buf);
    }
    if (ok && !buf.empty()) ok = flush(fd, buf);
    ::close(fd);
    return ok;
  }

 private:
  static uint32_t clamp32(uint64_t v) {
    return v > 0xffffffffull ? 0xffffffffu : static_cast<uint32_t>(v);
  }
  static bool flush(int fd, std::string& buf) {
    size_t off = 0;
    while (off < buf.size()) {
      const ssize_t w = ::write(fd, buf.data() + off, buf.size() - off);
      if (w <= 0) return false;
      off += static_cast<size_t>(w);
    }
    buf.clear();
    return true;
  }

  std::vector<uint64_t> seq_;
  std::vector<uint32_t> shm_, wire_, pub_, rxdel_;
  size_t n_ = 0;
  uint64_t overflow_ = 0;
};

}  // namespace metrics
