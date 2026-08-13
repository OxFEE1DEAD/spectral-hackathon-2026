// Cheap monotonic tick counter, plus a fit from ticks to CLOCK_REALTIME.
//
// Stage timestamps inside sender/receiver are taken in raw ticks because
// rdtscp is ~8-10 ns while a vDSO clock_gettime is ~18-25 ns and silently
// degrades to a real syscall when the clocksource is not TSC. Ticks are
// converted to realtime once, offline, so the hot path never pays for it.
//
// The fit exists because two of our stages are *not* in the tick domain: the
// producer's send_ts_ns (CLOCK_REALTIME) and the NIC's hardware RX timestamp
// (PHC domain). Joining them requires a common frame of reference.
//
// Architecture note: the target is x86_64 (m7i), where the counter is the TSC.
// Local development runs on aarch64, where it is the ARM generic timer. The
// abstraction is deliberately present from day one rather than bolted on
// later. See plans/00_TRADEOFFS.txt D-08.
#pragma once

#include <cstdint>
#include <ctime>

namespace tsc {

#if defined(__x86_64__) || defined(__i386__)

// TSC. Requires constant_tsc + nonstop_tsc, which every instance we target
// has -- bench/env_check.sh asserts it rather than assuming it.
inline uint64_t ticks() {
  uint32_t lo, hi, aux;
  __asm__ __volatile__("rdtscp" : "=a"(lo), "=d"(hi), "=c"(aux));
  return (static_cast<uint64_t>(hi) << 32) | lo;
}

// x86 exposes no architectural register for the TSC frequency, so it has to
// be measured against a known clock.
inline constexpr bool kHasNativeFrequency = false;
inline uint64_t native_frequency_hz() { return 0; }

#elif defined(__aarch64__)

// ARM generic timer. The isb is needed because mrs on cntvct_el0 is not
// ordered against surrounding loads/stores on its own.
inline uint64_t ticks() {
  uint64_t v;
  __asm__ __volatile__("isb; mrs %0, cntvct_el0" : "=r"(v));
  return v;
}

// Unlike x86, the frequency is architectural and exact.
inline constexpr bool kHasNativeFrequency = true;
inline uint64_t native_frequency_hz() {
  uint64_t f;
  __asm__ __volatile__("mrs %0, cntfrq_el0" : "=r"(f));
  return f;
}

#else

// Portable fallback: ticks are simply nanoseconds.
inline uint64_t ticks() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull +
         static_cast<uint64_t>(ts.tv_nsec);
}
inline constexpr bool kHasNativeFrequency = true;
inline uint64_t native_frequency_hz() { return 1000000000ull; }

#endif

inline uint64_t realtime_ns() {
  timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull +
         static_cast<uint64_t>(ts.tv_nsec);
}

// One (tick, realtime) pair sampled with the tightest bracket we could get.
struct Anchor {
  uint64_t tick;
  uint64_t realtime_ns;
  uint64_t bracket_ticks;  // width of the rdtscp..rdtscp window; smaller is better
};

// Sample clock_gettime bracketed by two tick reads, keeping the pair whose
// bracket was narrowest. The narrow bracket is what removes most of the
// sampling noise -- a wide one means we were interrupted mid-read.
inline Anchor sample_anchor(unsigned tries = 64) {
  Anchor best{0, 0, UINT64_MAX};
  for (unsigned i = 0; i < tries; ++i) {
    const uint64_t t0 = ticks();
    const uint64_t rt = realtime_ns();
    const uint64_t t1 = ticks();
    if (t1 < t0) continue;  // counter went backwards; discard
    const uint64_t bracket = t1 - t0;
    if (bracket < best.bracket_ticks) {
      best.tick = t0 + bracket / 2;
      best.realtime_ns = rt;
      best.bracket_ticks = bracket;
    }
  }
  return best;
}

// Linear map ticks -> CLOCK_REALTIME nanoseconds.
class Calibration {
 public:
  // Two anchors separated by `baseline_ms` give the slope; the later anchor is
  // the offset. A longer baseline lowers the slope error but costs startup
  // time, so callers refit periodically instead of calibrating once for a long
  // time (see plans/06_INSTRUMENTATION.txt).
  static Calibration measure(unsigned baseline_ms = 20) {
    const Anchor a = sample_anchor();
    timespec req{0, static_cast<long>(baseline_ms) * 1000000L};
    while (nanosleep(&req, &req) != 0 && req.tv_nsec > 0) {
    }
    const Anchor b = sample_anchor();

    Calibration c;
    const uint64_t dt = b.tick - a.tick;
    const uint64_t drt = b.realtime_ns - a.realtime_ns;
    if (dt == 0) {
      c.ns_per_tick_ = kHasNativeFrequency
                           ? 1e9 / static_cast<double>(native_frequency_hz())
                           : 1.0;
    } else {
      c.ns_per_tick_ = static_cast<double>(drt) / static_cast<double>(dt);
    }
    c.base_tick_ = b.tick;
    c.base_realtime_ns_ = b.realtime_ns;
    return c;
  }

  uint64_t to_realtime_ns(uint64_t tick) const {
    const double delta =
        (static_cast<double>(tick) - static_cast<double>(base_tick_)) *
        ns_per_tick_;
    return static_cast<uint64_t>(static_cast<double>(base_realtime_ns_) + delta);
  }

  uint64_t ticks_to_ns(uint64_t delta_ticks) const {
    return static_cast<uint64_t>(static_cast<double>(delta_ticks) * ns_per_tick_);
  }

  double ns_per_tick() const { return ns_per_tick_; }
  double hz() const { return 1e9 / ns_per_tick_; }

 private:
  double ns_per_tick_ = 1.0;
  uint64_t base_tick_ = 0;
  uint64_t base_realtime_ns_ = 0;
};

}  // namespace tsc
