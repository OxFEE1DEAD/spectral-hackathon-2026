// UDP plumbing: sockets, batched transmit to several destinations, batched
// receive, and the latency-relevant socket options.
//
// Everything here is allocation-free once opened. The mmsghdr/iovec arrays are
// sized at construction and reused, because a malloc in the hot loop is a page
// fault waiting to land in p99.99.
//
// Kernel feature detection is deliberate rather than incidental: Ubuntu 20.04
// (kernel 5.4) is on the judges' list, and SO_PREFER_BUSY_POLL only exists from
// 5.11. Anything optional is attempted and its failure tolerated, so the
// default build runs everywhere and simply gets less tuning on old kernels
// (plans/00_TRADEOFFS.txt D-08).
#pragma once

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "wire.h"

namespace net {

// Stable kernel ABI values; the headers on older distributions may not carry
// the names. setsockopt still returns ENOPROTOOPT there, which we tolerate.
#ifndef SO_BUSY_POLL
#define SO_BUSY_POLL 46
#endif
#ifndef SO_PREFER_BUSY_POLL
#define SO_PREFER_BUSY_POLL 69
#endif

struct Endpoint {
  sockaddr_in addr{};

  static bool parse(const std::string& host_port, Endpoint* out) {
    const size_t colon = host_port.rfind(':');
    if (colon == std::string::npos) return false;
    const std::string host = host_port.substr(0, colon);
    const int port = std::atoi(host_port.c_str() + colon + 1);
    if (port <= 0 || port > 65535) return false;

    std::memset(&out->addr, 0, sizeof(out->addr));
    out->addr.sin_family = AF_INET;
    out->addr.sin_port = htons(static_cast<uint16_t>(port));
    if (host.empty() || host == "*") {
      out->addr.sin_addr.s_addr = htonl(INADDR_ANY);
      return true;
    }
    return inet_pton(AF_INET, host.c_str(), &out->addr.sin_addr) == 1;
  }

  std::string str() const {
    char buf[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &addr.sin_addr, buf, sizeof(buf));
    return std::string(buf) + ":" + std::to_string(ntohs(addr.sin_port));
  }
  uint16_t port() const { return ntohs(addr.sin_port); }
};

// Applied to both directions. Returns how many optional knobs actually took, so
// the run bundle can record what the kernel gave us instead of what we asked
// for -- a number that differs between 5.4 and 6.8 and would otherwise silently
// explain a tail difference.
struct SockOpts {
  int send_buf = 8 << 20;
  int recv_buf = 8 << 20;
  int busy_poll_usec = 50;  // 0 disables
  bool prefer_busy_poll = true;
};

struct AppliedOpts {
  bool busy_poll = false;
  bool prefer_busy_poll = false;
  int send_buf = 0;
  int recv_buf = 0;
};

inline int open_udp() {
  const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) return -1;
  int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  return fd;
}

inline bool set_nonblocking(int fd) {
  const int fl = ::fcntl(fd, F_GETFL, 0);
  return fl >= 0 && ::fcntl(fd, F_SETFL, fl | O_NONBLOCK) == 0;
}

inline AppliedOpts apply_opts(int fd, const SockOpts& o) {
  AppliedOpts a;
  ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &o.send_buf, sizeof(o.send_buf));
  ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &o.recv_buf, sizeof(o.recv_buf));

  socklen_t len = sizeof(a.send_buf);
  ::getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &a.send_buf, &len);
  len = sizeof(a.recv_buf);
  ::getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &a.recv_buf, &len);

  // Busy polling asks the kernel to spin in the driver instead of sleeping and
  // waiting for the NAPI softirq. It is the cheapest way to take scheduler
  // wake-up latency out of the receive path, and unlike AF_XDP it costs nothing
  // in reproducibility (plans/11_OS_TUNING.txt).
  if (o.busy_poll_usec > 0) {
    const int v = o.busy_poll_usec;
    a.busy_poll = ::setsockopt(fd, SOL_SOCKET, SO_BUSY_POLL, &v, sizeof(v)) == 0;
    if (o.prefer_busy_poll) {
      const int one = 1;
      a.prefer_busy_poll =
          ::setsockopt(fd, SOL_SOCKET, SO_PREFER_BUSY_POLL, &one, sizeof(one)) == 0;
    }
  }
  return a;
}

inline bool bind_to(int fd, const Endpoint& ep) {
  return ::bind(fd, reinterpret_cast<const sockaddr*>(&ep.addr),
                sizeof(ep.addr)) == 0;
}

// One transmit stream: a socket with its own source port, fanning the same
// datagram out to every destination in a single sendmmsg.
//
// A stream exists so that redundant copies can leave from *different* source
// ports. The VPC fabric hashes the 5-tuple to pick a path and the receiving NIC
// hashes it to pick an RX queue, so two source ports decorrelate fabric
// queueing and receiver-side softirq stalls at once. Whether that decorrelation
// is real is the day-2 experiment (plans/03_PATH_DIVERSITY.txt); the plumbing
// is here either way because it costs nothing.
class TxStream {
 public:
  bool open(uint16_t src_port, const std::vector<Endpoint>& dests,
            const SockOpts& opts) {
    fd_ = open_udp();
    if (fd_ < 0) return false;
    if (src_port != 0) {
      Endpoint local;
      std::memset(&local.addr, 0, sizeof(local.addr));
      local.addr.sin_family = AF_INET;
      local.addr.sin_addr.s_addr = htonl(INADDR_ANY);
      local.addr.sin_port = htons(src_port);
      if (!bind_to(fd_, local)) {
        std::fprintf(stderr, "bind(src port %u): %s\n", src_port,
                     std::strerror(errno));
        return false;
      }
    }
    applied_ = apply_opts(fd_, opts);
    set_nonblocking(fd_);

    dests_ = dests;
    msgs_.resize(dests_.size());
    iovs_.resize(dests_.size());
    return true;
  }

  // Sends `len` bytes of `buf` to every destination. Returns the number of
  // datagrams the kernel accepted; a short count means the socket buffer is
  // full, which is a signal for the redundancy controller to back off rather
  // than something to retry into an already-congested queue.
  int send_all(const void* buf, uint32_t len) {
    const size_t n = dests_.size();
    for (size_t k = 0; k < n; ++k) {
      // Rotate so no receiver is systematically served last: with ~0.5-1 us of
      // in-kernel cost per destination, a fixed order would give the last one a
      // permanently worse tail for no reason (D-23).
      const size_t d = (k + rotate_) % n;
      iovs_[k].iov_base = const_cast<void*>(buf);
      iovs_[k].iov_len = len;
      std::memset(&msgs_[k], 0, sizeof(msgs_[k]));
      msgs_[k].msg_hdr.msg_name = &dests_[d].addr;
      msgs_[k].msg_hdr.msg_namelen = sizeof(dests_[d].addr);
      msgs_[k].msg_hdr.msg_iov = &iovs_[k];
      msgs_[k].msg_hdr.msg_iovlen = 1;
    }
    if (++rotate_ >= n) rotate_ = 0;

    const int sent = ::sendmmsg(fd_, msgs_.data(), static_cast<unsigned>(n), 0);
    if (sent < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS) {
        ++blocked_;
        return 0;
      }
      ++errors_;
      return -1;
    }
    if (static_cast<size_t>(sent) < n) ++partial_;
    sent_ += static_cast<uint64_t>(sent);
    return sent;
  }

  int fd() const { return fd_; }
  const AppliedOpts& applied() const { return applied_; }
  uint64_t sent() const { return sent_; }
  uint64_t blocked() const { return blocked_; }
  uint64_t partial() const { return partial_; }
  uint64_t errors() const { return errors_; }

  void close() {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
  }

 private:
  int fd_ = -1;
  AppliedOpts applied_{};
  std::vector<Endpoint> dests_;
  std::vector<mmsghdr> msgs_;
  std::vector<iovec> iovs_;
  size_t rotate_ = 0;
  uint64_t sent_ = 0, blocked_ = 0, partial_ = 0, errors_ = 0;
};

// Batched receive. One recvmmsg drains up to kVlen datagrams, which keeps the
// syscall count near the datagram count divided by the batch instead of equal
// to it, without adding any waiting: MSG_DONTWAIT means we take whatever is
// there right now and come straight back.
template <unsigned kVlen = 32>
class RxBatch {
 public:
  RxBatch() : storage_(static_cast<size_t>(kVlen) * wire::kMaxDatagram) {
    for (unsigned i = 0; i < kVlen; ++i) {
      iovs_[i].iov_base = storage_.data() + static_cast<size_t>(i) * wire::kMaxDatagram;
      iovs_[i].iov_len = wire::kMaxDatagram;
      std::memset(&msgs_[i], 0, sizeof(msgs_[i]));
      msgs_[i].msg_hdr.msg_name = &addrs_[i];
      msgs_[i].msg_hdr.msg_namelen = sizeof(addrs_[i]);
      msgs_[i].msg_hdr.msg_iov = &iovs_[i];
      msgs_[i].msg_hdr.msg_iovlen = 1;
    }
  }

  // >0 datagrams, 0 for "nothing right now", -1 on a real error.
  int recv(int fd) {
    for (unsigned i = 0; i < kVlen; ++i) {
      msgs_[i].msg_len = 0;
      msgs_[i].msg_hdr.msg_namelen = sizeof(addrs_[i]);
    }
    const int n = ::recvmmsg(fd, msgs_.data(), kVlen, MSG_DONTWAIT, nullptr);
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
      return -1;
    }
    return n;
  }

  const uint8_t* data(unsigned i) const {
    return storage_.data() + static_cast<size_t>(i) * wire::kMaxDatagram;
  }
  uint32_t len(unsigned i) const { return msgs_[i].msg_len; }
  const sockaddr_in& from(unsigned i) const { return addrs_[i]; }

 private:
  std::vector<uint8_t> storage_;
  std::array<mmsghdr, kVlen> msgs_{};
  std::array<iovec, kVlen> iovs_{};
  std::array<sockaddr_in, kVlen> addrs_{};
};

}  // namespace net
