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
#include <atomic>
#include <csignal>
#include <cstdint>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "cpu.h"
#include "delivery.h"
#include "iou_backend.h"
#include "metrics.h"
#include "poll_split.h"
#include "shm_ring.h"
#include "shm_segment.h"
#include "udp_backend.h"
#include "util.h"
#include "wire.h"

namespace {

// Which mechanism takes datagrams off the socket. See iou_backend.h for why io_uring
// is a candidate at all and what it changes.
enum class Backend : uint8_t { kUdp, kIoUring };

bool parse_backend(const std::string& s, Backend* out) {
  if (s == "udp") { *out = Backend::kUdp; return true; }
  if (s == "iouring" || s == "io_uring") { *out = Backend::kIoUring; return true; }
  return false;
}

// Set from a signal handler so the relay can stop at a loop boundary and still run
// its reporting. Without this, a SIGTERM from the harness kills the process before
// it writes out the samples it spent the whole run collecting.
volatile std::sig_atomic_t g_stop = 0;

void on_signal(int) { g_stop = 1; }

struct Config {
  // "monotonic" reproduces the baseline exactly; "bitmap" drops the ordering
  // constraint so a copy held back longer than a loss burst still counts.
  std::string delivery = "monotonic";
  // Reproduces the baseline's construction order: instrumentation buffers built after the
  // socket is already bound. Kept as a flag so the two orders can be measured against each
  // other in one binary, back to back, instead of across two builds minutes apart.
  bool late_alloc = false;
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
  // Default stays on the kernel-UDP path, so switching is opt-in and the baseline
  // remains whatever the earlier measurements were taken with.
  Backend backend = Backend::kUdp;
  // io_uring only: whether to wait inside the kernel so it can poll the device queue
  // on this core (napi), or read completions straight out of shared memory with no
  // system call at all (polled). Auto decides from the bound address, exactly as the
  // UDP backend decides between busy-poll and spin.
  iou::PollMode iou_poll = iou::PollMode::kAuto;
  // io_uring only: depth of the provided-buffer pool, which is the receive path's
  // flow-control slack. Exposed because running out of it drops datagrams here rather
  // than on the wire, so it needs to be measurable, not guessed at.
  uint32_t recv_buffers = 16384;
  // Ask the kernel to timestamp each datagram as it enters the receive path, which splits
  // the wire leg at the receiver's kernel boundary. Measurement-only: it changes recv()
  // into recvmsg() with control data, so it must not be on in the configuration whose
  // latency is being reported.
  bool rx_timestamp = false;
  // Run the receive loop as two threads: one that does nothing but poll the socket, and one
  // that parses, gates and publishes. Busy-poll only happens while a thread is inside recv,
  // so a single-threaded loop stops polling exactly when it falls behind -- see
  // poll_split.h for the feedback loop that creates and the measurements behind it.
  bool split_poll = false;
  int publish_core = -1;   // core for the parse-and-publish thread
  uint32_t queue_slots = 8192;
};

struct Stats {
  uint64_t datagrams = 0;    // first-copy datagrams received
  uint64_t frames = 0;       // frames published to the consumer
  uint64_t malformed = 0;    // rejected as not well-formed
  uint64_t pkt_gaps = 0;     // times the first-copy pkt_seq sequence jumped
  uint64_t pkt_lost = 0;     // first-copy datagrams missing, from those jumps
  uint64_t pkt_reorder = 0;  // arrived behind one we had already seen
  // Where the first gaps opened, in datagrams already received. A loss that is spread
  // through the run and a single stall both show up as "n datagrams lost"; only the
  // position separates them, and they call for opposite fixes.
  static constexpr int kGapLog = 8;
  uint64_t gap_at[kGapLog] = {0};
  uint64_t gap_len[kGapLog] = {0};
  uint64_t dup_recv = 0;     // redundant copies that reached us
  // Copies whose frames passed the gate, i.e. that arrived before their original did.
  //
  // This is NOT the same as "rescued a loss", and the difference only appeared once copies
  // were given a four-tuple of their own. When both copies share a socket the copy is always
  // sent second and travels the same path, so a copy that gets past the gate does imply the
  // original never came. Once the copy takes a different path it can simply be faster, and
  // then it wins the race while the original arrives moments later and is suppressed. On a
  // cross-region path this counter read 1,074,810 out of 1,124,242 -- it was measuring path
  // diversity, not recovery. Whether anything was truly lost is the gate's seq-gap count.
  uint64_t dup_first = 0;
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
    else if (a == "--delivery") c.delivery = next();
    else if (a == "--late-alloc") c.late_alloc = true;
    else if (a == "--trace") c.trace = true;
    else if (a == "--stage-csv") c.stage_csv = next();
    else if (a == "--stage-capacity") c.stage_capacity = std::stoull(next());
    else if (a == "--backend") {
      const std::string b = next();
      if (!parse_backend(b, &c.backend)) {
        fprintf(stderr, "--backend must be udp|iouring\n");
        std::exit(2);
      }
    }
    else if (a == "--recv-buffers")
      c.recv_buffers = static_cast<uint32_t>(std::stoul(next()));
    else if (a == "--rx-tstamp") c.rx_timestamp = true;
    else if (a == "--split-poll") c.split_poll = true;
    else if (a == "--publish-core") c.publish_core = std::stoi(next());
    else if (a == "--queue-slots")
      c.queue_slots = static_cast<uint32_t>(std::stoul(next()));
    else if (a == "--iou-poll") {
      const std::string m = next();
      if (!iou::parse_poll_mode(m, &c.iou_poll)) {
        fprintf(stderr, "--iou-poll must be auto|napi|polled\n");
        std::exit(2);
      }
    }
    else {
      fprintf(stderr, "unknown arg: %s\n", a.c_str());
      std::exit(2);
    }
  }
  if (!is_power_of_two(c.slots)) {
    fprintf(stderr, "--slots must be a power of two (got %u)\n", c.slots);
    std::exit(2);
  }
  if (!is_power_of_two(c.queue_slots)) {
    fprintf(stderr, "--queue-slots must be a power of two (got %u)\n", c.queue_slots);
    std::exit(2);
  }
  return c;
}

void print_stats(const Stats& s, const delivery::Stats& g) {
  fprintf(stderr, "---- receiver ----\n");
  fprintf(stderr, "datagrams recvd : %llu\n", (unsigned long long)s.datagrams);
  fprintf(stderr, "frames delivered: %llu\n", (unsigned long long)s.frames);
  fprintf(stderr, "malformed       : %llu\n", (unsigned long long)s.malformed);
  fprintf(stderr, "datagram gaps   : %llu (%llu datagrams)\n",
          (unsigned long long)s.pkt_gaps, (unsigned long long)s.pkt_lost);
  for (int i = 0; i < Stats::kGapLog && i < (int)s.pkt_gaps; ++i) {
    fprintf(stderr, "  gap[%d]        : %llu datagrams, after %llu received\n", i,
            (unsigned long long)s.gap_len[i], (unsigned long long)s.gap_at[i]);
  }
  fprintf(stderr, "datagram reorder: %llu\n", (unsigned long long)s.pkt_reorder);
  const uint64_t offered = s.datagrams + s.pkt_lost;
  if (offered) {
    fprintf(stderr, "first-copy loss : %.4f%%\n",
            100.0 * static_cast<double>(s.pkt_lost) /
                static_cast<double>(offered));
  }
  fprintf(stderr, "duplicates recvd: %llu\n", (unsigned long long)s.dup_recv);
  fprintf(stderr, "  arrived first  : %llu copies admitted before their original\n",
          (unsigned long long)s.dup_first);
  fprintf(stderr, "                   (a copy on its own path can win the race, so this is\n"
                  "                    not a recovery count -- see seq gaps below)\n");
  fprintf(stderr, "---- delivery gate ----\n");
  fprintf(stderr, "published       : %llu\n", (unsigned long long)g.delivered);
  fprintf(stderr, "suppressed      : %llu (duplicate or too late to insert)\n",
          (unsigned long long)g.suppressed);
  fprintf(stderr, "seq gaps        : %llu (%llu frames never arrived)\n",
          (unsigned long long)g.gaps, (unsigned long long)g.missing);
  if (g.rescued) {
    fprintf(stderr, "rescued         : %llu frames admitted below the high-water mark\n",
            (unsigned long long)g.rescued);
  }
}

// The receive-side kernel delivery gap, where the backend can supply one.
//
// Only the kernel-UDP path can: it comes from a control message on recvmsg(). The io_uring
// path receives into provided buffers with no control data attached, so there is nothing
// to read and it reports zero rather than pretending. Overload resolution rather than a
// virtual, to keep the relay loop free of indirection.
inline uint64_t rx_delivery_of(const udp::Receiver& net) { return net.rx_delivery_ns(); }
inline uint64_t rx_delivery_of(const iou::Receiver&) { return 0; }

// Returns a borrowed datagram to its backend however the scope exits. The loop below
// has several early-out paths (malformed framing, a datagram carrying nothing new), and
// on the io_uring path a buffer that is not released is a buffer the kernel never gets
// back -- which would show up as escalating loss, not as an obvious bug.
template <class ReceiverT>
class BorrowGuard {
 public:
  explicit BorrowGuard(ReceiverT& net) : net_(net) {}
  ~BorrowGuard() { net_.release(); }
  BorrowGuard(const BorrowGuard&) = delete;
  BorrowGuard& operator=(const BorrowGuard&) = delete;

 private:
  ReceiverT& net_;
};

// The relay, written once against either backend. Both expose borrow()/release() with
// the same contract: borrow() yields a pointer valid until release(), and release()
// must happen before the next borrow().
template <class ReceiverT, class GateT>
int run_relay(ReceiverT& net, const Config& cfg, metrics::Accumulator& trace_acc,
              metrics::StageAccumulator& stages) {
  shm::Segment seg = shm::Segment::open(cfg.shm_name,
                                        shm::region_size(cfg.slots),
                                        /*create=*/true);
  shm::Ring ring;
  ring.attach(seg.base(), cfg.slots, /*init=*/true);

  fprintf(stderr,
          "receiver: shm=%s slots=%u listening on %s core=%d recv=%s\n",
          cfg.shm_name.c_str(), cfg.slots,
          udp::describe(net.bound()).c_str(), cfg.core, net.mode_name());

  Stats stats;
  // Every frame passes this gate, which is what makes the delivered stream strictly
  // monotonic and what silently absorbs the redundant copies.
  GateT gate;
  uint64_t next_pkt_seq = 0;  // 0 = nothing seen yet
  const uint64_t idle_ns = cfg.idle_ms * 1000000ull;
  uint64_t last_activity = util::now_ns();
  uint32_t empty_polls = 0;

  // One implementation of the per-datagram work, so the single-threaded loop and the
  // split-poll publisher cannot drift apart. Only ever called from one thread.
  auto process = [&](const uint8_t* dgram, uint32_t n, uint64_t arrival_ns,
                     uint64_t rx_delivery_ns) {
    wire::Walker walker;
    if (!walker.begin(dgram, n)) {
      ++stats.malformed;
      return;
    }

    const bool is_dup = walker.duplicate();

    // Datagram-level loss accounting over first copies only, which is independent of the
    // seq_id gaps the consumer reports. It separates "the network dropped a packet" from
    // "the sender fell behind the source ring" -- indistinguishable at the consumer, and
    // they call for opposite fixes.
    if (!is_dup) {
      const uint64_t ps = walker.pkt_seq();
      if (next_pkt_seq == 0 || ps == next_pkt_seq) {
        next_pkt_seq = ps + 1;
      } else if (ps > next_pkt_seq) {
        if (stats.pkt_gaps < Stats::kGapLog) {
          stats.gap_at[stats.pkt_gaps] = stats.datagrams;
          stats.gap_len[stats.pkt_gaps] = ps - next_pkt_seq;
        }
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
      // The gate is the whole delivery policy: strictly increasing seq_id, so duplicates
      // and stragglers are dropped rather than breaking monotonicity.
      if (!gate.admit(h.seq_id)) continue;
      if (cfg.trace) {
        trace_acc.record(h.seq_id,
                         arrival_ns > h.send_ts_ns ? arrival_ns - h.send_ts_ns : 0);
      }
      ring.publish(frame, len);
      if (stages.enabled()) {
        const uint64_t sender_ts = walker.sender_ts_ns();
        const uint64_t published_ns = util::now_ns();
        stages.record(
            h.seq_id,
            sender_ts > h.send_ts_ns ? sender_ts - h.send_ts_ns : 0,
            arrival_ns > sender_ts ? arrival_ns - sender_ts : 0,
            published_ns > arrival_ns ? published_ns - arrival_ns : 0,
            rx_delivery_ns);
      }
      ++admitted;
    }
    stats.frames += admitted;

    if (is_dup) {
      ++stats.dup_recv;
      // The copy beat its original through the gate. That means the original had not
      // arrived *yet* -- not that it never will. See dup_first.
      if (admitted > 0) ++stats.dup_first;
    } else {
      ++stats.datagrams;
    }
  };

  if (cfg.split_poll) {
    // Two threads. The poller does nothing but take datagrams off the socket and hand
    // them over, so it is never absent from recv for longer than a memcpy and the
    // busy-poll feedback loop described in poll_split.h cannot close.
    poll_split::Ring q(cfg.queue_slots, 2048);
    std::atomic<bool> poller_done{false};
    fprintf(stderr, "receiver: split poll, queue=%u slots, publish core=%d\n",
            cfg.queue_slots, cfg.publish_core);

    std::thread poller([&] {
      if (cfg.core >= 0) cpu::pin_to_core(cfg.core);
      uint64_t seen = 0;
      uint32_t idle = 0;
      uint64_t last = util::now_ns();
      while (!g_stop) {
        const uint8_t* d = nullptr;
        const int n = net.borrow(&d);
        if (n < 0) {
          ++idle;
          const bool check = net.blocking() || (idle & kIdleCheckMask) == 0;
          if (idle_ns && check && util::now_ns() - last > idle_ns) break;
          continue;
        }
        idle = 0;
        // Both stamps must be taken here, in the polling thread. Taken on the other side
        // of the queue they would include the time spent in it, folding the queueing this
        // design introduces into the latency it is meant to protect.
        const uint64_t arrival = util::now_ns();
        const uint64_t rxd = rx_delivery_of(net);
        q.push(d, static_cast<uint32_t>(n), arrival, rxd);
        net.release();
        last = util::now_ns();
        ++seen;
      }
      poller_done.store(true, std::memory_order_release);
      fprintf(stderr, "poller: %llu datagrams, queue high-water %llu of %u, "
                      "dropped %llu\n",
              (unsigned long long)seen, (unsigned long long)q.high_water(),
              q.capacity(), (unsigned long long)q.dropped());
    });

    if (cfg.publish_core >= 0) cpu::pin_to_core(cfg.publish_core);
    while (cfg.count == 0 || stats.frames < cfg.count) {
      poll_split::Slot slot;
      const uint8_t* d = q.peek(&slot);
      if (d == nullptr) {
        if (poller_done.load(std::memory_order_acquire) && q.empty()) break;
        continue;
      }
      process(d, slot.len, slot.arrival_ns, slot.rx_delivery_ns);
      q.release();
    }
    g_stop = 1;
    poller.join();
  } else {
  while (!g_stop && (cfg.count == 0 || stats.frames < cfg.count)) {
    const uint8_t* dgram = nullptr;
    const int n = net.borrow(&dgram);
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
    // Captured before the per-frame loop, because borrow() overwrites it on the next
    // datagram and every frame in this one shares the same arrival.
    const uint64_t rx_delivery_ns = rx_delivery_of(net);
    empty_polls = 0;
    // From here on every exit from the iteration hands the datagram back.
    BorrowGuard<ReceiverT> borrowed(net);
    process(dgram, static_cast<uint32_t>(n), arrival_ns, rx_delivery_ns);
    last_activity = util::now_ns();
  }
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

}  // namespace

// Chooses the delivery policy once, at startup, and hands the loop a concrete
// type. Everything after this point is monomorphic: the policy that was not
// chosen is not in the instruction stream and its window is never allocated.
template <class ReceiverT>
int run_with_policy(ReceiverT& net, const Config& cfg, metrics::Accumulator& trace_acc,
                    metrics::StageAccumulator& stages) {
  if (cfg.delivery == "bitmap")
    return run_relay<ReceiverT, delivery::BitmapGate>(net, cfg, trace_acc, stages);
  return run_relay<ReceiverT, delivery::MonotonicGate>(net, cfg, trace_acc, stages);
}

int main(int argc, char** argv) {
  Config cfg = parse_args(argc, argv);
  if (cfg.delivery != "monotonic" && cfg.delivery != "bitmap") {
    fprintf(stderr, "receiver: --delivery must be monotonic or bitmap\n");
    return 1;
  }
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  if (cfg.core >= 0 && !cpu::pin_to_core(cfg.core)) return 1;

  // Built before the socket exists, and that ordering is the whole point.
  //
  // StageAccumulator at --stage-capacity 25000000 reserves five arrays and zeroes them:
  // roughly 600 MB of first-touch page faults, a few hundred milliseconds during which this
  // thread does not call recv even once. Constructed after bind -- where these two lines
  // used to live -- the socket is already accepting datagrams throughout, the receive buffer
  // overruns, and the kernel discards whatever will not fit. At 200k datagrams/s that is a
  // single contiguous hole of ~47,500 datagrams in every measured run, and it is what the
  // baseline publishes as "first-copy loss 1.27%": the instrument, not the path. Six
  // counterbalanced blocks put the difference at -1.30 percentage points, sign p=0.031,
  // every block agreeing.
  //
  // It does not touch the latency percentiles, and that is worth stating because the
  // opposite is the easy assumption. The hole opens while the receiver is starting, which
  // bench.sh follows with an 8-second warm-up before the consumer takes its first sample,
  // so the backlog that arrives late is never measured. In the same six blocks p99.9 and
  // p99.99 did not resolve. Allocating first costs nothing and removes the loss.
  std::unique_ptr<metrics::Accumulator> trace_acc;
  std::unique_ptr<metrics::StageAccumulator> stages;
  const auto build_instruments = [&] {
    trace_acc = std::make_unique<metrics::Accumulator>(cfg.trace ? cfg.stage_capacity : 0);
    stages = std::make_unique<metrics::StageAccumulator>(
        cfg.stage_csv.empty() ? 0 : cfg.stage_capacity);
  };
  if (!cfg.late_alloc) build_instruments();

  if (cfg.backend == Backend::kIoUring) {
    iou::Options opts;
    opts.poll = cfg.iou_poll;
    opts.recv_buffers = cfg.recv_buffers;
    iou::Receiver net;
    if (!net.open(cfg.bind_addr, cfg.port, opts)) return 1;
    if (cfg.late_alloc) build_instruments();
    const int rc = run_with_policy(net, cfg, *trace_acc, *stages);
    // Both are signs the receive path could not keep up in a way the datagram
    // counters alone would not show: a re-arm means multishot stopped and had to be
    // restarted, and an out-of-buffers event means the kernel had nowhere to put a
    // packet. Either can cost datagrams, so neither is left implicit.
    if (net.rearms() > 1 || net.no_buffer_events() != 0) {
      fprintf(stderr, "io_uring: recv re-arms=%llu out-of-buffers=%llu\n",
              (unsigned long long)net.rearms(),
              (unsigned long long)net.no_buffer_events());
    }
    return rc;
  }

  udp::Options opts;
  opts.busy_poll_us = cfg.busy_poll_us;
  opts.rx_timestamp = cfg.rx_timestamp;
  udp::Receiver net;
  if (!net.open(cfg.bind_addr, cfg.port, opts)) return 1;
  if (cfg.late_alloc) build_instruments();
  const int rc = run_with_policy(net, cfg, *trace_acc, *stages);
  if (net.rx_timestamp() && net.missing_rx_stamps() != 0) {
    // A datagram with no stamp contributes a zero to the column, so the count has to be
    // reported or the distribution would be quietly biased toward zero.
    fprintf(stderr, "rx stamps missing: %llu\n",
            (unsigned long long)net.missing_rx_stamps());
  }
  return rc;
}
