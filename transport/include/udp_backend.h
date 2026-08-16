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
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

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
  // SO_BUSY_POLL microseconds, or kBusyPollAuto to choose automatically.
  int busy_poll_us = kBusyPollAuto;
};

// Busy-polling is worth it only when there is a NAPI-backed device to poll.
//
// On a real NIC it lets the receiving thread pull packets off the device queue on
// its own core instead of waiting for a softirq, which measurably tightens the
// tail. On loopback there is no NAPI instance at all: sk_busy_loop() finds nothing
// to poll, the blocking recv() falls through to sleeping, and we pay a scheduler
// wake-up on every message -- measured at 2.5x the median and 34x the p99.99
// versus a plain non-blocking spin.
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

  // Send one datagram to every peer. Returns how many peers it reached, so partial
  // fan-out is accounted for rather than silently lost.
  int send_all(const void* buf, uint32_t len) {
    switch (method_) {
      case SendMethod::kConnected: {
        int reached = 0;
        for (int fd : fds_) {
          if (::send(fd, buf, len, MSG_DONTWAIT) == static_cast<ssize_t>(len)) {
            ++reached;
          }
        }
        return reached;
      }
      case SendMethod::kSendto: {
        int reached = 0;
        for (size_t i = 0; i < addrs_.size(); ++i) {
          if (::sendto(fds_[0], buf, len, MSG_DONTWAIT,
                       reinterpret_cast<sockaddr*>(&addrs_[i]),
                       sizeof(addrs_[i])) == static_cast<ssize_t>(len)) {
            ++reached;
          }
        }
        return reached;
      }
      default: {
        for (size_t i = 0; i < iov_.size(); ++i) {
          iov_[i].iov_base = const_cast<void*>(buf);
          iov_[i].iov_len = len;
        }
        const int n = sendmmsg(fds_[0], msgs_.data(),
                               static_cast<unsigned>(msgs_.size()), MSG_DONTWAIT);
        return n < 0 ? 0 : n;
      }
    }
  }

 private:
  SendMethod method_ = SendMethod::kConnected;
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

 private:
  int fd_ = -1;
  bool blocking_ = false;
  int busy_poll_us_ = 0;
  sockaddr_in bound_{};
};

}  // namespace udp
