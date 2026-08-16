// Receiver: takes datagrams off the network and republishes the frames inside
// them into a shared-memory ring for the consumer to measure.
//
// Second half of the transport under test. Like the sender it is a pure relay:
// frames go into the ring byte-identical to how the producer published them, so
// seq_id and send_ts_ns reach the consumer untouched.
//
// The ring is created here, so the receiver must be started before the consumer
// (which only ever opens an existing segment).
//
// Usage: receiver [--shm NAME] [--slots N] [--port P] [--bind ADDR]
//                 [--core N] [--count N] [--idle-ms MS] [--busy-poll US]
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "cpu.h"
#include "delivery.h"
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
  std::string shm_name = "/fanout_out";
  uint32_t slots = 1024;
  uint16_t port = 51000;
  std::string bind_addr;  // empty = all interfaces
  int core = -1;
  uint64_t count = 0;       // frames to deliver, 0 = unlimited
  uint64_t idle_ms = 5000;  // exit after this long with no datagrams, 0 = never
  // Per-stage timing capture: how long each event waited in the source ring, how
  // long it spent getting here, and how long until it was published. Written to
  // preallocated memory and dumped once after the run.
  std::string stage_csv;
  size_t stage_capacity = 8u << 20;  // 8M messages
  // Chosen automatically from the bound address: busy-poll on a real NIC, plain
  // spin on loopback, where there is no NAPI instance to poll and blocking recv()
  // only adds a wake-up. --spin and --busy-poll force it for the comparison runs
  // in the write-up; they are not intended as deployment settings.
  int busy_poll_us = udp::kBusyPollAuto;
  // Report producer-timestamp -> arrival-here latency, i.e. everything up to but
  // excluding the shared-memory hop to the consumer. Combined with the sender's
  // own --trace this splits the end-to-end figure into three stages:
  //   producer -> pre-send   (sender, one clock)
  //   producer -> arrival    (here, needs the two clocks synchronised)
  //   producer -> consumer   (the consumer's own metric)
  // This is only meaningful because the hosts are synchronised to well under a
  // microsecond, which is two orders of magnitude below what we are measuring.
  bool trace = false;
};

struct Stats {
  uint64_t datagrams = 0;    // first-copy datagrams received
  uint64_t frames = 0;       // frames published to the consumer
  uint64_t malformed = 0;    // rejected as not well-formed
  uint64_t pkt_gaps = 0;     // times the first-copy pkt_seq sequence jumped
  uint64_t pkt_lost = 0;     // first-copy datagrams missing, from those jumps
  uint64_t pkt_reorder = 0;  // arrived behind one we had already seen
  uint64_t dup_recv = 0;     // redundant copies that reached us
  uint64_t dup_rescued = 0;  // copies that carried data the original never delivered
};

// How often the spin-mode loop is allowed to look at the clock: once every 1024
// empty polls, so the idle check costs nothing on the hot path.
inline constexpr uint32_t kIdleCheckMask = 0x3ff;

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
    else if (a == "--port") c.port = static_cast<uint16_t>(std::stoul(next()));
    else if (a == "--bind") c.bind_addr = next();
    else if (a == "--core") c.core = std::stoi(next());
    else if (a == "--count") c.count = std::stoull(next());
    else if (a == "--idle-ms") c.idle_ms = std::stoull(next());
    else if (a == "--busy-poll") c.busy_poll_us = std::stoi(next());
    else if (a == "--spin") c.busy_poll_us = 0;
    else if (a == "--trace") c.trace = true;
    else if (a == "--stage-csv") c.stage_csv = next();
    else if (a == "--stage-capacity") c.stage_capacity = std::stoull(next());
    else {
      fprintf(stderr, "unknown arg: %s\n", a.c_str());
      std::exit(2);
    }
  }
  if (!is_power_of_two(c.slots)) {
    fprintf(stderr, "--slots must be a power of two (got %u)\n", c.slots);
    std::exit(2);
  }
  return c;
}

void print_stats(const Stats& s, const delivery::MonotonicGate::Stats& g) {
  fprintf(stderr, "---- receiver ----\n");
  fprintf(stderr, "datagrams recvd : %llu\n", (unsigned long long)s.datagrams);
  fprintf(stderr, "frames delivered: %llu\n", (unsigned long long)s.frames);
  fprintf(stderr, "malformed       : %llu\n", (unsigned long long)s.malformed);
  fprintf(stderr, "datagram gaps   : %llu (%llu datagrams)\n",
          (unsigned long long)s.pkt_gaps, (unsigned long long)s.pkt_lost);
  fprintf(stderr, "datagram reorder: %llu\n", (unsigned long long)s.pkt_reorder);
  const uint64_t offered = s.datagrams + s.pkt_lost;
  if (offered) {
    fprintf(stderr, "first-copy loss : %.4f%%\n",
            100.0 * static_cast<double>(s.pkt_lost) /
                static_cast<double>(offered));
  }
  fprintf(stderr, "duplicates recvd: %llu\n", (unsigned long long)s.dup_recv);
  fprintf(stderr, "  rescued       : %llu datagrams the original never delivered\n",
          (unsigned long long)s.dup_rescued);
  fprintf(stderr, "---- delivery gate ----\n");
  fprintf(stderr, "published       : %llu\n", (unsigned long long)g.delivered);
  fprintf(stderr, "suppressed      : %llu (duplicate or too late to insert)\n",
          (unsigned long long)g.suppressed);
  fprintf(stderr, "seq gaps        : %llu (%llu frames never arrived)\n",
          (unsigned long long)g.gaps, (unsigned long long)g.missing);
}

}  // namespace

int main(int argc, char** argv) {
  Config cfg = parse_args(argc, argv);
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  if (cfg.core >= 0 && !cpu::pin_to_core(cfg.core)) return 1;

  udp::Options opts;
  opts.busy_poll_us = cfg.busy_poll_us;
  udp::Receiver net;
  if (!net.open(cfg.bind_addr, cfg.port, opts)) return 1;

  shm::Segment seg = shm::Segment::open(cfg.shm_name,
                                        shm::region_size(cfg.slots),
                                        /*create=*/true);
  shm::Ring ring;
  ring.attach(seg.base(), cfg.slots, /*init=*/true);

  fprintf(stderr,
          "receiver: shm=%s slots=%u listening on %s core=%d recv=%s\n",
          cfg.shm_name.c_str(), cfg.slots,
          udp::describe(net.bound()).c_str(), cfg.core,
          net.busy_poll_us() > 0 ? "busy-poll" : "spin");

  // One datagram's worth of receive space. Sized for jumbo frames so the same
  // binary works when the sender is configured for a larger datagram; a
  // truncated read would otherwise be silently misparsed.
  std::vector<uint8_t> buf(65536);

  Stats stats;
  // Every frame passes this gate, which is what makes the delivered stream strictly
  // monotonic and what silently absorbs the redundant copies.
  delivery::MonotonicGate gate;
  metrics::Accumulator trace_acc(cfg.trace ? (1u << 21) : 0);
  metrics::StageAccumulator stages(cfg.stage_csv.empty() ? 0 : cfg.stage_capacity);
  uint64_t next_pkt_seq = 0;  // 0 = nothing seen yet
  const uint64_t idle_ns = cfg.idle_ms * 1000000ull;
  uint64_t last_activity = util::now_ns();
  uint32_t empty_polls = 0;

  while (!g_stop && (cfg.count == 0 || stats.frames < cfg.count)) {
    const int n = net.recv(buf.data(), static_cast<uint32_t>(buf.size()));
    if (n < 0) {
      // Nothing arrived. In spin mode, reading the clock on every empty poll
      // would add tens of nanoseconds to the gap between a packet landing in the
      // queue and us asking for it, so the check is throttled. In busy-poll mode
      // each -1 is already a 100 ms timeout, so throttling would stretch the idle
      // deadline into minutes -- check every time instead.
      ++empty_polls;
      const bool time_to_check =
          net.blocking() || (empty_polls & kIdleCheckMask) == 0;
      if (idle_ns && time_to_check &&
          util::now_ns() - last_activity > idle_ns) {
        break;
      }
      continue;
    }
    // Stamp arrival once per datagram, before any per-frame work, so the figure is
    // "when did this land" rather than "when did we finish with it".
    const bool timing = cfg.trace || stages.enabled();
    const uint64_t arrival_ns = timing ? util::now_ns() : 0;
    empty_polls = 0;

    wire::Walker walker;
    if (!walker.begin(buf.data(), static_cast<uint32_t>(n))) {
      ++stats.malformed;
      continue;
    }

    const bool is_dup = walker.duplicate();

    // Datagram-level loss accounting over first copies only, which is independent
    // of the seq_id gaps the consumer reports. It separates "the network dropped a
    // packet" from "the sender fell behind the source ring" -- indistinguishable at
    // the consumer, and they call for opposite fixes.
    if (!is_dup) {
      const uint64_t ps = walker.pkt_seq();
      if (next_pkt_seq == 0 || ps == next_pkt_seq) {
        next_pkt_seq = ps + 1;
      } else if (ps > next_pkt_seq) {
        ++stats.pkt_gaps;
        stats.pkt_lost += ps - next_pkt_seq;
        next_pkt_seq = ps + 1;
      } else {
        ++stats.pkt_reorder;
      }
    }

    uint32_t admitted = 0;
    for (;;) {
      uint32_t len = 0;
      bool malformed = false;
      const uint8_t* frame = walker.next(&len, &malformed);
      if (frame == nullptr) {
        if (malformed) ++stats.malformed;
        break;
      }
      msg::Header h;
      std::memcpy(&h, frame, sizeof(h));
      // The gate is the whole delivery policy: strictly increasing seq_id, so
      // duplicates and stragglers are dropped rather than breaking monotonicity.
      if (!gate.admit(h.seq_id)) continue;
      if (cfg.trace) {
        trace_acc.record(h.seq_id,
                         arrival_ns > h.send_ts_ns ? arrival_ns - h.send_ts_ns : 0);
      }
      ring.publish(frame, len);
      if (stages.enabled()) {
        // Three legs of the journey, split at the two points we control:
        //   shm  = producer stamp -> sender's pre-send stamp (waited in source ring)
        //   wire = pre-send -> arrival here (kernel TX, NIC, wire, kernel RX)
        //   pub  = arrival -> published into this receiver's ring
        // shm and pub each use a single host's clock. wire spans both, which is why
        // the hosts are synchronised to well under a microsecond.
        const uint64_t sender_ts = walker.sender_ts_ns();
        const uint64_t published_ns = util::now_ns();
        stages.record(
            h.seq_id,
            sender_ts > h.send_ts_ns ? sender_ts - h.send_ts_ns : 0,
            arrival_ns > sender_ts ? arrival_ns - sender_ts : 0,
            published_ns > arrival_ns ? published_ns - arrival_ns : 0);
      }
      ++admitted;
    }
    stats.frames += admitted;

    if (is_dup) {
      ++stats.dup_recv;
      // A copy that got frames past the gate means the original never arrived.
      // This is the direct measure of what duplication buys, as opposed to how
      // much bandwidth it costs.
      if (admitted > 0) ++stats.dup_rescued;
    } else {
      ++stats.datagrams;
    }
    last_activity = util::now_ns();
  }

  print_stats(stats, gate.stats());
  if (cfg.trace) {
    const metrics::Report r = trace_acc.report();
    fprintf(stderr, "---- producer -> arrival here (ns, synchronised clocks) ----\n");
    fprintf(stderr, "min=%llu p50=%llu p99=%llu p99.9=%llu p99.99=%llu max=%llu\n",
            (unsigned long long)r.lat_min, (unsigned long long)r.p50,
            (unsigned long long)r.p99, (unsigned long long)r.p999,
            (unsigned long long)r.p9999, (unsigned long long)r.lat_max);
  }
  if (stages.enabled()) {
    fprintf(stderr, "stage samples   : %zu\n", stages.stored());
    if (!stages.dump_csv(cfg.stage_csv)) {
      fprintf(stderr, "receiver: failed to write %s\n", cfg.stage_csv.c_str());
    }
  }
  seg.unlink();
  return 0;
}
