// Sender: drains the producer's shared-memory ring and puts the messages on the
// wire.
//
// The loop is deliberately the whole design:
//
//   forever:
//       take everything the ring has right now, up to one datagram's worth
//       send that datagram to every destination, on every redundancy stream
//
// Batch size is never configured. At a low rate the inner drain finds one
// message and sends it immediately; under load it finds enough to fill an MTU.
// A fixed batch would instead charge the *first* message in it the full time
// needed to collect the rest -- 23 us at 1M msg/s for a 23-message batch, which
// is the entire wire time (plans/00_TRADEOFFS.txt D-03).
//
// Usage: sender --dest HOST:PORT [--dest ...] [--shm NAME] [--slots N]
//               [--streams N] [--src-port P] [--control-port P]
//               [--count N] [--idle-ms MS] [--no-from-edge]
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "control.h"
#include "hist.h"
#include "message.h"
#include "net.h"
#include "shm_ring.h"
#include "shm_segment.h"
#include "tsc.h"
#include "util.h"
#include "wire.h"

namespace {

volatile sig_atomic_t g_running = 1;
void on_signal(int) { g_running = 0; }

struct Config {
  std::string shm_name = "/fanout_ring";
  uint32_t slots = 1024;
  std::vector<std::string> dests;
  uint16_t src_port = 0;      // 0 = ephemeral; base port when streams > 1
  uint16_t streams = 1;       // redundant copies, each on its own source port
  uint16_t control_port = 0;  // 0 = no handshake
  std::string control_host = "";
  uint64_t count = 0;         // 0 = until idle or signalled
  uint64_t idle_ms = 2000;
  bool from_edge = true;
};

void usage_die(const char* why) {
  std::fprintf(stderr, "sender: %s\n", why);
  std::fprintf(stderr,
               "usage: sender --dest HOST:PORT [--dest ...] [--shm NAME] "
               "[--slots N]\n"
               "              [--streams N] [--src-port P] "
               "[--control-port P] [--control-host H]\n"
               "              [--count N] [--idle-ms MS] [--no-from-edge]\n");
  std::exit(2);
}

Config parse_args(int argc, char** argv) {
  Config c;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) usage_die(("missing value for " + a).c_str());
      return argv[++i];
    };
    if (a == "--shm") c.shm_name = next();
    else if (a == "--slots") c.slots = static_cast<uint32_t>(std::stoul(next()));
    else if (a == "--dest") c.dests.push_back(next());
    else if (a == "--src-port") c.src_port = static_cast<uint16_t>(std::stoul(next()));
    else if (a == "--streams") c.streams = static_cast<uint16_t>(std::stoul(next()));
    else if (a == "--control-port") c.control_port = static_cast<uint16_t>(std::stoul(next()));
    else if (a == "--control-host") c.control_host = next();
    else if (a == "--count") c.count = std::stoull(next());
    else if (a == "--idle-ms") c.idle_ms = std::stoull(next());
    else if (a == "--no-from-edge") c.from_edge = false;
    else usage_die(("unknown arg: " + a).c_str());
  }
  if (c.dests.empty()) usage_die("at least one --dest is required");
  if (c.streams < 1) usage_die("--streams must be >= 1");
  if (c.streams > 1 && c.src_port == 0)
    usage_die("--streams > 1 needs an explicit --src-port base");
  return c;
}

// Wait for the producer's segment to exist *and* to have been sized.
//
// producer.cpp shm_unlinks on exit, and creates before it ftruncates, so a
// naive open races on both ends: we can attach to a segment that is about to
// vanish, or mmap one that is still zero-length and take a SIGBUS on first
// touch. Checking the size closes both windows. shm::Segment::open exits the
// process on failure, so the probe has to happen before we call it.
bool wait_for_segment(const std::string& name, size_t need, unsigned timeout_ms) {
  const unsigned step_ms = 20;
  for (unsigned waited = 0;; waited += step_ms) {
    const int fd = ::shm_open(name.c_str(), O_RDWR, 0600);
    if (fd >= 0) {
      struct stat st {};
      const bool sized = ::fstat(fd, &st) == 0 &&
                         static_cast<size_t>(st.st_size) >= need;
      ::close(fd);
      if (sized) return true;
    }
    if (waited >= timeout_ms) return false;
    timespec req{0, static_cast<long>(step_ms) * 1000000L};
    while (nanosleep(&req, &req) != 0 && req.tv_nsec > 0) {
    }
  }
}

struct Stats {
  uint64_t messages = 0;
  uint64_t datagrams = 0;
  uint64_t lapped = 0;      // producer overwrote slots before we read them
  uint64_t lapped_msgs = 0; // messages lost that way -- before the wire
  uint64_t clock_backwards = 0;
  hist::Latency pickup;  // S0 -> S1  producer stamp to our pickup
  hist::Latency encode;  // S1 -> S2  drain + frame into the datagram
  hist::Latency txcall;  // S2 -> S3  sendmmsg
  hist::Latency batch;   // messages per datagram
};

void report(const Config& cfg, const Stats& s,
            const std::vector<net::TxStream>& streams) {
  std::fprintf(stderr, "---- sender ----\n");
  std::fprintf(stderr, "messages     : %llu\n", (unsigned long long)s.messages);
  std::fprintf(stderr, "datagrams    : %llu\n", (unsigned long long)s.datagrams);
  std::fprintf(stderr, "streams      : %u  dests: %zu\n", cfg.streams,
               cfg.dests.size());
  std::fprintf(stderr, "ring lapped  : %llu events, ~%llu messages lost "
                       "BEFORE the wire\n",
               (unsigned long long)s.lapped, (unsigned long long)s.lapped_msgs);
  for (size_t i = 0; i < streams.size(); ++i) {
    std::fprintf(stderr,
                 "stream %zu     : sent=%llu blocked=%llu partial=%llu errors=%llu\n",
                 i, (unsigned long long)streams[i].sent(),
                 (unsigned long long)streams[i].blocked(),
                 (unsigned long long)streams[i].partial(),
                 (unsigned long long)streams[i].errors());
  }
  if (s.clock_backwards)
    std::fprintf(stderr, "clock backwards: %llu (calibration or clock skew)\n",
                 (unsigned long long)s.clock_backwards);

  auto line = [](const char* name, const hist::Latency& h, const char* unit) {
    if (h.count() == 0) return;
    std::fprintf(stderr,
                 "%-13s: n=%llu mean=%.0f p50=%llu p99=%llu p99.9=%llu "
                 "p99.99=%llu max=%llu %s\n",
                 name, (unsigned long long)h.count(), h.mean(),
                 (unsigned long long)h.percentile(0.50),
                 (unsigned long long)h.percentile(0.99),
                 (unsigned long long)h.percentile(0.999),
                 (unsigned long long)h.percentile(0.9999),
                 (unsigned long long)h.max(), unit);
  };
  line("S0->S1 pickup", s.pickup, "ns");
  line("S1->S2 encode", s.encode, "ns");
  line("S2->S3 tx", s.txcall, "ns");
  line("batch size", s.batch, "msgs");
}

}  // namespace

int main(int argc, char** argv) {
  const Config cfg = parse_args(argc, argv);
  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);

  // No page faults on the hot path. A major fault is a millisecond, which is
  // three orders of magnitude past anything else we are measuring.
  if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
    std::fprintf(stderr, "sender: mlockall failed (%s), continuing\n",
                 std::strerror(errno));

  std::vector<net::Endpoint> dests;
  for (const auto& d : cfg.dests) {
    net::Endpoint ep;
    if (!net::Endpoint::parse(d, &ep)) usage_die(("bad --dest: " + d).c_str());
    dests.push_back(ep);
  }

  if (cfg.control_port != 0) {
    control::Hello h{};
    h.magic = control::kMagic;
    h.version = control::kVersion;
    h.n_streams = cfg.streams;
    h.encoding = wire::kEncRaw;
    h.run_id = util::now_ns();
    control::HelloAck ack{};
    control::Client client;
    const std::string host =
        cfg.control_host.empty() ? cfg.dests[0].substr(0, cfg.dests[0].rfind(':'))
                                 : cfg.control_host;
    if (!client.connect_hello(host, cfg.control_port, h, &ack)) return 1;
    if (ack.slots != cfg.slots)
      std::fprintf(stderr,
                   "sender: warning, receiver ring has %u slots, we were told "
                   "%u\n",
                   ack.slots, cfg.slots);
    client.close();
  }

  const size_t need = shm::region_size(cfg.slots);
  if (!wait_for_segment(cfg.shm_name, need, 10000)) {
    std::fprintf(stderr, "sender: producer segment %s never appeared\n",
                 cfg.shm_name.c_str());
    return 1;
  }
  shm::Segment seg = shm::Segment::open(cfg.shm_name, need, /*create=*/false);
  shm::Ring ring;
  ring.attach(seg.base(), cfg.slots, /*init=*/false);

  const net::SockOpts opts;
  std::vector<net::TxStream> streams(cfg.streams);
  for (uint16_t s = 0; s < cfg.streams; ++s) {
    const uint16_t port = cfg.src_port ? static_cast<uint16_t>(cfg.src_port + s) : 0;
    if (!streams[s].open(port, dests, opts)) {
      std::fprintf(stderr, "sender: cannot open stream %u\n", s);
      return 1;
    }
  }

  const tsc::Calibration calib = tsc::Calibration::measure(20);

  std::fprintf(stderr,
               "sender: shm=%s slots=%u dests=%zu streams=%u src_port=%u "
               "from_edge=%d busy_poll=%d counter=%.1fMHz\n",
               cfg.shm_name.c_str(), cfg.slots, dests.size(), cfg.streams,
               cfg.src_port, static_cast<int>(cfg.from_edge),
               static_cast<int>(streams[0].applied().busy_poll),
               calib.hz() / 1e6);

  alignas(64) uint8_t dgram[wire::kMaxDatagram];
  wire::Builder builder(dgram, wire::kMaxDatagram);

  // Starting at the live edge matters: attaching at index 0 would replay
  // whatever the producer wrote before we existed, flooding the wire with
  // stale messages whose measured latency is meaningless.
  uint64_t read_index = cfg.from_edge ? ring.live_edge() : 0;
  uint32_t dgram_id = 0;
  Stats st;
  const uint64_t idle_ns = cfg.idle_ms * 1000000ull;
  uint64_t last_progress = util::now_ns();

  while (g_running && (cfg.count == 0 || st.messages < cfg.count)) {
    builder.begin(dgram_id, 0, wire::kEncRaw, wire::kRoleData);
    const uint64_t t_batch_start = tsc::ticks();
    unsigned in_batch = 0;

    for (;;) {
      // Only offer the ring an in-place destination when a whole frame is
      // guaranteed to fit; read() copies up to kFrameCap and cannot be told a
      // smaller bound.
      uint8_t* dst = builder.append_ptr(shm::kFrameCap);
      if (dst == nullptr) break;  // datagram full, flush it

      uint32_t len = 0;
      uint64_t resume = 0;
      const auto rc = ring.read(read_index, dst, &len, &resume);
      if (rc == shm::Ring::FrameStatus::kEmpty) break;
      if (rc == shm::Ring::FrameStatus::kLapped) {
        // We fell behind the producer. These messages are lost before they ever
        // reached the network, so they must be counted separately -- otherwise
        // our own slowness is charged to the transport.
        ++st.lapped;
        st.lapped_msgs += resume > read_index ? resume - read_index : 0;
        read_index = resume;
        break;
      }

      // This stage crosses a clock domain: the producer stamped CLOCK_REALTIME,
      // everything internal is in ticks. Reading realtime directly costs the
      // same one clock read and avoids the calibration slope error entirely --
      // a 2e-6 slope error is a couple of microseconds over a one-second run,
      // which is an order of magnitude larger than the stage being measured.
      // Ticks stay for stage *differences* inside one process, where the slope
      // cancels out.
      const uint64_t pick_rt = tsc::realtime_ns();
      const auto* h = reinterpret_cast<const msg::Header*>(dst);
      if (pick_rt >= h->send_ts_ns) st.pickup.record(pick_rt - h->send_ts_ns);
      else ++st.clock_backwards;

      builder.append_commit(len, h->seq_id, h->send_ts_ns);
      ++read_index;
      ++st.messages;
      ++in_batch;
      if (cfg.count != 0 && st.messages >= cfg.count) break;
    }

    if (in_batch == 0) {
      if (util::now_ns() - last_progress > idle_ns) break;  // producer done
      continue;                                             // spin, never sleep
    }

    const uint64_t t_encoded = tsc::ticks();
    auto* hdr = reinterpret_cast<wire::DgramHeader*>(dgram);
    for (uint16_t s = 0; s < cfg.streams; ++s) {
      // Redundant copies differ only in the stream id -- and therefore in the
      // source port they leave from, which is the whole point (D-04).
      hdr->stream_id = s;
      streams[s].send_all(dgram, builder.size());
    }
    const uint64_t t_sent = tsc::ticks();

    st.encode.record(calib.ticks_to_ns(t_encoded - t_batch_start));
    st.txcall.record(calib.ticks_to_ns(t_sent - t_encoded));
    st.batch.record(in_batch);
    ++st.datagrams;
    ++dgram_id;
    last_progress = util::now_ns();
  }

  report(cfg, st, streams);
  for (auto& s : streams) s.close();
  return 0;
}
