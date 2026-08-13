// Minimal assertion helpers, matching the style of harness/test/test_harness.cpp:
// no framework, no dependencies, non-zero exit on failure.
#pragma once

#include <cstdio>
#include <cstdlib>

namespace check {
inline int failures = 0;
inline int checks = 0;
}  // namespace check

#define CHECK(cond)                                                    \
  do {                                                                 \
    ++check::checks;                                                   \
    if (!(cond)) {                                                     \
      ++check::failures;                                               \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,     \
                   #cond);                                             \
    }                                                                  \
  } while (0)

#define CHECK_EQ(a, b)                                                       \
  do {                                                                       \
    ++check::checks;                                                          \
    const auto _a = (a);                                                      \
    const auto _b = (b);                                                      \
    if (!(_a == _b)) {                                                        \
      ++check::failures;                                                      \
      std::fprintf(stderr, "FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, \
                   __LINE__, #a, #b, static_cast<long long>(_a),              \
                   static_cast<long long>(_b));                               \
    }                                                                         \
  } while (0)

inline int check_report(const char* name) {
  if (check::failures == 0) {
    std::printf("ok  %-14s %d checks\n", name, check::checks);
    return 0;
  }
  std::printf("FAIL %-13s %d/%d failed\n", name, check::failures, check::checks);
  return 1;
}
