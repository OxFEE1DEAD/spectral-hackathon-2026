// Measure one-way latency without trusting the two hosts' clocks, and measure the offset
// between those clocks as a by-product.
//
// Why this exists. Every cross-host figure in the write-up is computed as
//
//     wire leg = (receiver's stamp, receiver's clock) - (sender's stamp, sender's clock)
//
// which is the true transit time *plus* whatever offset separates the two clocks. The same
// configuration measured 29,920, 32,836 and 36,014 ns in three sessions -- a 6 us spread --
// and there was no way to tell whether the path had changed or the clocks had drifted.
// Chrony reports a couple of hundred nanoseconds of dispersion, but that is an estimate of
// its own jitter, not a bound on the systematic offset between two machines.
//
// The trick is that a round trip is measured entirely on one clock and a turnaround
// entirely on the other, so neither quantity depends on the two agreeing:
//
//     initiator: t0 = now                      (initiator clock)
//     reflector: t1 = now on arrival           (reflector clock)
//     reflector: t2 = now just before replying (reflector clock)
//     initiator: t3 = now on the reply         (initiator clock)
//
//     round trip    rtt    = t3 - t0        one clock, exact
//     turnaround    turn   = t2 - t1        one clock, exact
//     one way       ow     = (rtt - turn) / 2
//     clock offset  offset = (t1 - t0) - ow
//
// The one-way figure assumes the path is symmetric, which is the usual assumption and the
// reason both directions are also measured separately. `offset` is what the relay's wire
// leg has to be corrected by, and it is the number this tool exists to produce.
//
// Deliberately a ping-pong with one datagram outstanding, so it measures the path rather
// than queueing behind our own traffic. Run it alongside a load to get the loaded figure.
//
// Usage:
//   clock_probe --reflect [--bind ADDR] [--port P] [--core N] [--idle-s S]
//   clock_probe --probe --peer HOST:PORT [--src ADDR] ...
//   clock_probe --probe --peer HOST[:PORT] [--count N] [--rate R] [--core N] [--csv PATH]
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "cpu.h"
#include "util.h"

namespace {

// Everything the arithmetic needs travels in the datagram, so no state has to be matched
// up afterwards and a lost reply simply drops out of the sample.
struct Probe {
  uint64_t seq;
  uint64_t t0;  // initiator, before send
  uint64_t t1;  // reflector, on arrival
  uint64_t t2;  // reflector, before reply
};

static_assert(sizeof(Probe) == 32, "probe payload must be 32 bytes");

// Stop at a loop boundary and still write the samples out. Without this a harness that
// tears the run down kills the probe before it has written anything, and the offset series
// -- the entire point of the tool -- is lost.
volatile std::sig_atomic_t g_stop = 0;

void on_signal(int) { g_stop = 1; }

struct Config {
  bool reflect = false;
  bool probe = false;
  std::string bind_addr = "0.0.0.0";
  // Probe mode only: the source address to send from. On a host whose uplink is selected by
  // an `ip rule` per local address, an unbound probe leaves by the default route and would
  // measure the offset over the management path instead of the link under test.
  std::string src_addr;
  std::string peer;
  uint16_t port = 51900;
  int core = -1;
  uint64_t count = 200000;
  uint32_t rate = 5000;  // ping-pongs per second
  uint32_t idle_s = 30;  // reflector exits after this long with no traffic
  std::string csv;
};

Config parse_args(int argc, char** argv) {
  Config c;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) {
        fprintf(stderr, "missing value for %s\n", a.c_str());
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--reflect") c.reflect = true;
    else if (a == "--probe") c.probe = true;
    else if (a == "--bind") c.bind_addr = next();
    else if (a == "--src") c.src_addr = next();
    else if (a == "--peer") c.peer = next();
    else if (a == "--port") c.port = static_cast<uint16_t>(std::stoul(next()));
    else if (a == "--core") c.core = std::stoi(next());
    else if (a == "--count") c.count = std::stoull(next());
    else if (a == "--rate") c.rate = static_cast<uint32_t>(std::stoul(next()));
    else if (a == "--idle-s") c.idle_s = static_cast<uint32_t>(std::stoul(next()));
    else if (a == "--csv") c.csv = next();
    else {
      fprintf(stderr, "unknown arg: %s\n", a.c_str());
      std::exit(2);
    }
  }
  if (c.reflect == c.probe) {
    fprintf(stderr, "exactly one of --reflect or --probe is required\n");
    std::exit(2);
  }
  if (c.probe && c.peer.empty()) {
    fprintf(stderr, "--probe requires --peer HOST[:PORT]\n");
    std::exit(2);
  }
  return c;
}

bool resolve(const std::string& spec, uint16_t default_port, sockaddr_in* out) {
  std::string host = spec, port = std::to_string(default_port);
  const size_t colon = spec.rfind(':');
  if (colon != std::string::npos) {
    host = spec.substr(0, colon);
    port = spec.substr(colon + 1);
  }
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;
  addrinfo* res = nullptr;
  if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0 || res == nullptr) {
    fprintf(stderr, "cannot resolve '%s'\n", spec.c_str());
    return false;
  }
  std::memcpy(out, res->ai_addr, sizeof(sockaddr_in));
  freeaddrinfo(res);
  return true;
}

// Busy-poll on both ends, for the same reason the relay's receiver does. Here it matters
// doubly: the reflector's turnaround is subtracted from the round trip, so a scheduler
// wake-up in it lands directly in the one-way figure.
int open_socket(const sockaddr_in* bind_to, int busy_poll_us) {
  const int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    fprintf(stderr, "socket: %s\n", std::strerror(errno));
    return -1;
  }
  const int tos = IPTOS_LOWDELAY;
  setsockopt(fd, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));
  const int buf = 4 << 20;
  setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf, sizeof(buf));
  setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf, sizeof(buf));
  if (busy_poll_us > 0) {
    setsockopt(fd, SOL_SOCKET, SO_BUSY_POLL, &busy_poll_us, sizeof(busy_poll_us));
    timeval tv{};
    tv.tv_usec = 200000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  }
  if (bind_to != nullptr &&
      bind(fd, reinterpret_cast<const sockaddr*>(bind_to), sizeof(*bind_to)) != 0) {
    fprintf(stderr, "bind: %s\n", std::strerror(errno));
    close(fd);
    return -1;
  }
  return fd;
}

int64_t pct(std::vector<int64_t>& v, double q) {
  if (v.empty()) return 0;
  const size_t i = static_cast<size_t>(q * static_cast<double>(v.size() - 1));
  return v[i];
}

void report(const char* name, std::vector<int64_t> v, const char* unit = "ns") {
  if (v.empty()) {
    printf("  %-22s (no samples)\n", name);
    return;
  }
  std::sort(v.begin(), v.end());
  printf("  %-22s p50=%9lld  p99=%9lld  p99.9=%10lld  min=%9lld  max=%11lld  %s\n",
         name, (long long)pct(v, 0.50), (long long)pct(v, 0.99),
         (long long)pct(v, 0.999), (long long)v.front(), (long long)v.back(), unit);
}

int run_reflector(const Config& cfg) {
  sockaddr_in local{};
  if (!resolve(cfg.bind_addr, cfg.port, &local)) return 1;
  const int fd = open_socket(&local, 50);
  if (fd < 0) return 1;
  char addr[INET_ADDRSTRLEN] = {0};
  inet_ntop(AF_INET, &local.sin_addr, addr, sizeof(addr));
  fprintf(stderr, "reflector: %s:%u, busy-poll, exits after %us idle\n", addr,
          ntohs(local.sin_port), cfg.idle_s);

  uint64_t reflected = 0;
  uint64_t last = util::now_ns();
  std::vector<int64_t> turn;
  turn.reserve(1u << 21);
  Probe p{};
  while (!g_stop) {
    sockaddr_in from{};
    socklen_t flen = sizeof(from);
    const ssize_t n = recvfrom(fd, &p, sizeof(p), 0,
                               reinterpret_cast<sockaddr*>(&from), &flen);
    if (n < 0) {
      if (util::now_ns() - last > static_cast<uint64_t>(cfg.idle_s) * 1000000000ull) break;
      continue;
    }
    const uint64_t t1 = util::now_ns();
    if (n < static_cast<ssize_t>(sizeof(Probe))) continue;
    p.t1 = t1;
    p.t2 = util::now_ns();
    sendto(fd, &p, sizeof(p), MSG_DONTWAIT, reinterpret_cast<sockaddr*>(&from), flen);
    ++reflected;
    last = util::now_ns();
    if (turn.size() < turn.capacity()) turn.push_back(static_cast<int64_t>(p.t2 - p.t1));
  }
  fprintf(stderr, "reflector: %llu reflected\n", (unsigned long long)reflected);
  printf("reflector turnaround (its own clock, exact):\n");
  report("turnaround", turn);
  close(fd);
  return 0;
}

int run_probe(const Config& cfg) {
  sockaddr_in dst{};
  if (!resolve(cfg.peer, cfg.port, &dst)) return 1;
  sockaddr_in src{};
  const sockaddr_in* src_ptr = nullptr;
  if (!cfg.src_addr.empty()) {
    if (!resolve(cfg.src_addr, 0, &src)) return 1;
    src_ptr = &src;
  }
  const int fd = open_socket(src_ptr, 50);
  if (fd < 0) return 1;
  if (connect(fd, reinterpret_cast<sockaddr*>(&dst), sizeof(dst)) != 0) {
    fprintf(stderr, "connect: %s\n", std::strerror(errno));
    return 1;
  }
  printf("probe: %llu ping-pongs to %s at %u/s\n", (unsigned long long)cfg.count,
         cfg.peer.c_str(), cfg.rate);

  std::vector<int64_t> rtt, turn, oneway, offset;
  rtt.reserve(cfg.count); turn.reserve(cfg.count);
  oneway.reserve(cfg.count); offset.reserve(cfg.count);
  const uint64_t period = cfg.rate ? 1000000000ull / cfg.rate : 0;
  uint64_t next = util::now_ns();
  uint64_t lost = 0;
  uint64_t consecutive_lost = 0;

  for (uint64_t i = 0; i < cfg.count && !g_stop; ++i) {
    if (period) {
      next += period;
      while (util::now_ns() < next) { /* pace */ }
    }
    Probe p{};
    p.seq = i;
    p.t0 = util::now_ns();
    if (send(fd, &p, sizeof(p), 0) != static_cast<ssize_t>(sizeof(p))) {
      ++lost;
      continue;
    }
    Probe r{};
    const ssize_t n = recv(fd, &r, sizeof(r), 0);
    const uint64_t t3 = util::now_ns();
    if (n != static_cast<ssize_t>(sizeof(r)) || r.seq != i) {
      ++lost;
      // Give up rather than grind on. With no reflector answering, every receive costs a
      // full socket timeout and the run takes hours instead of seconds -- which, under a
      // harness that retries on timeout, spawns copy after copy of this process.
      if (++consecutive_lost >= 200) {
        fprintf(stderr, "probe: %llu consecutive unanswered; is the reflector up?\n",
                (unsigned long long)consecutive_lost);
        break;
      }
      continue;
    }
    consecutive_lost = 0;
    const int64_t rt = static_cast<int64_t>(t3 - r.t0);
    const int64_t tn = static_cast<int64_t>(r.t2 - r.t1);
    const int64_t ow = (rt - tn) / 2;
    // t1 is on the reflector's clock, t0 on ours; subtracting the transit leaves the
    // offset between the two clocks.
    const int64_t off = static_cast<int64_t>(r.t1 - r.t0) - ow;
    rtt.push_back(rt);
    turn.push_back(tn);
    oneway.push_back(ow);
    offset.push_back(off);
  }

  printf("\nsamples=%zu lost=%llu\n", rtt.size(), (unsigned long long)lost);
  report("round trip", rtt);
  report("reflector turnaround", turn);
  report("one way (rtt-turn)/2", oneway);
  report("clock offset", offset);
  if (!offset.empty()) {
    std::vector<int64_t> o = offset;
    std::sort(o.begin(), o.end());
    const int64_t med = pct(o, 0.50);
    printf("\n  Correction for the relay's wire leg: subtract %lld ns.\n", (long long)med);
    printf("  A wire leg reported as W is really W - %lld.\n", (long long)med);
  }

  if (!cfg.csv.empty()) {
    FILE* f = fopen(cfg.csv.c_str(), "w");
    if (f != nullptr) {
      fprintf(f, "seq,rtt_ns,turnaround_ns,oneway_ns,offset_ns\n");
      for (size_t i = 0; i < rtt.size(); ++i) {
        fprintf(f, "%zu,%lld,%lld,%lld,%lld\n", i, (long long)rtt[i],
                (long long)turn[i], (long long)oneway[i], (long long)offset[i]);
      }
      fclose(f);
      fprintf(stderr, "wrote %s\n", cfg.csv.c_str());
    }
  }
  close(fd);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  Config cfg = parse_args(argc, argv);
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
  if (cfg.core >= 0 && !cpu::pin_to_core(cfg.core)) return 1;
  return cfg.reflect ? run_reflector(cfg) : run_probe(cfg);
}
