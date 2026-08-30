// Pin the calling thread to one core.
//
// Both the sender and the receiver busy-spin, which only pays off if the
// spinning thread owns a physical core that nothing else is scheduled on. Without
// that, the tail is dominated by the scheduler moving the thread and by whatever
// shares the core -- not by the transport. See harness/README.md for the
// boot-time isolcpus/nohz_full/rcu_nocbs setup this assumes.
#pragma once

#include <sched.h>

#include <cstdio>

namespace cpu {

// Returns false and explains itself on stderr if the core is not available;
// callers treat that as fatal, since an unpinned measurement is not worth taking.
inline bool pin_to_core(int core) {
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(core, &set);
  if (sched_setaffinity(0, sizeof(set), &set) != 0) {
    fprintf(stderr, "pin to core %d failed (is it online and permitted?)\n",
            core);
    return false;
  }
  return true;
}

}  // namespace cpu
