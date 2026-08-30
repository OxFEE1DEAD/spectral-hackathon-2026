// io_uring backend for the data path: same sockets, different way of talking to them.
//
// The sockets here are ordinary UDP sockets, configured exactly as the kernel-UDP
// backend configures them. What changes is submission. Instead of one system call per
// destination, the relay writes N submission entries into memory shared with the kernel
// and hands them over in a single transition -- or, with a submission-queue poll thread,
// in no transition at all.
//
// Why this is worth trying at all: the measured cost of replicating one datagram to ten
// destinations is about 11.5 us, and the per-stage split attributes the difference
// between send methods entirely to how fast the sender drains its source ring rather
// than to anything on the wire. Send cost is therefore the lever, and io_uring is the
// cheapest available way to pull it -- no privileges, no dedicated NIC queue, no
// reserved hugepages, unlike the kernel-bypass options.
//
//   Sender, three shapes, in descending syscall count:
//     kernel UDP          N connected sockets, N send() calls        N syscalls
//     io_uring            N entries, one enter() that also reaps     1 syscall
//     io_uring + SQPOLL   N entries, tail store, reap by polling     0 syscalls
//
// All three keep the same synchronous contract: send_all() returns only once every
// destination's send has completed. That is not a detail. An asynchronous send would
// leave the kernel reading from the datagram buffer after the call returned, and the
// relay reuses its buffers immediately -- the duplication path in particular re-sends
// the previous datagram out of the buffer the next one is about to overwrite. Waiting
// for completions costs nothing here (a UDP send completes as soon as the payload is
// copied into an skb) and it keeps the accounting exact, so a partial fan-out is still
// reported as a partial fan-out rather than discovered later.
//
// Receiver, two shapes:
//     io_uring + NAPI     block in enter(GETEVENTS); the kernel polls the device
//                         queue on this core, the way SO_BUSY_POLL does
//     io_uring, polled    read the completion tail from shared memory; no syscall
//                         at all, but nothing drives NAPI, so delivery waits on a
//                         softirq that runs on a housekeeping core
//
// The same reasoning as in udp_backend.h applies to choosing between those: it is a
// property of the path, not a preference, so it is selected from the bound address
// rather than exposed as a tuning knob. See pick_poll_mode().
#pragma once

#include <sys/socket.h>
#include <sys/uio.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "iou_abi.h"
#include "udp_backend.h"

namespace iou {

// How the receive side waits for completions. kAuto resolves from the bound address.
enum class PollMode : uint8_t { kAuto, kNapi, kPolled };

inline constexpr int kNapiBusyPollDefaultUs = 50;

inline const char* poll_mode_name(PollMode m) {
  switch (m) {
    case PollMode::kNapi: return "napi";
    case PollMode::kPolled: return "polled";
    default: return "auto";
  }
}

inline bool parse_poll_mode(const std::string& s, PollMode* out) {
  if (s == "napi") { *out = PollMode::kNapi; return true; }
  if (s == "polled") { *out = PollMode::kPolled; return true; }
  if (s == "auto") { *out = PollMode::kAuto; return true; }
  return false;
}

struct Options {
  // The sockets are plain UDP sockets; reuse the same buffer and TOS settings so a
  // backend comparison is not quietly also a socket-configuration comparison.
  udp::Options sock;
  // Hand submission to a kernel thread, removing the last system call from the send
  // path. Costs a core, which is why it is off unless asked for.
  bool sqpoll = false;
  int sq_cpu = -1;  // pin that thread; -1 leaves placement to the scheduler
  // Registered NAPI busy polling, the io_uring counterpart of SO_BUSY_POLL.
  int napi_busy_poll_us = kNapiBusyPollDefaultUs;
  PollMode poll = PollMode::kAuto;
  // How many receive buffers to keep published. This is the receive path's entire
  // flow-control slack: the kernel takes one per datagram, we return one per datagram
  // processed, so the pool absorbs however far behind the relay falls. Running out
  // means the kernel has nowhere to put a packet and drops it, so it is sized for a
  // delivery burst several times longer than any observed stall rather than for the
  // steady state. 16384 buffers is about 170 ms of slack at the measured per-receiver
  // datagram rate, and costs 32 MB.
  uint32_t recv_buffers = 16384;
};

// Busy-polling only pays when there is a NAPI-backed device to poll; without one it
// costs a wake-up per message instead of saving one. Same decision as the UDP path, same
// reason -- see pick_busy_poll() there.
inline PollMode pick_poll_mode(PollMode configured, const sockaddr_in& bound) {
  if (configured != PollMode::kAuto) return configured;
  return udp::is_loopback(bound) ? PollMode::kPolled : PollMode::kNapi;
}

// ---------------------------------------------------------------------------
// The ring itself: setup, the two mmapped regions, and cursor bookkeeping.
// ---------------------------------------------------------------------------
class Ring {
 public:
  Ring() = default;
  Ring(const Ring&) = delete;
  Ring& operator=(const Ring&) = delete;

  ~Ring() {
    if (sqes_ != nullptr) munmap(sqes_, sqes_bytes_);
    if (sq_base_ != nullptr) munmap(sq_base_, sq_bytes_);
    if (cq_base_ != nullptr && cq_base_ != sq_base_) munmap(cq_base_, cq_bytes_);
    if (fd_ >= 0) close(fd_);
  }

  // `entries` is rounded up to a power of two by the kernel. `extra_flags` carries
  // SQPOLL and friends.
  bool open(uint32_t entries, uint32_t extra_flags, int sq_cpu) {
    abi::Params p{};
    p.flags = extra_flags;
    if (sq_cpu >= 0 && (extra_flags & abi::kSetupSqPoll) != 0) {
      p.flags |= abi::kSetupSqAff;
      p.sq_thread_cpu = static_cast<uint32_t>(sq_cpu);
      // The poll thread should not park, because waking it costs the very system call
      // SQPOLL exists to avoid. A minute of permitted idleness is far longer than any
      // gap in a market-data stream, while staying a sane millisecond count -- the
      // kernel converts this to jiffies, so an absurd value here is asking for an
      // overflow rather than a longer sleep.
      p.sq_thread_idle = 60000;
    }
    fd_ = abi::setup(entries, &p);
    if (fd_ < 0) {
      fprintf(stderr, "io_uring_setup(%u, flags=0x%x) failed: %s\n", entries, p.flags,
              std::strerror(errno));
      if (errno == ENOSYS) {
        fprintf(stderr, "  io_uring is not available on this kernel\n");
      } else if (errno == EPERM) {
        fprintf(stderr, "  io_uring may be restricted; check "
                        "/proc/sys/kernel/io_uring_disabled\n");
      } else if (errno == EINVAL && (p.flags & abi::kSetupSqAff) != 0) {
        // Overwhelmingly the likely cause, and indistinguishable from any other
        // EINVAL without saying so: the poll thread's core must exist and be online.
        fprintf(stderr, "  core %d for the submission poll thread must be online\n",
                sq_cpu);
      }
      return false;
    }
    features_ = p.features;
    sq_entries_ = p.sq_entries;
    cq_entries_ = p.cq_entries;

    // One mapping covers both rings on any kernel that reports SINGLE_MMAP, which is
    // every kernel that has the operations we need. The fallback is kept honest rather
    // than assumed away.
    sq_bytes_ = p.sq_off.array + p.sq_entries * sizeof(uint32_t);
    cq_bytes_ = p.cq_off.cqes + p.cq_entries * sizeof(abi::Cqe);
    if ((features_ & abi::kFeatSingleMmap) != 0) {
      if (cq_bytes_ > sq_bytes_) sq_bytes_ = cq_bytes_;
      cq_bytes_ = sq_bytes_;
    }
    sq_base_ = static_cast<uint8_t*>(mmap(nullptr, sq_bytes_, PROT_READ | PROT_WRITE,
                                          MAP_SHARED | MAP_POPULATE, fd_,
                                          static_cast<off_t>(abi::kOffSqRing)));
    if (sq_base_ == MAP_FAILED) {
      sq_base_ = nullptr;
      fprintf(stderr, "mmap sq ring failed: %s\n", std::strerror(errno));
      return false;
    }
    if ((features_ & abi::kFeatSingleMmap) != 0) {
      cq_base_ = sq_base_;
    } else {
      cq_base_ = static_cast<uint8_t*>(mmap(nullptr, cq_bytes_, PROT_READ | PROT_WRITE,
                                            MAP_SHARED | MAP_POPULATE, fd_,
                                            static_cast<off_t>(abi::kOffCqRing)));
      if (cq_base_ == MAP_FAILED) {
        cq_base_ = nullptr;
        fprintf(stderr, "mmap cq ring failed: %s\n", std::strerror(errno));
        return false;
      }
    }

    sqes_bytes_ = p.sq_entries * sizeof(abi::Sqe);
    sqes_ = static_cast<abi::Sqe*>(mmap(nullptr, sqes_bytes_, PROT_READ | PROT_WRITE,
                                        MAP_SHARED | MAP_POPULATE, fd_,
                                        static_cast<off_t>(abi::kOffSqes)));
    if (sqes_ == MAP_FAILED) {
      sqes_ = nullptr;
      fprintf(stderr, "mmap sqes failed: %s\n", std::strerror(errno));
      return false;
    }

    sq_head_ = reinterpret_cast<uint32_t*>(sq_base_ + p.sq_off.head);
    sq_tail_ = reinterpret_cast<uint32_t*>(sq_base_ + p.sq_off.tail);
    sq_mask_ = *reinterpret_cast<uint32_t*>(sq_base_ + p.sq_off.ring_mask);
    sq_flags_ = reinterpret_cast<uint32_t*>(sq_base_ + p.sq_off.flags);
    sq_array_ = reinterpret_cast<uint32_t*>(sq_base_ + p.sq_off.array);
    cq_head_ = reinterpret_cast<uint32_t*>(cq_base_ + p.cq_off.head);
    cq_tail_ = reinterpret_cast<uint32_t*>(cq_base_ + p.cq_off.tail);
    cq_mask_ = *reinterpret_cast<uint32_t*>(cq_base_ + p.cq_off.ring_mask);
    cqes_ = reinterpret_cast<abi::Cqe*>(cq_base_ + p.cq_off.cqes);

    // The indirection array never changes for us: submission slot i always refers to
    // entry i. Filling it once at startup keeps it off the hot path entirely.
    for (uint32_t i = 0; i <= sq_mask_; ++i) sq_array_[i] = i;

    tail_cache_ = abi::load_relaxed(sq_tail_);
    return true;
  }

  int fd() const { return fd_; }
  uint32_t features() const { return features_; }
  uint32_t sq_entries() const { return sq_entries_; }
  uint32_t cq_entries() const { return cq_entries_; }

  // Refuse to run unless the kernel implements what we are about to submit, so a
  // wrong opcode is a startup error rather than silent misbehaviour.
  bool require_ops(const uint8_t* ops, size_t n) {
    abi::ProbeBuf probe;
    if (!abi::probe_ops(fd_, &probe)) {
      fprintf(stderr, "io_uring: REGISTER_PROBE unavailable, cannot verify opcodes\n");
      return false;
    }
    for (size_t i = 0; i < n; ++i) {
      if (!abi::op_supported(probe, ops[i])) {
        fprintf(stderr, "io_uring: kernel does not support opcode %u "
                        "(implements up to %u)\n",
                ops[i], probe.hdr.last_op);
        return false;
      }
    }
    return true;
  }

  // Next submission entry, or nullptr when the ring is full. Entries are claimed from
  // a cached tail and only become visible to the kernel in publish().
  abi::Sqe* get_sqe() {
    const uint32_t head = abi::load_acquire(sq_head_);
    if (tail_cache_ - head > sq_mask_) return nullptr;
    abi::Sqe* e = &sqes_[tail_cache_ & sq_mask_];
    ++tail_cache_;
    *e = abi::Sqe{};
    return e;
  }

  // Make every claimed entry visible. The release store is what orders the entry
  // contents ahead of the tail the kernel reads.
  void publish() { abi::store_release(sq_tail_, tail_cache_); }

  // True when an SQPOLL thread has parked and needs a nudge to look again.
  bool needs_wakeup() const {
    return (abi::load_acquire(sq_flags_) & abi::kSqNeedWakeup) != 0;
  }

  int enter(uint32_t to_submit, uint32_t min_complete, uint32_t flags) {
    return abi::enter(fd_, to_submit, min_complete, flags);
  }

  // Completions ready to read without entering the kernel.
  uint32_t cq_ready() const {
    return abi::load_acquire(cq_tail_) - abi::load_relaxed(cq_head_);
  }

  const abi::Cqe& cqe_at(uint32_t i) const {
    return cqes_[(abi::load_relaxed(cq_head_) + i) & cq_mask_];
  }

  // Hand `n` completion slots back to the kernel.
  void advance_cq(uint32_t n) {
    abi::store_release(cq_head_, abi::load_relaxed(cq_head_) + n);
  }

 private:
  int fd_ = -1;
  uint32_t features_ = 0;
  uint32_t sq_entries_ = 0;
  uint32_t cq_entries_ = 0;
  uint8_t* sq_base_ = nullptr;
  uint8_t* cq_base_ = nullptr;
  size_t sq_bytes_ = 0;
  size_t cq_bytes_ = 0;
  abi::Sqe* sqes_ = nullptr;
  size_t sqes_bytes_ = 0;
  uint32_t* sq_head_ = nullptr;
  uint32_t* sq_tail_ = nullptr;
  uint32_t* sq_flags_ = nullptr;
  uint32_t* sq_array_ = nullptr;
  uint32_t sq_mask_ = 0;
  uint32_t* cq_head_ = nullptr;
  uint32_t* cq_tail_ = nullptr;
  abi::Cqe* cqes_ = nullptr;
  uint32_t cq_mask_ = 0;
  uint32_t tail_cache_ = 0;
};

// ---------------------------------------------------------------------------
// Sender: N connected sockets, one submission batch per datagram.
// ---------------------------------------------------------------------------
class Sender {
 public:
  ~Sender() {
    for (int fd : fds_) {
      if (fd >= 0) close(fd);
    }
  }

  bool open(const std::vector<std::string>& peers, uint16_t default_port,
            const Options& opts) {
    if (peers.empty()) {
      fprintf(stderr, "no peers given\n");
      return false;
    }
    addrs_.resize(peers.size());
    for (size_t i = 0; i < peers.size(); ++i) {
      if (!udp::resolve(peers[i], default_port, &addrs_[i])) return false;
    }

    // Connected sockets, because that is what the end-to-end comparison favours and
    // because a connected send needs no address in the submission entry.
    fds_.assign(addrs_.size(), -1);
    for (size_t i = 0; i < addrs_.size(); ++i) {
      fds_[i] = socket(AF_INET, SOCK_DGRAM, 0);
      if (fds_[i] < 0) {
        fprintf(stderr, "socket failed: %s\n", std::strerror(errno));
        return false;
      }
      udp::apply_common(fds_[i], opts.sock, /*sending=*/true);
      if (connect(fds_[i], reinterpret_cast<sockaddr*>(&addrs_[i]),
                  sizeof(addrs_[i])) != 0) {
        fprintf(stderr, "connect to %s failed: %s\n",
                udp::describe(addrs_[i]).c_str(), std::strerror(errno));
        return false;
      }
    }

    // Room for one full fan-out with slack, so get_sqe() never fails mid-batch.
    uint32_t entries = 8;
    while (entries < addrs_.size() * 2) entries <<= 1;
    uint32_t flags = 0;
    if (opts.sqpoll) flags |= abi::kSetupSqPoll;
    if (!ring_.open(entries, flags, opts.sq_cpu)) return false;

    const uint8_t need[] = {abi::kOpSend};
    if (!ring_.require_ops(need, 1)) return false;

    // Registering the sockets lets each submission name a small index instead of a
    // descriptor, so the kernel skips the file-table lookup on every send.
    if (abi::register_(ring_.fd(), abi::kRegFiles, fds_.data(),
                       static_cast<uint32_t>(fds_.size())) == 0) {
      fixed_files_ = true;
    } else {
      fprintf(stderr, "warning: REGISTER_FILES failed (%s), using raw descriptors\n",
              std::strerror(errno));
    }

    sqpoll_ = opts.sqpoll;
    return true;
  }

  size_t peer_count() const { return addrs_.size(); }
  const std::vector<sockaddr_in>& peers() const { return addrs_; }
  const char* method_name() const { return sqpoll_ ? "iouring-sqpoll" : "iouring"; }
  uint64_t completion_errors() const { return completion_errors_; }
  int last_error() const { return last_error_; }

  // Submit one datagram to every peer and wait for all of them to complete. Returns
  // the number of destinations the datagram actually reached, matching the kernel-UDP
  // backend's contract exactly.
  int send_all(const void* buf, uint32_t len) {
    const uint32_t n = static_cast<uint32_t>(fds_.size());
    for (uint32_t i = 0; i < n; ++i) {
      abi::Sqe* e = ring_.get_sqe();
      if (e == nullptr) {
        // Cannot happen with the ring sized above, but losing datagrams silently
        // because of an arithmetic slip is not an acceptable failure mode.
        fprintf(stderr, "io_uring: submission ring full at %u/%u\n", i, n);
        return static_cast<int>(i);
      }
      e->opcode = abi::kOpSend;
      e->fd = fixed_files_ ? static_cast<int32_t>(i) : fds_[i];
      e->flags = fixed_files_ ? abi::kSqeFixedFile : 0;
      e->addr = reinterpret_cast<uint64_t>(buf);
      e->len = len;
      e->op_flags = MSG_DONTWAIT;  // drop rather than block, as the UDP path does
      e->user_data = i;
    }
    ring_.publish();

    if (sqpoll_) {
      // The poll thread picks the entries up from shared memory. The only reason to
      // enter the kernel is if it parked, which on an isolated core it should not.
      if (ring_.needs_wakeup()) {
        ring_.enter(0, 0, abi::kEnterSqWakeup);
      }
      return reap(n, /*may_enter=*/false);
    }
    // One transition submits all N and collects all N completions.
    const int rc = ring_.enter(n, n, abi::kEnterGetevents);
    if (rc < 0 && errno != EINTR && errno != EBUSY) {
      last_error_ = errno;
      return 0;
    }
    return reap(n, /*may_enter=*/true);
  }

 private:
  // Collect exactly `n` completions, counting how many carried a full-length send.
  int reap(uint32_t n, bool may_enter) {
    uint32_t seen = 0;
    int reached = 0;
    while (seen < n) {
      const uint32_t ready = ring_.cq_ready();
      if (ready == 0) {
        if (may_enter) {
          if (ring_.enter(0, n - seen, abi::kEnterGetevents) < 0 && errno != EINTR &&
              errno != EBUSY) {
            last_error_ = errno;
            break;
          }
        }
        // In SQPOLL mode this spins, which is the point: no syscall at all.
        continue;
      }
      const uint32_t take = ready < (n - seen) ? ready : (n - seen);
      for (uint32_t i = 0; i < take; ++i) {
        const abi::Cqe& c = ring_.cqe_at(i);
        if (c.res >= 0) {
          ++reached;
        } else {
          ++completion_errors_;
          last_error_ = -c.res;
        }
      }
      ring_.advance_cq(take);
      seen += take;
    }
    return reached;
  }

  Ring ring_;
  std::vector<int> fds_;
  std::vector<sockaddr_in> addrs_;
  bool fixed_files_ = false;
  bool sqpoll_ = false;
  uint64_t completion_errors_ = 0;
  int last_error_ = 0;
};

// ---------------------------------------------------------------------------
// Receiver: one multishot receive, a ring of provided buffers, and completions
// that arrive without any further submission.
// ---------------------------------------------------------------------------
class Receiver {
 public:
  ~Receiver() {
    if (buf_ring_ != nullptr) munmap(buf_ring_, buf_ring_bytes_);
    if (fd_ >= 0) close(fd_);
  }

  bool open(const std::string& bind_addr, uint16_t port, const Options& opts) {
    sockaddr_in local{};
    if (!udp::resolve(bind_addr.empty() ? std::string("0.0.0.0") : bind_addr, port,
                      &local)) {
      return false;
    }
    poll_ = pick_poll_mode(opts.poll, local);
    napi_us_ = opts.napi_busy_poll_us;

    fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) {
      fprintf(stderr, "socket failed: %s\n", std::strerror(errno));
      return false;
    }
    udp::apply_common(fd_, opts.sock, /*sending=*/false);
    if (bind(fd_, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
      fprintf(stderr, "bind %s failed: %s\n", udp::describe(local).c_str(),
              std::strerror(errno));
      return false;
    }
    bound_ = local;

    // The completion queue has to absorb a burst without dropping, because a dropped
    // completion is a lost datagram we would never hear about. Sized to the buffer
    // count so every outstanding buffer can complete before we recycle any.
    // A power of two, because the buffer ring is indexed with a mask.
    buffers_ = 8;
    while (buffers_ < opts.recv_buffers) buffers_ <<= 1;
    // The buffer id has to fit the 16 bits the completion carries it in.
    if (buffers_ > 65536) buffers_ = 65536;
    uint32_t entries = buffers_;
    const uint32_t flags = abi::kSetupSingleIssuer;
    if (!ring_.open(entries, flags, -1)) return false;

    const uint8_t need[] = {abi::kOpRecv};
    if (!ring_.require_ops(need, 1)) return false;

    if (!setup_buffers()) return false;

    if (poll_ == PollMode::kNapi && napi_us_ > 0) {
      abi::Napi napi{};
      napi.busy_poll_to = static_cast<uint32_t>(napi_us_);
      napi.prefer_busy_poll = 1;
      // nr_args is 1 here, not 0: the kernel validates it before dispatch, so passing
      // 0 fails with EINVAL and looks exactly like an unsupported opcode.
      if (abi::register_(ring_.fd(), abi::kRegNapi, &napi, 1) != 0) {
        fprintf(stderr,
                "warning: REGISTER_NAPI failed (%s); receive will wait without "
                "polling the device queue on this core\n",
                std::strerror(errno));
        napi_registered_ = false;
      } else {
        napi_registered_ = true;
      }
    }

    return arm();
  }

  const sockaddr_in& bound() const { return bound_; }
  PollMode poll_mode() const { return poll_; }
  bool napi_registered() const { return napi_registered_; }
  uint64_t rearms() const { return rearms_; }
  uint64_t no_buffer_events() const { return no_buffer_events_; }

  // Named to match the kernel-UDP backend so the relay loop is written once.
  //
  // In NAPI mode borrow() waits inside the kernel, which is what drives the device
  // polling; in polled mode it returns immediately and the caller spins. The relay uses
  // this the same way it does for the UDP path: to decide how often checking the clock
  // is worth the nanoseconds.
  bool blocking() const { return poll_ == PollMode::kNapi; }

  const char* mode_name() const {
    if (poll_ != PollMode::kNapi) return "polled";
    return napi_registered_ ? "napi" : "napi-unavailable";
  }

  // Borrow the next datagram without copying it. Returns the length, or -1 when
  // nothing arrived. The pointer stays valid until release() is called, which is why
  // the caller must finish with it before asking for another.
  int borrow(const uint8_t** data) {
    for (;;) {
      if (pending_ == 0) {
        // Re-arm only once the backlog is drained. Every completion we still have to
        // process is holding a buffer that release() has not returned yet, so arming
        // before then would restart the receive with the pool at its emptiest --
        // which is exactly how one starvation event turns into a run of them.
        if (needs_rearm_) {
          drain_consumed();
          if (!arm()) return -1;
        }
        pending_ = ring_.cq_ready();
        if (pending_ == 0) {
          if (poll_ == PollMode::kNapi) {
            // Blocking here is what lets the kernel busy-poll the device queue on
            // this core. Bounded so a caller can still notice a quiet stream.
            const int rc = ring_.enter(0, 1, abi::kEnterGetevents);
            if (rc < 0 && errno != EINTR && errno != ETIME && errno != EAGAIN) {
              return -1;
            }
            pending_ = ring_.cq_ready();
          }
          if (pending_ == 0) return -1;
        }
        cursor_ = 0;
      }

      const abi::Cqe& c = ring_.cqe_at(cursor_);
      const int32_t res = c.res;
      const uint32_t cflags = c.flags;
      const bool more = (cflags & abi::kCqeFMore) != 0;
      ++cursor_;
      --pending_;
      consumed_this_round_ = cursor_;

      if (!more) {
        // Multishot stopped. It has to be armed again or the socket goes permanently
        // deaf, which would read as total loss rather than as latency. The re-arm
        // happens at the top of the loop once the backlog is drained.
        needs_rearm_ = true;
      }
      if (res < 0) {
        // ENOBUFS means the kernel had no buffer for a packet, so a datagram was
        // dropped here rather than on the wire. Counted separately because it is our
        // fault, not the network's, and the two call for opposite responses.
        if (res == -ENOBUFS) ++no_buffer_events_;
        drain_consumed();
        continue;
      }
      if ((cflags & abi::kCqeFBuffer) == 0) {
        // Should not happen with buffer select, but returning garbage would be worse.
        drain_consumed();
        continue;
      }
      last_bid_ = static_cast<uint16_t>(cflags >> abi::kCqeBufferShift);
      *data = buffer_at(last_bid_);
      have_borrow_ = true;
      return res;
    }
  }

  // Hand the borrowed buffer back to the kernel and settle the completion slot.
  // Re-arming, if needed, waits until the backlog is drained -- see borrow().
  void release() {
    if (!have_borrow_) return;
    have_borrow_ = false;
    publish_buffer(last_bid_);
    drain_consumed();
  }

 private:
  static constexpr uint32_t kBufSize = 2048;
  static constexpr uint16_t kBufGroup = 1;

  uint8_t* buffer_at(uint16_t bid) { return buf_data_ + static_cast<size_t>(bid) * kBufSize; }

  bool setup_buffers() {
    // The buffer ring and the buffers themselves in one mapping: the ring is an array
    // of descriptors, the payload area follows it.
    buf_ring_bytes_ = static_cast<size_t>(buffers_) * sizeof(abi::Buf) +
                      static_cast<size_t>(buffers_) * kBufSize;
    void* m = mmap(nullptr, buf_ring_bytes_, PROT_READ | PROT_WRITE,
                   MAP_ANONYMOUS | MAP_PRIVATE | MAP_POPULATE, -1, 0);
    if (m == MAP_FAILED) {
      fprintf(stderr, "mmap buffer ring failed: %s\n", std::strerror(errno));
      buf_ring_bytes_ = 0;
      return false;
    }
    buf_ring_ = static_cast<abi::Buf*>(m);
    buf_data_ = reinterpret_cast<uint8_t*>(buf_ring_) +
                static_cast<size_t>(buffers_) * sizeof(abi::Buf);
    std::memset(buf_ring_, 0, static_cast<size_t>(buffers_) * sizeof(abi::Buf));

    abi::BufReg reg{};
    reg.ring_addr = reinterpret_cast<uint64_t>(buf_ring_);
    reg.ring_entries = buffers_;
    reg.bgid = kBufGroup;
    if (abi::register_(ring_.fd(), abi::kRegPbufRing, &reg, 1) != 0) {
      fprintf(stderr, "REGISTER_PBUF_RING failed: %s\n", std::strerror(errno));
      return false;
    }

    // Publish every buffer up front. Writing addr/len/bid leaves the overlaid tail
    // field alone, so the release store below is what makes them all visible at once.
    for (uint32_t i = 0; i < buffers_; ++i) {
      buf_ring_[i].addr = reinterpret_cast<uint64_t>(buffer_at(static_cast<uint16_t>(i)));
      buf_ring_[i].len = kBufSize;
      buf_ring_[i].bid = static_cast<uint16_t>(i);
    }
    buf_tail_ = static_cast<uint16_t>(buffers_);
    abi::store_release(&tail_field(), buf_tail_);
    return true;
  }

  uint16_t& tail_field() {
    return reinterpret_cast<abi::BufRing*>(buf_ring_)->tail;
  }

  void publish_buffer(uint16_t bid) {
    const uint32_t slot = buf_tail_ & (buffers_ - 1);
    buf_ring_[slot].addr = reinterpret_cast<uint64_t>(buffer_at(bid));
    buf_ring_[slot].len = kBufSize;
    buf_ring_[slot].bid = bid;
    ++buf_tail_;
    abi::store_release(&tail_field(), buf_tail_);
  }

  void drain_consumed() {
    if (consumed_this_round_ == 0) return;
    ring_.advance_cq(consumed_this_round_);
    consumed_this_round_ = 0;
    cursor_ = 0;
  }

  // One multishot receive covers every datagram until the kernel says otherwise.
  bool arm() {
    abi::Sqe* e = ring_.get_sqe();
    if (e == nullptr) {
      fprintf(stderr, "io_uring: no submission slot to arm receive\n");
      return false;
    }
    e->opcode = abi::kOpRecv;
    e->fd = fd_;
    e->addr = 0;
    e->len = 0;  // length comes from the provided buffer
    e->ioprio = abi::kRecvMultishot;
    e->flags = abi::kSqeBufferSelect;
    e->buf_index = kBufGroup;
    e->user_data = kRecvUserData;
    ring_.publish();
    if (ring_.enter(1, 0, 0) < 0 && errno != EBUSY) {
      fprintf(stderr, "io_uring: arming receive failed: %s\n", std::strerror(errno));
      return false;
    }
    needs_rearm_ = false;
    ++rearms_;
    return true;
  }

  static constexpr uint64_t kRecvUserData = 1;

  Ring ring_;
  int fd_ = -1;
  sockaddr_in bound_{};
  PollMode poll_ = PollMode::kNapi;
  int napi_us_ = kNapiBusyPollDefaultUs;
  bool napi_registered_ = false;

  abi::Buf* buf_ring_ = nullptr;
  uint8_t* buf_data_ = nullptr;
  size_t buf_ring_bytes_ = 0;
  uint32_t buffers_ = 0;
  uint16_t buf_tail_ = 0;

  uint32_t pending_ = 0;
  uint32_t cursor_ = 0;
  uint32_t consumed_this_round_ = 0;
  uint16_t last_bid_ = 0;
  bool have_borrow_ = false;
  bool needs_rearm_ = false;
  uint64_t rearms_ = 0;
  uint64_t no_buffer_events_ = 0;
};

}  // namespace iou
