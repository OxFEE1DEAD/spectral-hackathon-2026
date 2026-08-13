// Out-of-band control channel between sender and receiver.
//
// Only session setup lives here, and only on TCP: it happens once, it must not
// be lost, and nobody is measuring its latency. Everything latency-critical or
// loss-tolerant -- NACKs, periodic telemetry -- goes over UDP instead. Putting
// NACKs on TCP would give them head-of-line blocking exactly when the network
// is lossy, which is precisely when a repair request must not queue behind
// anything (plans/07_REDUNDANCY.txt).
//
// The handshake exists to fail loudly on configuration skew. A sender speaking
// a different encoding, or a different number of redundancy streams, would
// otherwise produce a receiver that quietly drops everything and a benchmark
// that looks like 100% packet loss.
#pragma once

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace control {

inline constexpr uint32_t kMagic = 0x53504330;  // "SPC0"
inline constexpr uint16_t kVersion = 1;

#pragma pack(push, 1)
struct Hello {  // sender -> receiver
  uint32_t magic;
  uint16_t version;
  uint16_t n_streams;   // redundancy copies the sender will emit
  uint8_t encoding;     // wire::Encoding
  uint8_t reserved[3];
  uint64_t run_id;      // ties both sides' result bundles together
};

struct HelloAck {  // receiver -> sender
  uint32_t magic;
  uint16_t version;
  uint16_t status;  // 0 = accepted
  uint32_t slots;   // ring slots the receiver created for the consumer
  uint32_t reserved;
};
#pragma pack(pop)

enum Status : uint16_t {
  kOk = 0,
  kBadMagic = 1,
  kBadVersion = 2,
  kBadEncoding = 3,
};

inline const char* status_str(uint16_t s) {
  switch (s) {
    case kOk: return "ok";
    case kBadMagic: return "bad magic";
    case kBadVersion: return "version mismatch";
    case kBadEncoding: return "encoding mismatch";
    default: return "unknown";
  }
}

inline bool read_exact(int fd, void* buf, size_t n) {
  auto* p = static_cast<uint8_t*>(buf);
  while (n > 0) {
    const ssize_t r = ::read(fd, p, n);
    if (r == 0) return false;
    if (r < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    p += r;
    n -= static_cast<size_t>(r);
  }
  return true;
}

inline bool write_exact(int fd, const void* buf, size_t n) {
  const auto* p = static_cast<const uint8_t*>(buf);
  while (n > 0) {
    const ssize_t w = ::write(fd, p, n);
    if (w < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    p += w;
    n -= static_cast<size_t>(w);
  }
  return true;
}

// Receiver side: listen, accept one sender, validate its Hello.
class Listener {
 public:
  bool listen_on(uint16_t port) {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) return false;
    int one = 1;
    ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(port);
    if (::bind(fd_, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) return false;
    return ::listen(fd_, 4) == 0;
  }

  // Blocks until a sender arrives. Returns false on error.
  bool accept_hello(uint32_t slots, uint8_t expect_encoding, Hello* out) {
    conn_ = ::accept(fd_, nullptr, nullptr);
    if (conn_ < 0) return false;
    int one = 1;
    ::setsockopt(conn_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    Hello h{};
    if (!read_exact(conn_, &h, sizeof(h))) return false;

    HelloAck ack{};
    ack.magic = kMagic;
    ack.version = kVersion;
    ack.slots = slots;
    ack.status = kOk;
    if (h.magic != kMagic) ack.status = kBadMagic;
    else if (h.version != kVersion) ack.status = kBadVersion;
    else if (h.encoding != expect_encoding) ack.status = kBadEncoding;

    write_exact(conn_, &ack, sizeof(ack));
    if (ack.status != kOk) {
      std::fprintf(stderr, "control: rejecting sender: %s\n",
                   status_str(ack.status));
      return false;
    }
    *out = h;
    return true;
  }

  void close() {
    if (conn_ >= 0) ::close(conn_);
    if (fd_ >= 0) ::close(fd_);
    conn_ = fd_ = -1;
  }

 private:
  int fd_ = -1;
  int conn_ = -1;
};

// Sender side. Retries because the documented start order is receiver first,
// and a race there would otherwise be a confusing intermittent failure rather
// than a two-second wait.
class Client {
 public:
  bool connect_hello(const std::string& host, uint16_t port, const Hello& h,
                     HelloAck* ack, unsigned timeout_ms = 5000) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &a.sin_addr) != 1) return false;

    const unsigned step_ms = 50;
    for (unsigned waited = 0;; waited += step_ms) {
      fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
      if (fd_ < 0) return false;
      if (::connect(fd_, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0) break;
      ::close(fd_);
      fd_ = -1;
      if (waited >= timeout_ms) {
        std::fprintf(stderr, "control: no receiver at %s:%u after %u ms\n",
                     host.c_str(), port, timeout_ms);
        return false;
      }
      timespec req{0, static_cast<long>(step_ms) * 1000000L};
      while (nanosleep(&req, &req) != 0 && req.tv_nsec > 0) {
      }
    }

    int one = 1;
    ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    if (!write_exact(fd_, &h, sizeof(h))) return false;
    if (!read_exact(fd_, ack, sizeof(*ack))) return false;
    if (ack->magic != kMagic || ack->status != kOk) {
      std::fprintf(stderr, "control: receiver rejected us: %s\n",
                   status_str(ack->status));
      return false;
    }
    return true;
  }

  void close() {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
  }

 private:
  int fd_ = -1;
};

}  // namespace control
