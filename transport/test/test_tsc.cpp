// Tick counter and its fit to CLOCK_REALTIME.
//
// This does not validate accuracy -- that needs a disciplined PHC and belongs
// on the AWS bench (plans/02_CLOCK_FOUNDATION.txt). What it does check is that
// the counter is monotonic, that the calibration is not wildly wrong, and that
// converting a measured interval lands within a few percent of the same
// interval measured by the kernel.
#include "tsc.h"

#include <cstdint>
#include <ctime>

#include "check.h"

namespace {

void test_monotonic() {
  uint64_t prev = tsc::ticks();
  for (int i = 0; i < 100000; ++i) {
    const uint64_t now = tsc::ticks();
    CHECK(now >= prev);
    prev = now;
  }
}

void test_anchor() {
  const tsc::Anchor a = tsc::sample_anchor();
  CHECK(a.tick != 0);
  CHECK(a.realtime_ns > 1600000000ull * 1000000000ull);  // after 2020
  CHECK(a.bracket_ticks != UINT64_MAX);
}

void test_calibration() {
  const tsc::Calibration c = tsc::Calibration::measure(20);
  CHECK(c.ns_per_tick() > 0.0);
  // Any plausible counter sits between 1 MHz and 10 GHz.
  CHECK(c.hz() > 1e6 && c.hz() < 1e10);

  if (tsc::kHasNativeFrequency) {
    // Where the architecture publishes the frequency, the measured fit must
    // agree with it; a mismatch means the calibration itself is broken.
    const double native = static_cast<double>(tsc::native_frequency_hz());
    CHECK(c.hz() > native * 0.98 && c.hz() < native * 1.02);
  }

  // A measured interval, converted through the fit, should match what the
  // kernel says the same interval was.
  const uint64_t t0 = tsc::ticks();
  const uint64_t r0 = tsc::realtime_ns();
  timespec req{0, 50 * 1000 * 1000};  // 50 ms
  while (nanosleep(&req, &req) != 0 && req.tv_nsec > 0) {
  }
  const uint64_t t1 = tsc::ticks();
  const uint64_t r1 = tsc::realtime_ns();

  const uint64_t via_ticks = c.ticks_to_ns(t1 - t0);
  const uint64_t via_kernel = r1 - r0;
  const double err = (static_cast<double>(via_ticks) -
                      static_cast<double>(via_kernel)) /
                     static_cast<double>(via_kernel);
  CHECK(err > -0.02 && err < 0.02);

  // Absolute conversion should land near the realtime clock. The tolerance is
  // loose on purpose: a VM's counter is coarse, and this test only exists to
  // catch a fit that is off by orders of magnitude.
  const uint64_t mapped = c.to_realtime_ns(t1);
  const int64_t drift = static_cast<int64_t>(mapped) - static_cast<int64_t>(r1);
  CHECK(drift > -5000000 && drift < 5000000);  // within 5 ms
}

}  // namespace

int main() {
  test_monotonic();
  test_anchor();
  test_calibration();
  std::printf("     counter: %.3f MHz, %.2f ns/tick%s\n",
              tsc::Calibration::measure(20).hz() / 1e6,
              tsc::Calibration::measure(20).ns_per_tick(),
              tsc::kHasNativeFrequency ? " (architectural)" : " (measured)");
  return check_report("tsc");
}
