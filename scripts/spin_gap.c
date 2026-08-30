// Does an isolated core actually get uninterrupted CPU on this instance?
//
// Every explanation offered so far for the 1-3 ms stall lives inside the relay: the
// receive path, the copy stream, the scheduler. None of them was tested against the
// simplest alternative -- that the host takes the core away. This is an m7i.2xlarge, a
// shared instance, not the bare-metal sender the baseline was measured on.
//
// So: spin on an isolated core doing nothing at all, and record the gap between successive
// iterations. If a thread whose only instruction is a clock read also sees millisecond
// gaps, the stall is not the relay's.
#define _GNU_SOURCE
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static long long now(void) {
  struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1000000000LL + t.tv_nsec;
}
static int cmp(const void *a, const void *b) {
  long long x = *(const long long *)a, y = *(const long long *)b;
  return (x > y) - (x < y);
}
int main(int argc, char **argv) {
  int core = argc > 1 ? atoi(argv[1]) : 3;
  int secs = argc > 2 ? atoi(argv[2]) : 60;
  cpu_set_t s; CPU_ZERO(&s); CPU_SET(core, &s);
  if (sched_setaffinity(0, sizeof(s), &s)) { perror("affinity"); return 1; }

  long long cap = 40000000, n = 0, *g = malloc(cap * sizeof(long long));
  long long t0 = now(), prev = t0, deadline = t0 + (long long)secs * 1000000000LL, over = 0, worst = 0;
  while (prev < deadline) {
    long long t = now(), d = t - prev;
    if (n < cap) g[n++] = d;
    if (d > 1000000) ++over;
    if (d > worst) worst = d;
    prev = t;
  }
  qsort(g, n, sizeof(long long), cmp);
  printf("core %d, %d s, %lld samples\n", core, secs, n);
  printf("  gap p50=%lld p99=%lld p99.9=%lld p99.99=%lld p99.999=%lld max=%lld ns\n",
         g[n/2], g[n*99/100], g[n*999/1000], g[n*9999/10000], g[n*99999/100000], worst);
  printf("  gaps over 1 ms: %lld\n", over);
  return 0;
}
