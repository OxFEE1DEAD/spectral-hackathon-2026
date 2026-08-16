// Sender: reads events from the producer's shared-memory ring and relays them
// over UDP to one or more receivers.
//
// This is the first half of the transport under test. It is a pure relay: it does
// not interpret market data and does not touch the framing header the consumer
// measures from (seq_id, send_ts_ns travel through byte-identical).
//
// Two mechanisms share one principle: use spare capacity, never delay a message.
//
// Batching is opportunistic. The loop reads everything the ring already has ready,
// packs it into one datagram, and sends immediately. It never waits for a datagram
// to fill, so no message is ever held back in the hope of company. At a low rate a
// datagram carries one message; under load it fills up by itself.
//
// Duplication is opportunistic in exactly the same way. Having sent a datagram, the
// loop looks for more work; if the ring is empty it sends a second copy of what it
// just sent, then goes back to looking. So the redundant copy only ever occupies
// time the sender had nothing better to do with.
//
// That makes redundancy load-adaptive with no knob and no policy to tune:
//
//   * At low and moderate rates there is idle time after every datagram, so
//     effectively everything is sent twice and independent loss p becomes p^2.
//   * As the rate rises the idle gaps shrink and the duplication ratio falls off on
//     its own.
//   * Near saturation there is no idle time at all, so duplication stops entirely
//     rather than doubling the packet rate exactly when the path can least afford
//     it -- which is precisely when duplication would otherwise *add* latency.
//
// The alternative, sending both copies back to back unconditionally, doubles the
// packet rate at every load and would push a saturated path into queueing. The
// reported duplication ratio makes the trade-off visible instead of implicit.
//
// Usage: sender --peer HOST[:PORT] [--peer HOST[:PORT] ...]
//               [--shm NAME] [--slots N] [--port P] [--datagram BYTES]
//               [--core N] [--from-start] [--count N] [--idle-ms MS]
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "cpu.h"
#include "metrics.h"
#include "shm_ring.h"
#include "shm_segment.h"
#include "udp_backend.h"
#include "util.h"
#include "wire.h"

namespace {

// Set from a signal handler so the relay can stop at a loop boundary and still run
// its reporting. Without this, a SIGTERM from the harness kills the process before
// it writes out the samples it spent the whole run collecting.
volatile std::sig_atomic_t g_stop = 0;

void on_signal(int) { g_stop = 1; }

struct Config {
  std::string shm_name = "/fanout_ring";
  uint32_t slots = 1024;
  std::vector<std::string> peers;
  uint16_t port = 51000;
  uint32_t datagram = wire::kDefaultDatagram;
  int core = -1;
  // A relay starts at the producer's live edge: replaying a backlog that was
  // published before we attached would report the age of old messages as
  // transport latency. --from-start is there for deterministic tests.
  bool from_edge = true;
  uint64_t count = 0;    // frames to relay, 0 = unlimited
  uint64_t idle_ms = 0;  // exit after this long with nothing to send, 0 = never
  // Send a redundant copy of each datagram when the loop would otherwise idle.
  // On by default: it is free whenever there is spare capacity and self-limiting
  // when there is not. --no-duplicate exists to measure what it buys.
  bool duplicate = true;
  // Report how long each frame took to get from the producer's timestamp to just
  // before we hand it to the kernel. Both timestamps come from this host's clock,
  // so this splits the end-to-end figure into "before the wire" and "the rest"
  // with no cross-host clock question. Costs one clock read per datagram, so it
  // is opt-in rather than always on.
  bool trace = false;
  // How the datagram is replicated to N receivers. Auto unless a comparison run
  // pins it explicitly.
  udp::SendMethod method = udp::SendMethod::kAuto;
};

struct Stats {
  uint64_t frames = 0;
  uint64_t datagrams = 0;      // distinct datagrams, not counting copies
  uint64_t duplicates = 0;     // redundant copies actually sent
  uint64_t dup_skipped = 0;    // copies dropped because new data arrived first
  uint64_t lapped = 0;         // we fell behind the producer's ring
  uint64_t send_fail = 0;      // datagrams that reached no peer at all
  uint64_t partial = 0;        // datagrams that reached some but not all peers
};

bool is_power_of_two(uint32_t x) { return x != 0 && (x & (x - 1)) == 0; }

Config parse_args(int argc, char** argv) {
  Config c;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) {
        fprintf(stderr, "missing value for %s\n", a.c_str());
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--shm") c.shm_name = next();
    else if (a == "--slots") c.slots = static_cast<uint32_t>(std::stoul(next()));
    else if (a == "--peer") c.peers.push_back(next());
    else if (a == "--port") c.port = static_cast<uint16_t>(std::stoul(next()));
    else if (a == "--datagram") c.datagram = static_cast<uint32_t>(std::stoul(next()));
    else if (a == "--core") c.core = std::stoi(next());
    else if (a == "--from-start") c.from_edge = false;
    else if (a == "--count") c.count = std::stoull(next());
    else if (a == "--idle-ms") c.idle_ms = std::stoull(next());
    else if (a == "--trace") c.trace = true;
    else if (a == "--no-duplicate") c.duplicate = false;
    else if (a == "--send-method") {
      const std::string m = next();
      if (!udp::parse_method(m, &c.method)) {
        fprintf(stderr, "--send-method must be auto|sendmmsg|sendto|connected\n");
        std::exit(2);
      }
    }
    else {
      fprintf(stderr, "unknown arg: %s\n", a.c_str());
      std::exit(2);
    }
  }
  if (c.peers.empty()) {
    fprintf(stderr, "at least one --peer HOST[:PORT] is required\n");
    std::exit(2);
  }
  if (!is_power_of_two(c.slots)) {
    fprintf(stderr, "--slots must be a power of two (got %u)\n", c.slots);
    std::exit(2);
  }
  // Below this a largest-size message could not be sent at all, which would
  // stall the relay on the first order book rather than fail visibly here.
  if (c.datagram < wire::kMinDatagram) {
    fprintf(stderr, "--datagram must be >= %u to fit one largest frame\n",
            wire::kMinDatagram);
    std::exit(2);
  }
  return c;
}

void print_stats(const Stats& s) {
  fprintf(stderr, "---- sender ----\n");
  fprintf(stderr, "frames relayed  : %llu\n", (unsigned long long)s.frames);
  fprintf(stderr, "datagrams sent  : %llu\n", (unsigned long long)s.datagrams);
  if (s.datagrams) {
    fprintf(stderr, "frames/datagram : %.2f\n",
            static_cast<double>(s.frames) / static_cast<double>(s.datagrams));
  }
  fprintf(stderr, "duplicates sent : %llu\n", (unsigned long long)s.duplicates);
  if (s.datagrams) {
    // The headline number for the redundancy policy: 100% means every datagram
    // went twice, 0% means the path was saturated and redundancy stood down.
    fprintf(stderr, "duplication     : %.1f%% (skipped %llu, no idle time)\n",
            100.0 * static_cast<double>(s.duplicates) /
                static_cast<double>(s.datagrams),
            (unsigned long long)s.dup_skipped);
  }
  fprintf(stderr, "source lapped   : %llu\n", (unsigned long long)s.lapped);
  fprintf(stderr, "send failed     : %llu\n", (unsigned long long)s.send_fail);
  fprintf(stderr, "send partial    : %llu\n", (unsigned long long)s.partial);
}

}  // namespace

int main(int argc, char** argv) {
  Config cfg = parse_args(argc, argv);
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  if (cfg.core >= 0 && !cpu::pin_to_core(cfg.core)) return 1;

  udp::Options opts;
  udp::Sender net;
  if (!net.open(cfg.peers, cfg.port, opts, cfg.method)) return 1;

  shm::Segment seg = shm::Segment::open(cfg.shm_name,
                                        shm::region_size(cfg.slots),
                                        /*create=*/false);
  shm::Ring ring;
  ring.attach(seg.base(), cfg.slots, /*init=*/false);

  fprintf(stderr,
          "sender: shm=%s slots=%u peers=%zu datagram=%u core=%d send=%s\n",
          cfg.shm_name.c_str(), cfg.slots, net.peer_count(), cfg.datagram,
          cfg.core, udp::method_name(net.method()));
  for (const sockaddr_in& p : net.peers()) {
    fprintf(stderr, "sender: -> %s\n", udp::describe(p).c_str());
  }

  // Two datagram buffers used alternately. While one is being filled, the other
  // still holds the datagram we sent last, so the redundant copy can be re-sent
  // straight out of it -- no copy, no rebuild, nothing added to the hot path.
  std::vector<uint8_t> pktbuf[2] = {std::vector<uint8_t>(cfg.datagram),
                                    std::vector<uint8_t>(cfg.datagram)};
  wire::Packer packers[2] = {{pktbuf[0].data(), cfg.datagram},
                             {pktbuf[1].data(), cfg.datagram}};
  int cur = 0;
  // The previously sent datagram, still awaiting its redundant copy.
  uint8_t* dup_buf = nullptr;
  uint32_t dup_len = 0;

  alignas(64) uint8_t scratch[shm::kFrameCap];
  // A frame read from the ring that did not fit in the datagram we were building.
  // It is already consumed from the ring, so it has to lead the next datagram.
  bool carry = false;
  uint32_t carry_len = 0;

  uint64_t read_index = cfg.from_edge ? ring.live_edge() : 0;
  uint64_t pkt_seq = 0;
  Stats stats;
  metrics::Accumulator trace_acc(cfg.trace ? (cfg.count ? cfg.count : 1u << 20)
                                           : 0);
  const uint64_t idle_ns = cfg.idle_ms * 1000000ull;
  uint64_t last_activity = util::now_ns();

  while (!g_stop && (cfg.count == 0 || stats.frames < cfg.count)) {
    wire::Packer& packer = packers[cur];
    packer.reset(pkt_seq + 1);
    if (carry) {
      packer.add(scratch, carry_len);
      carry = false;
    }

    // Drain what is already published; stop at the first empty slot.
    while (cfg.count == 0 || stats.frames + packer.count() < cfg.count) {
      uint32_t len = 0;
      uint64_t resume = 0;
      const auto st = ring.read(read_index, scratch, &len, &resume);
      if (st == shm::Ring::FrameStatus::kEmpty) break;
      if (st == shm::Ring::FrameStatus::kLapped) {
        // We were too slow and the producer overwrote what we had not read yet.
        // Skipping forward is the only option; the gap shows up as a seq_id gap
        // at the consumer, which is exactly how it should be accounted for.
        ++stats.lapped;
        read_index = resume;
        continue;
      }
      ++read_index;
      if (!packer.has_room(len)) {
        carry = true;
        carry_len = len;
        break;
      }
      packer.add(scratch, len);
    }

    if (packer.empty()) {
      // The ring had nothing for us. This is the only place the loop is allowed to
      // spend time, precisely because no message is waiting on us.
      //
      // Spend it on redundancy: re-send the previous datagram. Because we only get
      // here when there was no new data, the copy never delays a real message, and
      // it goes out microseconds after the original -- early enough that if the
      // original was lost, the copy still arrives before any later message exists
      // and can therefore pass the receiver's monotonic gate.
      if (dup_buf != nullptr) {
        wire::mark_duplicate(dup_buf);
        wire::stamp_send_ts(dup_buf, util::now_ns());
        net.send_all(dup_buf, dup_len);
        ++stats.duplicates;
        dup_buf = nullptr;
      }
      if (idle_ns && util::now_ns() - last_activity > idle_ns) break;
      continue;
    }

    // New data arrived before we found idle time for the copy. Redundancy loses to
    // fresh data every time; this counter is how the write-up shows that happening.
    if (dup_buf != nullptr) {
      ++stats.dup_skipped;
      dup_buf = nullptr;
    }

    uint32_t dlen = 0;
    const uint8_t* dgram = packer.finish(&dlen);

    if (cfg.trace) {
      // One clock read for the whole datagram: every frame in it is about to go
      // on the wire at the same moment, so they share the pre-send timestamp.
      const uint64_t pre_send = util::now_ns();
      const uint8_t* p = dgram + sizeof(wire::PktHeader);
      for (uint16_t i = 0; i < packer.count(); ++i) {
        msg::Header h;
        std::memcpy(&h, p, sizeof(h));
        trace_acc.record(h.seq_id,
                         pre_send > h.send_ts_ns ? pre_send - h.send_ts_ns : 0);
        p += msg::frame_size(h.type);
      }
    }

    wire::stamp_send_ts(const_cast<uint8_t*>(dgram), util::now_ns());
    const int reached = net.send_all(dgram, dlen);

    ++pkt_seq;
    ++stats.datagrams;
    stats.frames += packer.count();
    if (reached == 0) ++stats.send_fail;
    else if (static_cast<size_t>(reached) < net.peer_count()) ++stats.partial;
    last_activity = util::now_ns();

    // Hand this datagram to the duplication slot and switch buffers, so filling the
    // next one cannot overwrite the bytes the copy will be sent from.
    if (cfg.duplicate) {
      dup_buf = pktbuf[cur].data();
      dup_len = dlen;
      cur ^= 1;
    }
  }

  print_stats(stats);
  if (cfg.trace) {
    const metrics::Report r = trace_acc.report();
    fprintf(stderr, "---- producer -> pre-send (ns, this host's clock only) ----\n");
    fprintf(stderr, "min=%llu p50=%llu p99=%llu p99.9=%llu p99.99=%llu max=%llu\n",
            (unsigned long long)r.lat_min, (unsigned long long)r.p50,
            (unsigned long long)r.p99, (unsigned long long)r.p999,
            (unsigned long long)r.p9999, (unsigned long long)r.lat_max);
  }
  return 0;
}
