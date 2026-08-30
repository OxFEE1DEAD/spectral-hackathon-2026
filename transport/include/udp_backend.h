// Tuned kernel-UDP backend for the data path.
//
// Concrete classes rather than an abstract interface with virtual calls: the send
// path runs once per datagram, and a future kernel-bypass backend can be a
// sibling type with the same signatures, chosen once at startup instead of
// through a vtable on every datagram.
//
// Fan-out to N receivers means putting the same datagram on N destinations, and
// there is more than one way to do that. Which is fastest is not obvious, so all
// three are implemented and measured rather than assumed:
//
//   kSendmmsg   one sendmmsg() carrying N destinations. One syscall total, but the
//               kernel still resolves each destination per message.
//   kSendto     N sendto() calls on one unconnected socket. N syscalls, and a
//               route lookup on every one of them.
//   kConnected  N sockets, each connect()ed once, then one send() each. N syscalls,
//               but destination and route are resolved at open time, not per send.
//
// The trade is syscall count against per-call work, and the measured answer is that
// per-call work wins: N cheap connected sends beat one sendmmsg carrying N
// destinations, because the sender completes each datagram sooner and therefore drains
// its source ring sooner. It also matters that latency is measured at *every*
// receiver, so the skew between first and last destination lands in the tail we are
// judged on -- and fewer syscalls did not help there either. See the write-up.
//
// The receive path has two shapes and the transport chooses between them itself,
// based on whether there is a NAPI-backed device to poll -- see pick_busy_poll()
// below for the measurements behind that decision.
#pragma once

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <linux/errqueue.h>
#include <linux/net_tstamp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "util.h"

namespace udp {

// Negative means "decide from the bound address" -- see pick_busy_poll().
inline constexpr int kBusyPollAuto = -1;
inline constexpr int kBusyPollDefaultUs = 50;

// How a datagram is replicated to N destinations. kAuto resolves to whichever the
// measurements favour; the explicit values exist to reproduce that comparison.
enum class SendMethod : uint8_t { kAuto, kSendmmsg, kSendto, kConnected };

inline const char* method_name(SendMethod m) {
  switch (m) {
    case SendMethod::kSendmmsg: return "sendmmsg";
    case SendMethod::kSendto: return "sendto";
    case SendMethod::kConnected: return "connected";
    default: return "auto";
  }
}

inline bool parse_method(const std::string& s, SendMethod* out) {
  if (s == "sendmmsg") { *out = SendMethod::kSendmmsg; return true; }
  if (s == "sendto") { *out = SendMethod::kSendto; return true; }
  if (s == "connected") { *out = SendMethod::kConnected; return true; }
  if (s == "auto") { *out = SendMethod::kAuto; return true; }
  return false;
}

struct Options {
  int sndbuf = 8 << 20;
  int rcvbuf = 8 << 20;
  // Ask the kernel to stamp each datagram just before it hands it to the driver, so the
  // sending kernel's protocol path can be split off the opaque wire leg. Like the receive
  // stamp, the interval it yields is two readings of one clock on one host, so it is exact
  // and needs no cross-host synchronisation. Measurement-only.
  bool tx_timestamp = false;
  // Serve peers in a fixed order, as the baseline does. Kept so the rotation below can be
  // measured against it in one binary rather than across two builds.
  bool fixed_order = false;
  // Ask the kernel to stamp each datagram as it enters the receive path, so the wire leg
  // can be split at the receiver's own kernel boundary. Measurement-only: it turns the
  // hot-path recv() into a recvmsg() with a control buffer to parse, which is exactly the
  // sort of thing that should not be on by default in the configuration being measured.
  bool rx_timestamp = false;
  // SO_BUSY_POLL microseconds, or kBusyPollAuto to choose automatically.
  int busy_poll_us = kBusyPollAuto;
  // Source address for the sending sockets. Empty means "let the route decide", which is
  // right on a directly-attached L2 path and wrong on anything selected by policy routing.
  //
  // On an L3 path the outgoing interface can be chosen by *source address* rather than by
  // destination: the host carries one routing table per uplink and an `ip rule` per local
  // address that selects it. A socket that does not bind its source therefore leaves by
  // whichever uplink the main table points at -- which is the default route, i.e. the
  // management interface, not the link under test. Measured on such a pair: bound to the
  // interface the peer was unreachable, bound to the source address the same peer answered
  // in 69.7 ms over the intended uplink.
  //
  // Port is left at zero so each socket still gets its own ephemeral source port, which is
  // what keeps the four-tuple diversity of the redundant leg intact.
  std::string src_addr;
};

// Busy-polling is worth it only when there is a NAPI-backed device to poll.
//
// On a real NIC it lets the receiving thread pull packets off the device queue on
// its own core instead of waiting for a softirq, which measured 1.7x better at the
// median and 3.4x at p99.9 cross-host. Bound to an address with no NAPI-backed
// device, sk_busy_loop() finds nothing to poll: the blocking recv() falls through to
// sleeping and we pay a scheduler wake-up per message instead of avoiding one, so the
// same option becomes a cost rather than a saving.
//
// So the transport picks per bound address rather than exposing a knob. Offering
// the operator two modes would mean shipping "these settings for the median, those
// for the tail", and the right choice here is determined by the path, not by which
// percentile someone cares about.
inline bool is_loopback(const sockaddr_in& a) {
  return (ntohl(a.sin_addr.s_addr) >> 24) == 127;
}

inline int pick_busy_poll(int configured, const sockaddr_in& bound) {
  if (configured != kBusyPollAuto) return configured;  // explicit, for experiments
  // A wildcard bind could receive on any device, so assume a real one.
  return is_loopback(bound) ? 0 : kBusyPollDefaultUs;
}

// Resolve "host" or "host:port" to an IPv4 address, using default_port when the
// string carries none.
inline bool resolve(const std::string& spec, uint16_t default_port,
                    sockaddr_in* out) {
  std::string host = spec;
  std::string port = std::to_string(default_port);
  const size_t colon = spec.rfind(':');
  if (colon != std::string::npos) {
    host = spec.substr(0, colon);
    port = spec.substr(colon + 1);
  }

  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;
  addrinfo* res = nullptr;
  const int rc = getaddrinfo(host.c_str(), port.c_str(), &hints, &res);
  if (rc != 0 || res == nullptr) {
    fprintf(stderr, "cannot resolve '%s': %s\n", spec.c_str(),
            gai_strerror(rc));
    return false;
  }
  std::memcpy(out, res->ai_addr, sizeof(sockaddr_in));
  freeaddrinfo(res);
  return true;
}

// Print an address without leaking which host it belongs to beyond what the
// operator already typed on the command line.
inline std::string describe(const sockaddr_in& a) {
  char ip[INET_ADDRSTRLEN] = {0};
  inet_ntop(AF_INET, &a.sin_addr, ip, sizeof(ip));
  return std::string(ip) + ":" + std::to_string(ntohs(a.sin_port));
}

inline void apply_common(int fd, const Options& o, bool sending) {
  // Bind the source before anything else: on a policy-routed host this is what decides
  // which uplink the datagrams leave by, and connect() latches the route.
  //
  // Applied here rather than in each backend's open() because the kernel-UDP and io_uring
  // senders both come through this function, so they cannot disagree about it.
  if (sending && !o.src_addr.empty()) {
    sockaddr_in src{};
    src.sin_family = AF_INET;
    src.sin_port = 0;  // ephemeral, so distinct sockets keep distinct four-tuples
    if (inet_pton(AF_INET, o.src_addr.c_str(), &src.sin_addr) != 1) {
      fprintf(stderr, "source address %s is not a valid IPv4 literal\n",
              o.src_addr.c_str());
    } else if (bind(fd, reinterpret_cast<sockaddr*>(&src), sizeof(src)) != 0) {
      // Loud rather than ignored: silently sending from the wrong address means measuring
      // a different network path than the one named on the command line.
      fprintf(stderr, "bind to source %s failed: %s\n", o.src_addr.c_str(),
              std::strerror(errno));
    }
  }
  const int buf = sending ? o.sndbuf : o.rcvbuf;
  const int name = sending ? SO_SNDBUF : SO_RCVBUF;
  if (setsockopt(fd, SOL_SOCKET, name, &buf, sizeof(buf)) != 0) {
    fprintf(stderr, "warning: SO_%sBUF=%d failed: %s\n",
            sending ? "SND" : "RCV", buf, std::strerror(errno));
  }
  // Ask the network for low delay over throughput where anything honours it.
  const int tos = IPTOS_LOWDELAY;
  setsockopt(fd, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));
}

class Sender {
 public:
  ~Sender() {
    for (int fd : fds_) {
      if (fd >= 0) close(fd);
    }
  }

  bool open(const std::vector<std::string>& peers, uint16_t default_port,
            const Options& opts, SendMethod method = SendMethod::kAuto) {
    if (peers.empty()) {
      fprintf(stderr, "no peers given\n");
      return false;
    }
    fixed_order_ = opts.fixed_order;
    addrs_.resize(peers.size());
    for (size_t i = 0; i < peers.size(); ++i) {
      if (!resolve(peers[i], default_port, &addrs_[i])) return false;
    }

    // kAuto = connected sockets, at every destination count.
    //
    // Measured at 10 destinations and 200k msg/s: p50 47.8 us against 52.3 (sendmmsg)
    // and 50.5 (sendto), with the lowest skew between destinations. sendto x 10 could
    // not sustain 1M msg/s at that fan-out at all.
    //
    // The per-stage split says why, and it is not what one would guess. The wire leg is
    // the same for all three within 5%; the whole difference is how long a message waits
    // in the source ring before the sender reaches it (5.8 us against 8.2). A cheaper
    // send call drains the ring faster, so less queues up. It is a throughput effect at
    // the sender, not anything about route lookup -- a route lookup is 100-200 ns and
    // could not account for a 4.6 us difference.
    //
    // Worth noting that the argument which predicts the opposite -- one syscall for N
    // destinations must beat N syscalls -- has now lost at both 3 and 10 destinations.
    // Whether it wins somewhere beyond that is untested.
    method_ = method == SendMethod::kAuto ? SendMethod::kConnected : method;

    const size_t sockets =
        method_ == SendMethod::kConnected ? addrs_.size() : 1;
    fds_.assign(sockets, -1);
    for (size_t i = 0; i < sockets; ++i) {
      fds_[i] = socket(AF_INET, SOCK_DGRAM, 0);
      if (fds_[i] < 0) {
        fprintf(stderr, "socket failed: %s\n", std::strerror(errno));
        return false;
      }
      apply_common(fds_[i], opts, /*sending=*/true);
    }

    if (method_ == SendMethod::kConnected) {
      for (size_t i = 0; i < addrs_.size(); ++i) {
        if (connect(fds_[i], reinterpret_cast<sockaddr*>(&addrs_[i]),
                    sizeof(addrs_[i])) != 0) {
          fprintf(stderr, "connect to %s failed: %s\n",
                  describe(addrs_[i]).c_str(), std::strerror(errno));
          return false;
        }
      }
    } else if (method_ == SendMethod::kSendmmsg) {
      // Pre-build the scatter/gather array once; only the length changes per send.
      iov_.resize(addrs_.size());
      msgs_.resize(addrs_.size());
      for (size_t i = 0; i < addrs_.size(); ++i) {
        std::memset(&msgs_[i], 0, sizeof(msgs_[i]));
        msgs_[i].msg_hdr.msg_name = &addrs_[i];
        msgs_[i].msg_hdr.msg_namelen = sizeof(addrs_[i]);
        msgs_[i].msg_hdr.msg_iov = &iov_[i];
        msgs_[i].msg_hdr.msg_iovlen = 1;
      }
    }
    return true;
  }

  size_t peer_count() const { return addrs_.size(); }
  const std::vector<sockaddr_in>& peers() const { return addrs_; }
  SendMethod method() const { return method_; }
  // Named the same as the io_uring backend's accessor so the relay loop can be written
  // once against either.
  const char* method_name() const { return udp::method_name(method_); }
  // No asynchronous completions on this path, so nothing can fail after the fact.
  uint64_t completion_errors() const { return 0; }

  // ---- transmit timestamping, measurement only ----------------------------------
  //
  // The kernel reports a transmit timestamp on the socket's error queue rather than
  // inline, so this is a three-part dance: enable it, remember when we handed each
  // datagram over, and later collect the stamps and difference them. What comes out is
  // the cost of the kernel's own transmit path -- protocol headers, route, qdisc -- up to
  // the point the driver takes the frame.
  //
  // SOF_TIMESTAMPING_OPT_ID makes the kernel label each datagram with a counter that
  // starts at zero when the option is enabled, so the label is exactly the index of our
  // send. OPT_TSONLY keeps the error-queue message down to the timestamp instead of
  // echoing the payload back at us.
  bool enable_tx_timestamps() {
    const int flags = SOF_TIMESTAMPING_TX_SOFTWARE | SOF_TIMESTAMPING_SOFTWARE |
                      SOF_TIMESTAMPING_OPT_ID | SOF_TIMESTAMPING_OPT_TSONLY;
    if (fds_.empty()) return false;
    if (setsockopt(fds_[0], SOL_SOCKET, SO_TIMESTAMPING, &flags, sizeof(flags)) != 0) {
      fprintf(stderr, "warning: SO_TIMESTAMPING(TX) not applied: %s\n",
              std::strerror(errno));
      return false;
    }
    presend_.assign(kTxIdRing, 0);
    tx_samples_.reserve(1u << 21);
    tx_timestamp_ = true;
    return true;
  }

  bool tx_timestamp() const { return tx_timestamp_; }

  // Remember when this datagram was handed to the kernel. Called once per send_all, so
  // the id the kernel will report matches the counter kept here.
  void note_send(uint64_t presend_ns) {
    if (!tx_timestamp_) return;
    presend_[tx_next_id_ & (kTxIdRing - 1)] = presend_ns;
    ++tx_next_id_;
  }

  // Collect whatever the kernel has posted. Called from the relay's idle branch, so it
  // never delays a real message -- the same rule the redundancy path follows.
  void drain_tx_stamps() {
    if (!tx_timestamp_) return;
    alignas(8) char control[512];
    for (int budget = 0; budget < 64; ++budget) {
      msghdr msg{};
      msg.msg_control = control;
      msg.msg_controllen = sizeof(control);
      const ssize_t n = ::recvmsg(fds_[0], &msg, MSG_ERRQUEUE | MSG_DONTWAIT);
      if (n < 0) return;
      uint64_t stamp = 0;
      uint32_t id = 0;
      bool have_stamp = false, have_id = false;
      for (cmsghdr* c = CMSG_FIRSTHDR(&msg); c != nullptr; c = CMSG_NXTHDR(&msg, c)) {
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SO_TIMESTAMPING) {
          timespec ts[3];
          std::memcpy(ts, CMSG_DATA(c), sizeof(ts));
          stamp = static_cast<uint64_t>(ts[0].tv_sec) * 1000000000ull +
                  static_cast<uint64_t>(ts[0].tv_nsec);
          have_stamp = stamp != 0;
        } else if (c->cmsg_level == SOL_IP && c->cmsg_type == IP_RECVERR) {
          sock_extended_err ee;
          std::memcpy(&ee, CMSG_DATA(c), sizeof(ee));
          if (ee.ee_origin == SO_EE_ORIGIN_TIMESTAMPING) {
            id = ee.ee_data;
            have_id = true;
          }
        }
      }
      if (!have_stamp || !have_id) {
        ++tx_unmatched_;
        continue;
      }
      // The ring only holds the most recent kTxIdRing sends; anything older than that
      // has been overwritten and is counted rather than guessed at.
      if (tx_next_id_ - id > kTxIdRing) {
        ++tx_expired_;
        continue;
      }
      const uint64_t sent = presend_[id & (kTxIdRing - 1)];
      if (sent == 0 || stamp <= sent) {
        ++tx_unmatched_;
        continue;
      }
      if (tx_samples_.size() < tx_samples_.capacity()) {
        tx_samples_.push_back(static_cast<uint32_t>(
            std::min<uint64_t>(stamp - sent, 0xffffffffull)));
      }
    }
  }

  const std::vector<uint32_t>& tx_stack_samples() const { return tx_samples_; }
  uint64_t tx_unmatched() const { return tx_unmatched_; }
  uint64_t tx_expired() const { return tx_expired_; }

  // Send one datagram to every peer. Returns how many peers it reached, so partial
  // fan-out is accounted for rather than silently lost.
  // Send one datagram to every peer, starting from a different peer each time. Returns how
  // many peers it reached, so partial fan-out is accounted for rather than silently lost.
  //
  // The rotation is the point. Copying a datagram to n peers is n sequential handoffs to the
  // kernel, so the peer served last waits for the n-1 before it -- and if the order never
  // changes, that wait is the same peer's every single datagram. The result is not jitter
  // that averages out across receivers; it is a fixed penalty attached to a fixed receiver,
  // and it grows with n. Advancing the starting index by one per datagram costs an add and a
  // compare, spreads the penalty evenly, and leaves every receiver with the same mean.
  int send_all(const void* buf, uint32_t len) {
    const size_t n_peers = addrs_.size();
    const size_t start = (n_peers && !fixed_order_) ? (rr_++ % n_peers) : 0;
    switch (method_) {
      case SendMethod::kConnected: {
        int reached = 0;
        for (size_t k = 0; k < fds_.size(); ++k) {
          const int fd = fds_[(start + k) % fds_.size()];
          if (::send(fd, buf, len, MSG_DONTWAIT) == static_cast<ssize_t>(len)) {
            ++reached;
          }
        }
        return reached;
      }
      case SendMethod::kSendto: {
        int reached = 0;
        for (size_t k = 0; k < n_peers; ++k) {
          const size_t i = (start + k) % n_peers;
          if (::sendto(fds_[0], buf, len, MSG_DONTWAIT,
                       reinterpret_cast<sockaddr*>(&addrs_[i]),
                       sizeof(addrs_[i])) == static_cast<ssize_t>(len)) {
            ++reached;
          }
        }
        return reached;
      }
      default: {
        // sendmmsg walks its array in order inside one syscall, so the same fixed penalty
        // applies; rotating which address each slot carries is what spreads it.
        for (size_t k = 0; k < msgs_.size(); ++k) {
          iov_[k].iov_base = const_cast<void*>(buf);
          iov_[k].iov_len = len;
          msgs_[k].msg_hdr.msg_name = &addrs_[(start + k) % n_peers];
        }
        const int n = sendmmsg(fds_[0], msgs_.data(),
                               static_cast<unsigned>(msgs_.size()), MSG_DONTWAIT);
        return n < 0 ? 0 : n;
      }
    }
  }

 private:
  // Power of two: the id the kernel reports is masked into this ring.
  static constexpr uint32_t kTxIdRing = 1u << 16;

  SendMethod method_ = SendMethod::kConnected;
  bool tx_timestamp_ = false;
  uint32_t tx_next_id_ = 0;
  size_t rr_ = 0;   // rotating start index for fan-out, see send_all
  bool fixed_order_ = false;
  std::vector<uint64_t> presend_;
  std::vector<uint32_t> tx_samples_;
  uint64_t tx_unmatched_ = 0;
  uint64_t tx_expired_ = 0;
  std::vector<int> fds_;
  std::vector<sockaddr_in> addrs_;
  std::vector<iovec> iov_;
  std::vector<mmsghdr> msgs_;
};

class Receiver {
 public:
  ~Receiver() {
    if (fd_ >= 0) close(fd_);
  }

  bool open(const std::string& bind_addr, uint16_t port, const Options& opts) {
    sockaddr_in local{};
    if (!resolve(bind_addr.empty() ? std::string("0.0.0.0") : bind_addr, port,
                 &local)) {
      return false;
    }

    // Two receive modes, and the difference is not a detail:
    //
    //  spin (busy_poll_us == 0): non-blocking socket, the caller spins on
    //    recv(MSG_DONTWAIT). Costs one syscall per poll. Packets only become
    //    visible once the NIC softirq has run, and on a host with isolated cores
    //    that softirq runs on a *housekeeping* core -- so if those cores are
    //    busy, delivery arrives in bursts however hard we spin here.
    //
    //  busy-poll (busy_poll_us > 0): blocking socket with SO_BUSY_POLL, so the
    //    kernel runs napi_busy_loop() on *our* core and pulls packets off the NIC
    //    queue itself, bypassing the softirq scheduling delay entirely.
    //
    // The second mode only works from the blocking receive path: SO_BUSY_POLL is
    // consulted by sk_busy_loop(), which MSG_DONTWAIT never reaches. A
    // non-blocking socket with SO_BUSY_POLL set silently does nothing.
    busy_poll_us_ = pick_busy_poll(opts.busy_poll_us, local);
    blocking_ = busy_poll_us_ > 0;

    fd_ = socket(AF_INET, SOCK_DGRAM | (blocking_ ? 0 : SOCK_NONBLOCK), 0);
    if (fd_ < 0) {
      fprintf(stderr, "socket failed: %s\n", std::strerror(errno));
      return false;
    }
    apply_common(fd_, opts, /*sending=*/false);

    if (blocking_) {
      const int us = busy_poll_us_;
      if (setsockopt(fd_, SOL_SOCKET, SO_BUSY_POLL, &us, sizeof(us)) != 0) {
        fprintf(stderr, "warning: SO_BUSY_POLL=%d not applied: %s\n", us,
                std::strerror(errno));
      }
      // Bound the block so a caller can still notice that the stream went quiet.
      timeval tv{};
      tv.tv_usec = 100000;  // 100 ms
      setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    rx_timestamp_ = opts.rx_timestamp;
    if (rx_timestamp_) {
      // RX_SOFTWARE is the stamp taken as the packet enters the receive path; SOFTWARE
      // is what asks for it to be reported. Software flags only -- hardware timestamping is
      // deliberately not requested here, because asking for it would only invite a
      // silently empty slot.
      const int flags = SOF_TIMESTAMPING_RX_SOFTWARE | SOF_TIMESTAMPING_SOFTWARE;
      if (setsockopt(fd_, SOL_SOCKET, SO_TIMESTAMPING, &flags, sizeof(flags)) != 0) {
        fprintf(stderr, "warning: SO_TIMESTAMPING not applied: %s\n",
                std::strerror(errno));
        rx_timestamp_ = false;
      }
    }
    if (bind(fd_, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
      fprintf(stderr, "bind %s failed: %s\n", describe(local).c_str(),
              std::strerror(errno));
      return false;
    }
    bound_ = local;
    return true;
  }

  const sockaddr_in& bound() const { return bound_; }

  // True when recv() blocks (busy-poll mode) rather than returning immediately.
  // Callers use this to decide how often it is worth checking the clock.
  bool blocking() const { return blocking_; }

  // Microseconds of SO_BUSY_POLL actually in effect; 0 means spin mode was chosen.
  int busy_poll_us() const { return busy_poll_us_; }

  // One datagram, or -1 when nothing arrived. In spin mode this returns
  // immediately and the caller polls; in busy-poll mode it blocks, with the
  // kernel polling the NIC on this core, and returns -1 only on timeout.
  int recv(void* buf, uint32_t cap) {
    const ssize_t n = ::recv(fd_, buf, cap, blocking_ ? 0 : MSG_DONTWAIT);
    return n < 0 ? -1 : static_cast<int>(n);
  }

  // Nanoseconds between the kernel stamping this datagram on entry to its receive path
  // and borrow() returning it to us. Valid only when Options::rx_timestamp was set and
  // the kernel actually attached a stamp; zero otherwise.
  uint64_t rx_delivery_ns() const { return rx_delivery_ns_; }
  uint64_t missing_rx_stamps() const { return missing_rx_stamps_; }

  // Borrow/release, matching the io_uring backend so one relay loop serves both.
  // Here the datagram is read into a buffer this object owns, which is the same
  // single copy the kernel would make into any caller-supplied buffer; the io_uring
  // side hands back kernel-filled memory directly. release() has nothing to do.
  //
  // Sized for jumbo frames so a sender configured for a larger datagram is truncated
  // visibly at the parser rather than silently misparsed.
  int borrow(const uint8_t** data) {
    if (buf_.empty()) buf_.resize(65536);
    const int n = rx_timestamp_ ? recv_stamped(buf_.data(),
                                              static_cast<uint32_t>(buf_.size()))
                                : recv(buf_.data(), static_cast<uint32_t>(buf_.size()));
    if (n >= 0) *data = buf_.data();
    return n;
  }

  // recvmsg() variant that also collects the kernel's receive timestamp. Kept separate
  // from recv() so the untimestamped path stays exactly as it was measured.
  int recv_stamped(void* buf, uint32_t cap) {
    iovec iov{buf, cap};
    alignas(8) char control[256];
    msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);
    const ssize_t n = ::recvmsg(fd_, &msg, blocking_ ? 0 : MSG_DONTWAIT);
    if (n < 0) return -1;
    // Read the clock only after the data is in hand, so the interval measured is
    // "kernel stamped it" to "we have it", with nothing of ours in between.
    const uint64_t now = util::now_ns();
    rx_delivery_ns_ = 0;
    for (cmsghdr* c = CMSG_FIRSTHDR(&msg); c != nullptr; c = CMSG_NXTHDR(&msg, c)) {
      if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SO_TIMESTAMPING) continue;
      // Three slots: software, deprecated hardware, raw hardware. Only software stamps are
      // requested, so only the software slot is ever populated.
      timespec ts[3];
      std::memcpy(ts, CMSG_DATA(c), sizeof(ts));
      const uint64_t stamp = static_cast<uint64_t>(ts[0].tv_sec) * 1000000000ull +
                             static_cast<uint64_t>(ts[0].tv_nsec);
      if (stamp != 0 && now > stamp) rx_delivery_ns_ = now - stamp;
      break;
    }
    if (rx_delivery_ns_ == 0) ++missing_rx_stamps_;
    return static_cast<int>(n);
  }

  void release() {}

  const char* mode_name() const { return busy_poll_us_ > 0 ? "busy-poll" : "spin"; }
  bool rx_timestamp() const { return rx_timestamp_; }

 private:
  int fd_ = -1;
  bool blocking_ = false;
  int busy_poll_us_ = 0;
  sockaddr_in bound_{};
  std::vector<uint8_t> buf_;
  bool rx_timestamp_ = false;
  uint64_t rx_delivery_ns_ = 0;
  uint64_t missing_rx_stamps_ = 0;
};

}  // namespace udp
