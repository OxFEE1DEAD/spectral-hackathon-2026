// Consumer: reads events from the shared-memory broadcast ring, stamps a receive
// timestamp, and computes delivery metrics (latency percentiles, drop rate) from
// the per-message send timestamp and sequence id. Fixed measurement end of the
// harness.
//
// Usage: consumer [--shm NAME] [--slots N] [--count N] [--from-edge]
//                 [--csv FILE] [--idle-ms MS]
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "message.h"
#include "metrics.h"
#include "shm_ring.h"
#include "shm_segment.h"
#include "util.h"

namespace {

struct Config {
  std::string shm_name = "/fanout_ring";
  uint32_t slots = 1024;
  uint64_t count = 0;
  bool from_edge = false;
  std::string csv;
  uint64_t idle_ms = 2000;
};

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
    else if (a == "--count") c.count = std::stoull(next());
    else if (a == "--from-edge") c.from_edge = true;
    else if (a == "--csv") c.csv = next();
    else if (a == "--idle-ms") c.idle_ms = std::stoull(next());
    else {
      fprintf(stderr, "unknown arg: %s\n", a.c_str());
      std::exit(2);
    }
  }
  return c;
}

void print_report(const metrics::Report& r) {
  printf("---- delivery metrics ----\n");
  printf("received     : %llu\n", (unsigned long long)r.received);
  printf("expected     : %llu\n", (unsigned long long)r.expected);
  printf("dropped      : %llu\n", (unsigned long long)r.dropped);
  printf("drop_rate    : %.4f%%\n", r.drop_rate * 100.0);
  if (r.overflow) {
    // Samples the buffer could not hold. Percentiles then describe a prefix of the
    // run rather than all of it, so say so loudly instead of quietly biasing them.
    printf("NOT STORED   : %llu (raise --count; percentiles cover a prefix only)\n",
           (unsigned long long)r.overflow);
  }
  printf("latency (ns) : min=%llu mean=%.0f max=%llu\n",
         (unsigned long long)r.lat_min, r.lat_mean,
         (unsigned long long)r.lat_max);
  printf("  p50        : %llu\n", (unsigned long long)r.p50);
  printf("  p90        : %llu\n", (unsigned long long)r.p90);
  printf("  p99        : %llu\n", (unsigned long long)r.p99);
  printf("  p99.9      : %llu\n", (unsigned long long)r.p999);
  printf("  p99.99     : %llu\n", (unsigned long long)r.p9999);
  printf("  p99.999    : %llu\n", (unsigned long long)r.p99999);
}

}  // namespace

int main(int argc, char** argv) {
  Config cfg = parse_args(argc, argv);

  shm::Segment seg =
      shm::Segment::open(cfg.shm_name, shm::region_size(cfg.slots), /*create=*/false);
  shm::Ring ring;
  ring.attach(seg.base(), cfg.slots, /*init=*/false);

  // Reserved once, up front. Sized to the requested run length so nothing is
  // dropped and nothing is allocated mid-measurement.
  metrics::Accumulator acc(cfg.count ? cfg.count : 1u << 22);

  uint64_t read_index = cfg.from_edge ? ring.live_edge() : 0;
  uint64_t received = 0;
  uint64_t lapped_events = 0;
  const uint64_t idle_ns = cfg.idle_ms * 1000000ull;
  uint64_t last_progress = util::now_ns();

  uint8_t frame[shm::kFrameCap];
  while (cfg.count == 0 || received < cfg.count) {
    uint32_t len = 0;
    uint64_t resume = 0;
    auto st = ring.read(read_index, frame, &len, &resume);

    if (st == shm::Ring::FrameStatus::kOk) {
      const uint64_t recv_ts = util::now_ns();
      const auto* hdr = reinterpret_cast<const msg::Header*>(frame);
      const uint64_t latency =
          recv_ts > hdr->send_ts_ns ? recv_ts - hdr->send_ts_ns : 0;
      acc.record(hdr->seq_id, latency);
      ++received;
      ++read_index;
      last_progress = recv_ts;
    } else if (st == shm::Ring::FrameStatus::kLapped) {
      ++lapped_events;
      read_index = resume;  // skip the gap; drops show up as seq gaps in metrics
    } else {  // kEmpty
      if (util::now_ns() - last_progress > idle_ns) break;  // producer done
    }
  }

  fprintf(stderr, "consumer: lapped %llu times\n",
          (unsigned long long)lapped_events);
  print_report(acc.report());
  // The only time samples touch the filesystem, and the run is already over.
  if (!cfg.csv.empty() && !acc.dump_csv(cfg.csv)) {
    fprintf(stderr, "consumer: failed to write %s\n", cfg.csv.c_str());
    return 1;
  }
  return 0;
}
