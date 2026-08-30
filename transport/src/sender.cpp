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
#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "cpu.h"
#include "iou_backend.h"
#include "metrics.h"
#include "shm_ring.h"
#include "shm_segment.h"
#include "udp_backend.h"
#include "xdp_backend.h"
#include "util.h"
#include "wire.h"

namespace {

// Which mechanism carries the datagrams. Both are kernel UDP sockets configured
// identically; they differ only in how submission reaches the kernel, which is the
// whole point of being able to switch between them under an unchanged relay loop.
enum class Backend : uint8_t { kUdp, kIoUring, kXdp };

bool parse_backend(const std::string& s, Backend* out) {
  if (s == "udp") { *out = Backend::kUdp; return true; }
  if (s == "iouring" || s == "io_uring") { *out = Backend::kIoUring; return true; }
  if (s == "xdp" || s == "afxdp") { *out = Backend::kXdp; return true; }
  return false;
}

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
  // Kernel UDP sockets either way; io_uring changes only how submissions are handed
  // over. Default stays on the measured winner so switching is opt-in.
  Backend backend = Backend::kUdp;
  bool sqpoll = false;  // io_uring only: kernel-side submission thread
  int sq_core = -1;     // and which core it runs on
  // Source address for the sending sockets, for paths where the outgoing uplink is chosen
  // by source address rather than by destination. See udp::Options::src_addr -- on such a
  // host an unbound socket leaves by the default route, which is the management interface
  // rather than the link under test.
  std::string src_addr;
  // AF_XDP only. The interface is required rather than guessed: transmitting a frame
  // means choosing which wire it goes out of, and getting that wrong is not something
  // to infer silently.
  std::string xdp_iface;
  uint32_t xdp_queue = 0;
  bool xdp_udp_csum = false;
  // Measurement only, kernel-UDP path only: ask the kernel to stamp each datagram just
  // before it hands it to the driver, so the sending kernel's own transmit path can be
  // separated from the opaque leg beyond it. Single clock, so it is exact.
  bool tx_timestamp = false;
  // Send every datagram over two source/destination port pairs instead of one.
  //
  // Same-path duplication was measured as nearly useless -- 2 of 485 losses rescued --
  // because loss and delay arrive in correlated bursts, so back-to-back copies fail
  // together. Two port pairs decorrelate at least the queueing: different ports hash to
  // different receive queues and can take different fabric paths. The receiver needs no new
  // delivery logic at all, because the monotonic gate already publishes whichever copy
  // arrives first and silently suppresses the other.
  //
  // Costs twice the packet rate, which trades directly against fan-out: the packet-rate
  // ceiling divided by destinations is already the binding constraint at high fan-out.
  bool dual_path = false;

  // --- the redundant leg: dual path's four-tuple, opportunistic duplication's timing ---
  //
  // Measured, --dual-path bought 5.8x at p99.99 and charged 10% at p50 and 16% at p99.
  // The stage split placed those two effects in different halves of the path: 3,347 ns of
  // the 3,372 ns median cost lands upstream of the receiving kernel's stamp, and 5,660 ns
  // of the 6,371 ns p99 cost lands inside it. Two costs, two places, two causes.
  //
  // This is the fix for the first one. --dual-path submits both copies inline, so copy B of
  // datagram N-1 sits ahead of copy A of datagram N in one shared transmit queue and every
  // real message waits behind a redundant one. Opportunistic duplication already solved
  // exactly that scheduling problem -- it sends only from the branch where the source ring
  // was empty -- but sent on the *same* sockets, so its copies shared a four-tuple with the
  // original and died with it.
  //
  // Neither mechanism was wrong; each had the other's missing half. A dedicated leg with
  // its own sockets, written to only when nothing is waiting, has both.
  bool dup_path = false;
  // The leg is a separate object, so it can be a different backend from the primary --
  // and it should be. io_uring and AF_XDP were both rejected for the primary path on
  // grounds that do not apply to a purely redundant one:
  //
  //   AF_XDP is 4.6 us worse end to end, which does not matter on a leg whose only job is
  //   to beat a millisecond-scale drain, and it transmits via dev_direct_xmit -- no qdisc,
  //   its own bound transmit queue -- so it shares no software or hardware queue with the
  //   primary. That is precisely the resource whose sharing costs the median.
  //
  //   io_uring with SQPOLL removes the last system call from this thread. Its one known
  //   defect, an unexplained drop burst, is harmless here: a dropped redundant copy costs
  //   nothing, because the original is still on its way.
  Backend dup_backend = Backend::kUdp;
  uint16_t dup_port = 0;      // 0 = the same destination port as the primary
  bool dup_sqpoll = false;    // io_uring leg only
  int dup_sq_core = -1;
  uint32_t dup_xdp_queue = 1;  // a *different* transmit queue from the primary's
  uint16_t dup_src_port = 50001;
};

struct Stats {
  uint64_t frames = 0;
  uint64_t datagrams = 0;      // distinct datagrams, not counting copies
  uint64_t duplicates = 0;     // redundant copies actually sent
  uint64_t dup_skipped = 0;    // copies dropped because new data arrived first
  uint64_t lapped = 0;         // we fell behind the producer's ring
  uint64_t send_fail = 0;      // datagrams that reached no peer at all
  uint64_t partial = 0;        // datagrams that reached some but not all peers
  uint64_t dup_leg_sent = 0;   // redundant copies that went out the redundant leg
  uint64_t dup_leg_fail = 0;   // and ones it could not place, which cost us nothing
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
    else if (a == "--backend") {
      const std::string b = next();
      if (!parse_backend(b, &c.backend)) {
        fprintf(stderr, "--backend must be udp|iouring\n");
        std::exit(2);
      }
    }
    else if (a == "--sqpoll") c.sqpoll = true;
    else if (a == "--sq-core") c.sq_core = std::stoi(next());
    else if (a == "--xdp-iface") c.xdp_iface = next();
    else if (a == "--xdp-queue") c.xdp_queue = static_cast<uint32_t>(std::stoul(next()));
    else if (a == "--xdp-udp-csum") c.xdp_udp_csum = true;
    else if (a == "--tx-tstamp") c.tx_timestamp = true;
    else if (a == "--src-addr") c.src_addr = next();
    else if (a == "--dual-path") c.dual_path = true;
    else if (a == "--dup-path") c.dup_path = true;
    else if (a == "--dup-backend") {
      const std::string b = next();
      if (!parse_backend(b, &c.dup_backend)) {
        fprintf(stderr, "--dup-backend must be udp|iouring|xdp\n");
        std::exit(2);
      }
    }
    else if (a == "--dup-port") c.dup_port = static_cast<uint16_t>(std::stoul(next()));
    else if (a == "--dup-sqpoll") c.dup_sqpoll = true;
    else if (a == "--dup-sq-core") c.dup_sq_core = std::stoi(next());
    else if (a == "--dup-xdp-queue") c.dup_xdp_queue = static_cast<uint32_t>(std::stoul(next()));
    else if (a == "--dup-src-port") c.dup_src_port = static_cast<uint16_t>(std::stoul(next()));
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
  // Both flags add a second copy of every datagram, by different means and with opposite
  // costs. Running them together would send four and attribute the result to neither.
  if (c.dual_path && c.dup_path) {
    fprintf(stderr, "--dual-path and --dup-path are alternatives, not additions: "
                    "the first sends both copies inline, the second sends the copy "
                    "only into idle time\n");
    std::exit(2);
  }
  if (c.dup_path && c.dup_backend == Backend::kXdp && c.xdp_iface.empty()) {
    fprintf(stderr, "--dup-backend xdp needs --xdp-iface: transmitting a frame means "
                    "choosing which wire it leaves by\n");
    std::exit(2);
  }
  // A redundant leg that shares the primary's transmit queue removes the point of it.
  if (c.dup_path && c.dup_backend == Backend::kXdp &&
      c.backend == Backend::kXdp && c.dup_xdp_queue == c.xdp_queue) {
    fprintf(stderr, "--dup-xdp-queue must differ from --xdp-queue, or the redundant leg "
                    "shares the queue whose sharing it exists to avoid\n");
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

// Percentiles of the sending kernel's transmit path, if it was measured.
void report_tx_stack(const std::vector<uint32_t>& v, uint64_t unmatched,
                     uint64_t expired) {
  if (v.empty()) {
    fprintf(stderr, "tx stack        : no samples "
                    "(unmatched %llu, expired %llu)\n",
            (unsigned long long)unmatched, (unsigned long long)expired);
    return;
  }
  std::vector<uint32_t> s = v;
  std::sort(s.begin(), s.end());
  const auto at = [&s](double q) {
    return s[static_cast<size_t>(q * static_cast<double>(s.size() - 1))];
  };
  fprintf(stderr,
          "---- userspace -> kernel transmit stamp (one clock, exact) ----\n"
          "samples=%zu p50=%u p99=%u p99.9=%u p99.99=%u max=%u  "
          "(unmatched %llu, expired %llu)\n",
          s.size(), at(0.50), at(0.99), at(0.999), at(0.9999), s.back(),
          (unsigned long long)unmatched, (unsigned long long)expired);
}

inline void report_tx_stamps(udp::Sender& s) {
  if (s.tx_timestamp()) {
    s.drain_tx_stamps();
    report_tx_stack(s.tx_stack_samples(), s.tx_unmatched(), s.tx_expired());
  }
}
inline void report_tx_stamps(iou::Sender&) {}
inline void report_tx_stamps(xdp::Sender&) {}

// Transmit timestamping exists only on the kernel-UDP path: the stamp arrives on the
// socket's error queue, which io_uring's submission ring and AF_XDP's frame rings do not
// have. Overload resolution rather than a virtual, to keep the relay loop free of
// indirection, and the non-UDP overloads do nothing rather than pretend.
inline void note_send_if_stamping(udp::Sender& s, uint64_t t) { s.note_send(t); }
inline void note_send_if_stamping(iou::Sender&, uint64_t) {}
inline void note_send_if_stamping(xdp::Sender&, uint64_t) {}
inline void drain_tx_stamps_if_stamping(udp::Sender& s) { s.drain_tx_stamps(); }
inline void drain_tx_stamps_if_stamping(iou::Sender&) {}
inline void drain_tx_stamps_if_stamping(xdp::Sender&) {}

// The redundant leg, behind the one virtual call in this program.
//
// Indirection is acceptable here and nowhere else on the send path, because send() is only
// ever reached from the branch in which the source ring was empty -- by construction
// nothing is waiting on it. The alternative was instantiating run_relay once per
// (primary, redundant) backend pair, which is nine copies of the relay loop to save an
// indirect call in the one place where the loop has time to spare.
struct DupLeg {
  virtual ~DupLeg() = default;
  virtual int send(const void* buf, uint32_t len) = 0;
  virtual size_t peer_count() const = 0;
  virtual const char* backend_name() const = 0;
  // Backend-specific counters worth seeing separately: a redundant copy that never
  // reached the wire is not a lost message, and should not be reported as one.
  virtual void report() const {}
};

template <class NetT>
class DupLegOf final : public DupLeg {
 public:
  explicit DupLegOf(const char* name) : name_(name) {}
  NetT& net() { return net_; }
  int send(const void* buf, uint32_t len) override { return net_.send_all(buf, len); }
  size_t peer_count() const override { return net_.peer_count(); }
  const char* backend_name() const override { return name_; }
  // Specialised below for the two backends that have counters of their own; the plain
  // socket leg has nothing to add beyond what the shared counters already say.
  void report() const override {}

 private:
  NetT net_;
  const char* name_;
};

// AF_XDP counts two failure modes the socket backends cannot have, and on a redundant leg
// both are survivable rather than fatal -- which is exactly why this leg is a good home
// for a backend that has them.
template <>
void DupLegOf<xdp::Sender>::report() const {
  fprintf(stderr, "dup leg afxdp   : kicks %llu, pool stalls %llu, completion errors %llu\n",
          (unsigned long long)net_.kicks(), (unsigned long long)net_.pool_stalls(),
          (unsigned long long)net_.completion_errors());
}
template <>
void DupLegOf<iou::Sender>::report() const {
  fprintf(stderr, "dup leg io_uring: completion errors %llu\n",
          (unsigned long long)net_.completion_errors());
}

// Open the redundant leg, or return nullptr if none was asked for. *ok is set false only
// when one was asked for and could not be opened, so a configuration error is never
// mistaken for "redundancy was off".
std::unique_ptr<DupLeg> open_dup_leg(const Config& cfg, bool* ok) {
  *ok = true;
  if (!cfg.dup_path) return nullptr;
  const uint16_t port = cfg.dup_port != 0 ? cfg.dup_port : cfg.port;

  switch (cfg.dup_backend) {
    case Backend::kXdp: {
      xdp::Options o;
      o.sock.src_addr = cfg.src_addr;
      o.ifname = cfg.xdp_iface;
      o.queue_id = cfg.dup_xdp_queue;
      o.udp_checksum = cfg.xdp_udp_csum;
      // AF_XDP writes its own headers, so its source port is chosen rather than assigned.
      // It has to differ from whatever the primary is using or there is no second
      // four-tuple and the leg is pointless.
      o.src_port = cfg.dup_src_port;
      auto leg = std::make_unique<DupLegOf<xdp::Sender>>("afxdp");
      if (!leg->net().open(cfg.peers, port, o)) { *ok = false; return nullptr; }
      return leg;
    }
    case Backend::kIoUring: {
      iou::Options o;
      o.sock.src_addr = cfg.src_addr;
      o.sqpoll = cfg.dup_sqpoll;
      o.sq_cpu = cfg.dup_sq_core;
      auto leg = std::make_unique<DupLegOf<iou::Sender>>(
          cfg.dup_sqpoll ? "io_uring+sqpoll" : "io_uring");
      if (!leg->net().open(cfg.peers, port, o)) { *ok = false; return nullptr; }
      return leg;
    }
    default: {
      udp::Options o;
      o.src_addr = cfg.src_addr;
      auto leg = std::make_unique<DupLegOf<udp::Sender>>("udp");
      // Separate sockets to the same destination, so connect() assigns them ephemeral
      // source ports of their own. That is the entire four-tuple diversity mechanism:
      // nothing is configured, the copies simply cannot share a tuple with the primary.
      if (!leg->net().open(cfg.peers, port, o)) { *ok = false; return nullptr; }
      return leg;
    }
  }
}

// The relay itself, written once against whichever backend was selected. Both expose
// peer_count()/send_all()/method_name() with identical semantics -- in particular
// send_all() has finished with the buffer by the time it returns -- so nothing in here
// needs to know which one it is driving. Chosen once at startup, so the dispatch costs
// a template instantiation rather than an indirect call per datagram.
template <class SenderT>
int run_relay(SenderT& net, DupLeg* dup, const Config& cfg) {
  shm::Segment seg = shm::Segment::open(cfg.shm_name,
                                        shm::region_size(cfg.slots),
                                        /*create=*/false);
  shm::Ring ring;
  ring.attach(seg.base(), cfg.slots, /*init=*/false);

  fprintf(stderr,
          "sender: shm=%s slots=%u peers=%zu datagram=%u core=%d send=%s\n",
          cfg.shm_name.c_str(), cfg.slots, net.peer_count(), cfg.datagram,
          cfg.core, net.method_name());
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
        if (dup != nullptr) {
          // Deliberately *not* restamped, unlike the same-socket copy below.
          //
          // Restamping would make the latency of a winning copy read from its own
          // departure rather than the original's, quietly subtracting the stagger from
          // the wire leg -- flattering exactly the number this leg exists to improve.
          // Keeping the original stamp means a copy that wins reports the true age of
          // the message, stagger included.
          if (dup->send(dup_buf, dup_len) > 0) ++stats.dup_leg_sent;
          else ++stats.dup_leg_fail;
        } else {
          wire::stamp_send_ts(dup_buf, util::now_ns());
          net.send_all(dup_buf, dup_len);
        }
        ++stats.duplicates;
        dup_buf = nullptr;
      }
      // Collecting transmit stamps belongs here for the same reason the redundant copy
      // does: the ring is empty, so nothing is waiting on us.
      drain_tx_stamps_if_stamping(net);
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

    const uint64_t presend_ns = util::now_ns();
    wire::stamp_send_ts(const_cast<uint8_t*>(dgram), presend_ns);
    const int reached = net.send_all(dgram, dlen);
    note_send_if_stamping(net, presend_ns);

    ++pkt_seq;
    ++stats.datagrams;
    stats.frames += packer.count();
    if (reached == 0) ++stats.send_fail;
    else if (static_cast<size_t>(reached) < net.peer_count()) ++stats.partial;
    last_activity = util::now_ns();

    // Hand this datagram to the duplication slot and switch buffers, so filling the
    // next one cannot overwrite the bytes the copy will be sent from.
    if (cfg.duplicate || cfg.dup_path) {
      dup_buf = pktbuf[cur].data();
      dup_len = dlen;
      cur ^= 1;
    }
  }

  print_stats(stats);
  if (dup != nullptr) {
    fprintf(stderr, "---- redundant leg (%s, %zu destinations, idle time only) ----\n",
            dup->backend_name(), dup->peer_count());
    fprintf(stderr, "dup leg sent    : %llu\n", (unsigned long long)stats.dup_leg_sent);
    // Not counted as a send failure: the original went out on the primary leg regardless,
    // so a copy that never made it costs redundancy and not delivery.
    fprintf(stderr, "dup leg unplaced: %llu (original was sent anyway)\n",
            (unsigned long long)stats.dup_leg_fail);
    dup->report();
  }
  report_tx_stamps(net);
  // Asynchronous submission can fail after send_all() has already counted a datagram
  // as sent, so report it separately rather than letting it hide inside send_fail.
  if (net.completion_errors() != 0) {
    fprintf(stderr, "completion errs : %llu (reported after submission)\n",
            (unsigned long long)net.completion_errors());
  }
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

}  // namespace

int main(int argc, char** argv) {
  Config cfg = parse_args(argc, argv);
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  if (cfg.core >= 0 && !cpu::pin_to_core(cfg.core)) return 1;

  // Dual path is expressed by listing every destination twice.
  //
  // The two copies go to the same destination port, so the receiver needs no change at all
  // -- not one extra socket, and not a second thread to poll it. What differs is the
  // *source* port: connect() on an unbound datagram socket binds an ephemeral one, and two
  // sockets to the same destination necessarily get different ones. That makes two distinct
  // four-tuples, which is what receive-side steering and any equal-cost path selection in
  // the fabric hash on, so the two copies can diverge where it matters while arriving at
  // one socket where it does not.
  // The redundant leg is opened before the primary so that a misconfiguration fails here
  // rather than after the relay has already started moving messages.
  bool dup_ok = true;
  std::unique_ptr<DupLeg> dup = open_dup_leg(cfg, &dup_ok);
  if (!dup_ok) {
    fprintf(stderr, "sender: redundant leg failed to open\n");
    return 1;
  }
  if (dup) {
    fprintf(stderr, "sender: redundant leg on %s, %zu destinations, port %u, "
                    "written only when the source ring is empty\n",
            dup->backend_name(), dup->peer_count(),
            (unsigned)(cfg.dup_port != 0 ? cfg.dup_port : cfg.port));
  }

  std::vector<std::string> peers = cfg.peers;
  if (cfg.dual_path) {
    const size_t original = peers.size();
    peers.insert(peers.end(), cfg.peers.begin(), cfg.peers.end());
    fprintf(stderr, "sender: dual path, %zu destinations sent twice from distinct "
                    "source ports\n", original);
  }

  if (cfg.backend == Backend::kXdp) {
    if (!cfg.src_addr.empty()) {
      // AF_XDP writes its own IP header from the interface's address, so it cannot honour
      // a chosen source. Rejected rather than ignored: on a policy-routed path the source
      // address is what selects the uplink, so ignoring it would measure another link.
      fprintf(stderr, "--src-addr is not supported with --backend xdp: the frame's source "
                      "comes from the interface\n");
      return 2;
    }
    xdp::Options opts;
    opts.ifname = cfg.xdp_iface;
    opts.queue_id = cfg.xdp_queue;
    opts.udp_checksum = cfg.xdp_udp_csum;
    xdp::Sender net;
    if (!net.open(peers, cfg.port, opts)) return 1;
    const int rc = run_relay(net, dup.get(), cfg);
    // The kick count is the actual system call count for the run, and a pool stall means
    // a datagram was not sent at all. Neither should be inferred from the latency.
    fprintf(stderr, "afxdp kicks     : %llu (one per fan-out batch)\n",
            (unsigned long long)net.kicks());
    if (net.pool_stalls() != 0) {
      fprintf(stderr, "afxdp pool stalls: %llu (datagrams dropped before the wire)\n",
              (unsigned long long)net.pool_stalls());
    }
    return rc;
  }

  if (cfg.backend == Backend::kIoUring) {
    iou::Options opts;
    opts.sock.src_addr = cfg.src_addr;
    opts.sqpoll = cfg.sqpoll;
    opts.sq_cpu = cfg.sq_core;
    iou::Sender net;
    if (!net.open(peers, cfg.port, opts)) return 1;
    return run_relay(net, dup.get(), cfg);
  }

  // AF_XDP is now legitimately usable without being the primary backend: a kernel-UDP
  // primary with an AF_XDP redundant leg is the configuration in which its own transmit
  // queue is worth having, so the interface is required there too rather than rejected.
  if (!cfg.xdp_iface.empty() && !(cfg.dup_path && cfg.dup_backend == Backend::kXdp)) {
    fprintf(stderr, "--xdp-iface/--xdp-queue apply only to --backend xdp "
                    "or --dup-backend xdp\n");
    return 2;
  }
  if (cfg.sqpoll || cfg.sq_core >= 0) {
    // Silently ignoring these would make a mistyped command look like a real result.
    fprintf(stderr, "--sqpoll/--sq-core apply only to --backend iouring\n");
    return 2;
  }
  udp::Options opts;
  opts.src_addr = cfg.src_addr;
  udp::Sender net;
  if (!net.open(peers, cfg.port, opts, cfg.method)) return 1;
  if (cfg.tx_timestamp && !net.enable_tx_timestamps()) {
    fprintf(stderr, "--tx-tstamp requested but unavailable\n");
  }
  return run_relay(net, dup.get(), cfg);
}
