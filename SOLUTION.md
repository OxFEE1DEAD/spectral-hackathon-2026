# Low-latency fan-out transport — solution description

A shared-memory-to-shared-memory relay: the producer publishes into a local ring,
a **sender** reads that ring and puts events on the wire, a **receiver** on each
destination host republishes them into a local ring, and one or more consumers read
that ring and measure delivery latency.

```
producer ──shm ring──> sender ──UDP──> receiver ──shm ring──> consumer(s)
 (given)                (ours)          (ours)                 (given)
```

Everything below is a design decision with a reason and, where possible, a
measurement. Sections that are still open are marked as such rather than left to
look finished.

The experiments that went looking for the remaining latency -- including the ones that
failed, and a clock-offset finding that bounds how precisely any cross-host figure here can
be read -- are in a companion document: **[LATENCY_INVESTIGATION.md](LATENCY_INVESTIGATION.md)**.

## Status at a glance

Most of the work behind this document was negative, and that is the useful part: the shipping
configuration is the measured winner rather than the cautious default.

**What ships.** Kernel UDP at both ends with `SO_BUSY_POLL` on the receive side, opportunistic
batching, opportunistic redundancy, and a strictly-monotonic delivery gate.

**Established, and reproduced across sessions, rates and two host pairs.**

- Our own three stages cost about **620 ns** end to end. The wire leg is everything else.
- The delivery gate is safe on a routed path: **zero reordering in 1,549,105 datagrams**, so the
  strictness that buys deduplication and gap accounting in one rule costs nothing.
- MTU 1500 holds end to end on both paths, so nothing fragments.
- Fan-out is packet-rate bound, not latency bound: about **840k datagrams/second**.
- No alternative submission mechanism beats kernel UDP end to end. io_uring's receive path is 69%
  worse at the median; AF_XDP cuts the send cycle 37% -- the largest saving measured -- and gives
  all of it back in the wire leg, ending 4.6 µs worse.

**Measured and negative -- worth reading before repeating the work.**

- Redundancy buys nothing against the loss either path shows, because loss arrives in bursts far
  longer than the gap between a datagram and its copy. On the routed path, six interleaved blocks
  resolve it as a small net **cost** (+170 undelivered frames, sign p = 0.031).
- Splitting the receiver into polling and publishing threads refutes its own premise.
- Path diversity's apparent tail win **did not replicate**. That retraction is kept deliberately:
  it is why the block design exists, and removing it would make the far-tail figures look better
  supported than they are.

**Bounded, not resolved -- with the bound stated rather than the number guessed.**

- The 32.8 µs between the sending and receiving kernels is one opaque number. Every timestamp here is
  a software timestamp and both software routes into that leg are exhausted; splitting it further
  needs hardware timestamping.
- Loss at the 1-10% the task contemplates: the two available paths span 2.2e-05 to 1.6e-03, so that
  regime has to be induced rather than found.
- How finely anything can be compared on a long-haul path: the reference arm's own median moves
  **1.1 ms between blocks** there, so smaller effects must come from a single-clock metric instead.

**The measurement itself was audited, and it changed published numbers.** Percentiles were being
computed as the median of per-file percentiles, which discards exactly the files holding the rare
events: it understated far tails by up to **719%** and one maximum by **115×**. Medians and p99s
did not move. Comparisons now run as counterbalanced interleaved blocks with a paired sign test,
because the previous sequential design let inter-host clock drift reverse the sign of a result.

The experiments behind all of this -- including the ones that failed -- are in
**[LATENCY_INVESTIGATION.md](LATENCY_INVESTIGATION.md)**.

## Contents

- [Status at a glance](#status-at-a-glance)
- [Delivery guarantees](#delivery-guarantees)
- [Redundancy: why duplication and not retransmission](#redundancy-why-duplication-and-not-retransmission)
- [Batching](#batching)
- [Fan-out to many receivers](#fan-out-to-many-receivers)
- [Submission mechanisms: io_uring and AF_XDP](#submission-mechanisms-io_uring-and-af_xdp)
- [What redundancy actually bought](#what-redundancy-actually-bought)
- [A second path: routed, long-haul, and what it settles](#a-second-path-routed-long-haul-and-what-it-settles)
- [Message format: what each field is for](#message-format-what-each-field-is-for)
- [Results](#results)
- [Measurement methodology](#measurement-methodology)
- [Environment and host tuning](#environment-and-host-tuning)
- [Clock synchronisation](#clock-synchronisation)
- [Next steps](#next-steps)
- [Known limits of these results](#known-limits-of-these-results)
- [Companion: LATENCY_INVESTIGATION.md](LATENCY_INVESTIGATION.md)

## Delivery guarantees

The consumer must observe a **strictly increasing** `seq_id`; anything outside that
sequence counts as a drop, and late delivery is worse than no delivery. The whole
delivery policy is therefore one rule, in `transport/include/delivery.h`:

> Publish a frame if and only if its `seq_id` exceeds the highest already published.

That single comparison provides four things at once: de-duplication, protection against reordering,
the late-arrival drop policy, and correct gap accounting. It also decides the loss-recovery strategy,
which is the next section.

We accept a non-zero drop rate rather than delaying the stream to repair it. The justification is not
"loss is cheap" — it is that under this rule a repair arriving late **cannot be delivered at all**,
so paying latency for it buys nothing. Instead loss is made rare, via redundancy that costs no
latency, and harmless, via a format from which a consumer can recover state. The drop rate is
reported alongside latency for every configuration so the trade is never made silently.

## Redundancy: why duplication and not retransmission

Under the monotonic rule a retransmitted frame is worthless by construction: by the time a NACK
completes a round trip, later `seq_id`s have been published, so the gate must reject the repair.
Forward error correction fails for a related reason — parity reconstructs a missing frame only after
enough *later* frames arrive, by which point the stream has moved past it.

The only recovery with no latency cost is **immediate duplication**. If the first copy is lost the
second still arrives before the next message exists, so it passes the gate. Independent loss `p`
becomes `p²` — which is the premise, and it turns out to be false on both measured paths, because
loss is bursty rather than independent.

Sending everything twice unconditionally would double the packet rate at every load, including when
the path can least afford it. So duplication is **opportunistic**: having sent a datagram the sender
looks for more work, and only if the source ring is empty does it send the copy. Redundancy consumes
only time the sender had nothing better to do with, and is load-adaptive with no knob to tune:

| offered rate | msgs/datagram | duplication |
|---|---|---|
| 1,000 | 1.00 | 100% |
| 500,000 | 1.00 | 77.7% |
| 1,000,000 | 1.19 | 0.4% |
| 2,000,000 | 2.53 | 0.0% |

It **stands down on its own** as the rate rises, reaching effectively zero by 1M msg/s — before
packet rate becomes the binding constraint. As it backs off, batching takes over. Whether the
redundancy is *worth* anything is a separate question, answered largely no in [What redundancy
actually bought](#what-redundancy-actually-bought).

## Batching

Several events are packed into one datagram, at every rate, through one code path
with no rate threshold. The sender drains whatever the source ring already holds
and sends immediately.

Crucially it never *waits* for a datagram to fill. At a low rate that yields one message per
datagram — not because batching is disabled, but because only one message exists. Waiting for a
second at 1,000 msg/s would mean a 1 ms delay against an end-to-end latency of roughly 2.2 µs, some
450× the entire budget, to save one packet. Batching what is available costs nothing; batching that
waits costs everything.

As duplication backs off under load, batching takes over. Both follow the same principle — use spare
capacity, never delay a message — which is why they compose without configuration.

## Receive mode

Kernel busy-poll lets the receiving thread pull packets off the device queue on its
own core instead of waiting for a softirq. Measured at 200k msg/s:

| mode | p50 | p99 | p99.9 |
|---|---|---|---|
| **busy-poll**, real NIC | **34,611** | **39,156** | **54,522** |
| spin, real NIC | 59,046 | 82,863 | 191,876 |

Busy-poll is 1.7x better at the median and 3.5x better at p99.9. Both rows are
cross-host over the NIC, like every other number in this document.

The mode is still selected from the bound address rather than exposed as a flag, and the
reason is mechanical rather than measured: `SO_BUSY_POLL` is consulted by `sk_busy_loop()`,
which needs a NAPI-backed device to poll. Bound to an address with no such device there is
nothing to poll, so the blocking receive falls through to sleeping and adds a scheduler
wake-up per message instead of removing one. The right answer is a property of the path, so
the transport derives it rather than asking the operator to know which path they are on.

Two alternative receive architectures were measured and both lost. io_uring has no system call to
save on this side — both shapes cost one transition per datagram — and re-tested at twice the
datagram rate, where a multishot receive should finally have one, it was **1.98× worse in absolute
p99**; it merely stops getting worse, because a larger constant cost already dominates. And
`--split-poll`, which runs polling and publishing as two threads on the theory that a
single-threaded loop stops polling exactly when it falls behind, refuted its own premise: the
handoff queue never held more than 4 datagrams of 8,192, so the publisher was never behind and the
loop it targets was never active — leaving only its cost, the ring-publish leg rising from 52 ns to
505 ns. The single-threaded kernel busy-poll receiver ships.

## Fan-out to many receivers

Each receiver is an independent destination: its own socket, its own shared-memory
ring, its own consumer. The sender transmits a **separate datagram to every one of
them**, so the cost that grows with receiver count is datagram replication.

### Replicating to N destinations — measured, not assumed

Three call shapes put the same bytes on N addresses: `sendmmsg` (one syscall, one socket plus N
`msg_name`s), `sendto` × N, and connected sockets with `send` × N. Measured at 10 destinations and
200k msg/s, a rate none of them saturates:

| method | msgs/datagram | p50 | p99 | source-ring leg | wire leg | skew p50 |
|---|---|---|---|---|---|---|
| **connected** | 2.27 | **47,764** | **59,451** | **5,841** | 36,014 | **10,234** |
| `sendmmsg` | 3.16 | 52,328 | 65,822 | 8,182 | 35,687 | 13,839 |
| `sendto` × N | 3.23 | 50,533 | 64,794 | 8,320 | 34,231 | 13,938 |

**The stage split identifies the mechanism, and it is not the obvious one.** The wire leg is the
same for all three within 5%. The entire difference is in the source-ring leg — how long a message
waits before the sender reaches it. A cheaper send call drains the ring faster, so less queues up
(5.8 µs against 8.2) and fewer messages accumulate per datagram (2.27 against 3.16). The tempting
explanation is per-`msg_name` route resolution, and it is **wrong**: a route lookup is 100–200 ns and
cannot account for 4.6 µs. The cause is sender throughput.

`sendto` × 10 additionally could not sustain 1M msg/s at 10 destinations at all — a workload
taking ~4 seconds for the other two had not finished after 12 minutes — so it is ruled out on
throughput, not just latency.

`kAuto` is therefore connected sockets at every destination count. The argument that *predicts*
the opposite — one syscall must beat N — has now lost at both 3 and 10 destinations.

### Cost per receiver, and the limit it implies

Measured at 500k msg/s with redundancy disabled so receiver count is the only variable:

| receivers | p50 | p99 | skew p50 | skew p99 |
|---|---|---|---|---|
| 1 | 32,569 | 41,701 | — | — |
| 2 | 40,295 | 47,449 | 790 | 1,611 |
| 3 | 36,585 | 44,593 | 3,103 | 5,188 |
| 10 | 47,663 | 59,322 | 11,669 | 11,691 |

**Skew is the clean signal**, growing about **1.3 µs per additional receiver** to 11.7 µs at ten.
The medians rise too — roughly 15 µs from one receiver to ten — but do not order cleanly between two
and three, where the difference is smaller than the burst events under [Results](#results).

**The binding limit is not the skew, it is the packet-rate budget.** Every receiver costs one more
packet per datagram, against a ceiling this path puts at roughly 840k datagrams per second. At ten
receivers and 500k msg/s the sender was already at **97% of it** (81k datagrams/s × 10 = 812k pps),
so the ceiling divides achievable message rate by receiver count:

| receivers | datagram budget | message rate at ~6 msgs/datagram |
|---|---|---|
| 1 | 840,000 | ~5,000,000 |
| 3 | 280,000 | ~1,680,000 |
| 10 | 84,000 | ~504,000 |
| 20 | 42,000 | ~252,000 |
| 50 | 16,800 | **~101,000** |

So **serial unicast replication cannot deliver the task's top rate to anything like 50 receivers**,
and no optimisation above the socket layer changes that: the limit is packets, and unicast fan-out
needs N per datagram. Batching pushes the ceiling out by a constant factor and is already doing so —
6.2 messages per datagram at ten receivers — but cannot change the linear dependence on N.

Getting to 50 receivers means not sending N copies from one host: **network-level multicast**, where
one packet reaches every receiver and packet rate stops depending on N (whether the fabric supports
it is the question — a VPC generally does not without extra machinery), or **a relay tree**, where
each tier fans out to a bounded number of children so no host pays more than its branching factor,
at the cost of a hop of latency per tier.

## Submission mechanisms: io_uring and AF_XDP

Both were built and measured against kernel UDP. Both lose, which is the useful outcome: the
default is now the measured winner rather than the conservative choice. The backend is selectable
per end, so send and receive are separated rather than confounded.

**The receive path is the unambiguous result.** Send held on kernel UDP, 10 destinations,
200k msg/s:

| percentile | kernel UDP | io_uring | ratio |
|---|---|---|---|
| p50 | 44,147 | 74,730 | **1.7×** |
| p99 | 55,056 | 125,687 | **2.3×** |
| p99.9 | 65,781 | 289,987 | **4.4×** |

Nowhere near overlapping, and the io_uring receive path also dropped datagrams where the kernel path
dropped none. **It is arithmetic, not a surprise:** kernel UDP costs one blocking `recv()` per
datagram and io_uring's NAPI mode costs one `enter()` per datagram — same call count, plus ring and
provided-buffer bookkeeping. There was never a system call to save on this side. What is *not*
explained is the size; 69% at the median is more than bookkeeping should cost, and rather than invent
a mechanism that is recorded as open.

**The send path is not separated at all, and how that was established matters more than the
numbers.** Three tight repetitions of one backend followed by three of the next confounds backend
with time. Measuring the same pair again in a later session:

| | kernel UDP p50 | io_uring p50 | difference |
|---|---|---|---|
| session 1 | 43,754 / 43,965 / 44,717 | 39,869 / 41,306 / 42,246 | **−3,005 ns** (io_uring faster) |
| session 2 | 41,147 / 41,390 / 41,442 | 43,583 / 44,361 / 44,762 | **+2,909 ns** (io_uring slower) |

The sign reversed and the reversal was the same size as the effect. Within either session the
repetitions are tight and non-overlapping, which is what makes the trap dangerous: the data looks
conclusive and is not. A "65% off p99.99" reading from these runs turns out to be an artefact of
summarising per-file percentiles rather than pooling them, and no median difference survives either.
This is the finding that produced the interleaved block design in
[Measurement methodology](#measurement-methodology).

Two things do reproduce. **The sender's own drain rate**: at a fixed offered rate, frames per
datagram reads out how fast the sender clears its ring, and io_uring packs 2.30 frames per datagram
against kernel UDP's 2.68 at 500k msg/s, with a send cycle of 11.5 µs against 13.4 — a 14% cheaper
send, in every repetition, needing no cross-host clock. And **the per-stage split**, which shows the
saving cannot reach the consumer: it lands in the source-ring wait, 5.3 µs of a ~36 µs path, while
the wire leg absorbs the difference.

**AF_XDP was chosen to test kernel interference and could not.** It writes a complete Ethernet frame
into memory the driver reads from, skipping the kernel's IP and UDP layers, routing lookup, netfilter
hooks and qdisc; `transport/include/xdp_backend.h` builds Ethernet, IPv4 and UDP itself, computes the
IPv4 checksum, and takes the next hop's MAC from the kernel's neighbour cache rather than
reimplementing ARP. But no zero-copy is available in this configuration, so only `XDP_COPY` binds,
and copy-mode transmit still runs through `xsk_generic_xmit` — which allocates an skb and calls
`dev_direct_xmit`. That removes the protocol stack and keeps skb allocation and the driver path, so
most of the kernel involvement remains. The limitation was written into the header comment *before*
measuring, with the prediction that copy mode might therefore be slower than a plain connected
`send()`. It was: **37% off the send cycle, the largest saving of any mechanism, and 4.6 µs worse end
to end**, because the saving moves into the wire leg.

Two AF_XDP details worth recording. A transmit-only socket cannot be bound — creating only the
completion and transmit rings is refused with `EINVAL` at every queue and flag combination, while
adding the fill and receive rings makes the identical bind succeed, so the relay creates all four and
leaves two permanently empty. And it needs root, which is both a deployment cost and an operational
hazard: when a session died mid-run a root-owned sender survived it and kept a transmit queue bound
until found by hand, so the sender now has an idle timeout that terminates an orphan.

**The ABI is declared in-tree, not taken from `liburing`**, so the build depends on the running
kernel rather than a build environment. It is guarded at compile time by `static_assert`s on every
structure size and every field offset the kernel reads, and at run time by `IORING_REGISTER_PROBE`,
which refuses to start if an opcode is missing. `send_all()` stays synchronous, because the relay
reuses its datagram buffers immediately and the redundancy path re-sends the *previous* datagram out
of the buffer the next one is about to overwrite — an asynchronous send would leave the kernel reading
memory the relay had moved on from.

### What ships

Kernel UDP at both ends, single path, single-threaded receiver. `--backend iouring`, `--backend xdp`,
`--dual-path` and `--split-poll` are all opt-in, and of the four only `--dual-path` improves anything
measured — above p99.9, on the direct path, and only below about 420k msg/s of rate × fan-out.
Keeping the default is also deliberately conservative: every rate, fan-out and loss figure in this
document was measured on the kernel-UDP path, and promoting a new default without re-running that
sweep would leave the results describing a configuration that is no longer shipped.

What the alternatives did buy is a quantified model of where the time goes — the send cycle is
~11.5 µs of a ~36 µs path, cheaper submission shortens the source-ring wait exactly as predicted, and
none of it reaches the consumer because the wire leg absorbs it. That points the remaining work at
the wire leg rather than at submission, and it is a more useful result than a few percent would have
been.

## What redundancy actually bought

The duplication scheme was designed against independent loss, where sending each datagram twice
turns `p` into `p²`. That reasoning is only as good as its independence assumption, so it needs
measuring. The result is mostly negative, and the reason is interesting.

### Loss actually observed on the direct path

Across the whole rate sweep with redundancy disabled — **241 million messages** — loss is
essentially absent: zero at four of the six rates, and the only loss (5,262 datagrams,
8.8 × 10⁻⁵) sits at the highest packets-per-second point, consistent with the pps limit under
[Results](#results). Total 2.2 × 10⁻⁵.

Nothing there comes near the 0.01–1% the task treats as realistic, so **this path cannot exercise a
loss-recovery mechanism at all.** That is a property of the test path, not a claim about the design —
and it is why the routed path below matters.

### The loss that does occur is bursty, and duplication misses it

| offered rate | duplication | gap events | datagrams lost | rescued by a copy |
|---|---|---|---|---|
| 500,000 | 77.7% | 7 | 485 | **2** |
| 1,000,000 | 0.4% | 50 | 4,007 | **14** |

Read the middle columns together: **485 lost datagrams arrived in 7 events** — about 69
consecutive per event. Loss here is not independent at all; it comes in bursts, exactly like the
latency spikes. That is why only 2 of 485 were rescued: a copy sent microseconds after its
original lands **inside the same burst**. At 500k msg/s a 69-datagram burst spans ~140 µs, and the
copy follows by a few microseconds — three orders of magnitude too close to escape.

**So duplication as implemented buys almost nothing against the loss this path exhibits**, and the
p → p² argument does not apply because its independence premise is false. It costs nothing at low
rate, using only idle capacity and standing down under load, but "harmless" is not "useful". The
fix it points to is a **temporal stagger** — hold the copy back by longer than a burst — which is
a small sender change needing nothing from the receiver, since the gate already absorbs a late
copy.

### Redundancy across four-tuples: a real cost, and a benefit that would not replicate

The result above is about *loss*, for two copies sharing one four-tuple. A different arrangement
sends the copies over **distinct source ports**, so the four-tuple that path selection hashes on
differs. Two variants were measured: `--dual-path`, which submits both copies inline, and
`--dup-path`, which gives redundancy its own sockets and writes to them only when the source ring
is empty, so a copy can never delay a real message. The receiver needs no new code either way --
the monotonic gate already publishes whichever copy arrives first.

**Measured twice, and neither half of the apparent trade survives.** One session showed 5.8× off
p99.99 for 10% onto the median; repeating it reversed the sign of both — p50 from +3,303 ns to
−1,183, and p99.99 from 0.17× to 3.19×. The median effect is clock drift: per-repetition spreads
measure variation *inside* a window and are blind to drift *between* two configurations' windows. The
far-tail effect is baseline variance: across three runs of the *identical* configuration, p99.99 was
81,133 / 921,223 / 1,172,844 ns, a **14.5× spread**, so a single pairing reveals which window it
sampled rather than which configuration is better.

**What does replicate is a p99 cost, and it is entirely the receiving kernel.** On the receiver's
own clock, needing no offset correction, its delivery leg goes 1,518 -> 7,437 ns and
2,041 -> 7,438 ns in the two sessions. That accounts for essentially the whole end-to-end p99
penalty. At the median it is 905 -> 949 ns, i.e. nothing: every published message now arrives with
a twin that still had to be received, timestamped and gated, and that only tells when the loop is
behind.

**No send mechanism reaches that cost.** The copy was carried in turn by kernel UDP inline, kernel
UDP on its own sockets, io_uring with SQPOLL, and AF_XDP on its own transmit queue. The receiving
kernel's p99 across all four spans **237 ns, 3.2%**, on a penalty of ~5,400–6,100 ns. AF_XDP is the
decisive case: it goes through `dev_direct_xmit`, sharing neither qdisc nor hardware queue with the
primary, and is indistinguishable from sending both copies back to back down one socket. Bypass on
the send path cannot reach it.

**So four-tuple redundancy is not shipped.** Its cost is real, reproducible and located; the benefit
that would pay for it is unproven on the direct path and resolved *against* on the routed one. Both
variants stay available (`--dual-path`, `--dup-path --dup-backend ...`) because settling the question
needs them. Full analysis in [LATENCY_INVESTIGATION.md](LATENCY_INVESTIGATION.md).

## A second path: routed, long-haul, and what it settles

Every figure above comes from a directly-attached path: one hop, ~35 µs one way. The transport
was then measured over a **routed inter-region path** at ~35 *milliseconds* one way -- three
orders of magnitude longer. That inverts what is measurable. Our own cost, ~620 ns, is 0.002% of
this path; what can be tested is whether the design's *rules* survive a network that is allowed
to reorder, drop and re-route.

**Reaching it required one code change.** The outgoing link on these hosts is selected by source
address rather than destination prefix -- one routing table per uplink and an `ip rule` per local
address. An unbound socket leaves by the main table, i.e. the management interface, not the link
under test: bound to the interface the peer was unreachable, bound to the source address it
answered in 69.7 ms. `--src-addr` now exists, applied in the shared socket setup so the
kernel-UDP and io_uring senders cannot disagree, loud on failure rather than silently falling
back, and rejected outright by AF_XDP, which builds its own IP header and cannot honour it.

**The delivery rule holds.** The monotonic gate drops anything not strictly increasing, so
reordering would cost real messages rather than merely delaying them. Measured across three
rates: **zero reordered datagrams in 1,549,105**, and nothing suppressed. The strictness that
buys deduplication, late-drop and gap accounting in one rule costs nothing here either. Path MTU
is also exactly 1500 end to end (1472 + DF passes, 1473 fails), so the no-fragmentation
assumption is not an artefact of the short path.

| offered rate | reordered | first-copy loss | mean burst | one-way p50 | p99.9 − p50 |
|---|---|---|---|---|---|
| 1,000 | **0** | 0.0119% | 1 | 35.20 ms | 1,219 µs |
| 10,000 | **0** | 0.0466% | 133 | 34.01 ms | 6,855 µs |
| 50,000 | **0** | 0.1789% | 20.6 | 33.54 ms | 763 µs |

**Loss is 20-80× the short path's**, and rises with offered rate. The highest rate again has the
lowest median and tightest tail, for the same cold-cache reason as on the short path; the
receiving kernel's delivery leg falls from 3.5 µs to 1.2 µs across the sweep.

**This is the one regime where absolute one-way latency is trustworthy**: the clock offset measured
~27 µs, but that is 0.08% of a 33.5 ms path, and the measured one-way sits within 0.3% of half the
ICMP round trip. The corresponding warning is that between-run variation of the *median* was ~1.7 ms,
so comparisons here need the block design even more than on the direct path.

**Redundancy measurably makes things worse here, across six interleaved blocks** — the one
comparison in this document that reaches a resolved verdict, because the metrics that matter on a
lossy path are counters on the receiving host alone, so no clock is involved:

| metric | without redundancy | with redundancy | n | sign p | verdict |
|---|---|---|---|---|---|
| first-copy loss | 0.1558% | **+0.0129 pp** (+0.0121..+0.0361) | 6 | 0.031 | **resolved** |
| frames never delivered | 1,920 | **+170** (+135..+217) | 6 | 0.031 | **resolved** |
| end-to-end p50 | 34.20 ms | +77,574 ns | 6 | 0.688 | not resolved |
| end-to-end p99.9 | 35.19 ms | -654,650 ns | 6 | 0.688 | not resolved |

Twelve of twelve arm-runs valid, a block's two arms two minutes apart, and every block agreeing in
sign on both loss metrics: redundancy costs about **9% more undelivered frames**. The mechanism is
the event structure — the arm without redundancy lost its ~1,920 datagrams in a **single ~39 ms
outage**, and a copy sent microseconds later, even over a different four-tuple, is inside it;
doubling the packet rate over the same physical link then adds the +170.

Nothing on the latency side separates, and the table says why: the reference arm's own median moves
**1.1 ms between blocks** and its p99.99 moves 4.5 ms. That is why the loss counters carry the
result and the latency rows are reported unresolved rather than squeezed for a conclusion.

Full detail in [LATENCY_INVESTIGATION.md](LATENCY_INVESTIGATION.md).

## Message format: what each field is for

The original harness format carried 320 bytes per message on a mixed stream: padding, values
derivable from other values, values sent twice in two representations, and static reference data
repeated on every message.

**The reworked format lives in `harness/include/message.h`, not in a separate wire codec.** The
messages themselves are the optimised form, so the producer emits them, the shared-memory rings
carry them, the wire carries them byte-for-byte, and the relay does no conversion on the hot path.
The ring slot shrinks with the largest frame, from 640 to 192 bytes, so every shared-memory hop
moves a third of the memory it used to.

### The rule that constrains everything

**Nothing is encoded relative to a previous message.** Encoding a price as an offset from the
previous trade, or a book as a delta against the last one, would be among the largest savings
available — and would mean losing one message corrupts every message after it. The only relative
encoding used is *within* a single frame (a book's lower levels against the top of that same side),
where loss cannot separate the parts.

Every frame is therefore independently interpretable, with no decoder state for a gap to corrupt.
That is also why `seq_id` and `send_ts_ns` stay absolute rather than becoming deltas against a
datagram base: the 6 bytes saved are not worth making a frame meaningless on its own, and keeping
them absolute is what makes the ring frame byte-identical to the wire frame.

### Framing header — 24 B → 32 B, absorbing 48 B from every body

The one part that grew, and it pays for itself immediately: it now carries the instrument id and both
venue timestamps that previously sat in each body as 32 bytes of text and 16 bytes of absolute
timestamps.

| field | decision | reasoning |
|---|---|---|
| `seq_id` | **kept**, 8 B absolute | Required framing, unchanged. |
| `send_ts_ns` | **kept**, 8 B absolute | Required framing, unchanged. |
| `type` | **kept**, 1 B | Needed to interpret the body, and now also implies its length. |
| `instrument` | **added**, 2 B | Replaces 32 B of symbol/venue text per message. |
| `flags` | **added**, 1 B | Absorbs every per-type boolean as bits. |
| `exch_ts_delta_ns` | **moved here**, 4 B | Was an 8 B absolute timestamp in each body; now an offset from this frame's own `send_ts_ns`, so the frame stays self-contained. |
| `match_ts_delta_ns` | **moved here**, 4 B | Same, relative to `exchange_ts_ns`. |
| `version` | **dropped** | Describes the protocol, not the message. Carried once per datagram. |
| `body_len` | **dropped** | Fully derivable from `type` — every type is a fixed size (`frame_size`). |

### The three bodies, by class of decision

Every field was decided individually; the decisions collapse into five rules, which is more
useful to read than three tables of field names.

- **Derived values go.** `price` was exactly `price_ticks × 0.01`, `quantity` exactly
  `quantity_lots × 0.001`, `notional` exactly their product, `body_len` fully implied by `type`.
  Sending a number twice in two representations is not redundancy, it is a rounding-bug surface —
  the scaled integer is canonical and exactly representable.
- **Reference data goes.** `symbol`, `venue` and both currency names are static for a session:
  32 bytes of text per message replaced by a 2-byte instrument id, which is what real venues put
  on the wire.
- **Booleans become bits.** Aggressor side, block trade, retail price improvement, liquidation
  and a four-state tick direction fit in one `flags` byte.
- **Timestamps become deltas, but only within a frame.** `exchange_ts_ns` is carried as a 4-byte
  offset from the frame's own `send_ts_ns`, and `match_engine_ts_ns` as an offset from that. Both
  stay self-contained; neither references another message.
- **Padding goes**, and the space it frees pays for the fields worth adding.

Three judgement calls are worth stating because they cost bytes rather than saving them.
`trade_id` is **kept** — it is the venue's identity for the print, needed to reconcile against
the venue's own feed and to spot venue-side gaps, and it is not derivable. `tick_direction` is
kept even though a consumer could in principle derive it, because a consumer that missed the
previous trade cannot. And running totals (`cum_quantity_lots`, `cum_notional_ticks`,
`cum_trade_count`) were **added**: they let a consumer that missed messages recover aggregate
state without replay, which is worth 24 bytes on a format that still shrank 2.4×.

Order books keep the top of each side absolute and encode the remaining levels relative to it —
the one relative encoding used, and safe because loss cannot separate a frame from itself. Spending
bytes to keep every book self-contained is the same decision as refusing to fragment, for the same
reason.

### Result

| message | before | after | ratio |
|---|---|---|---|
| Trade | 192 | 80 | 2.4× |
| Bbo | 192 | 64 | 3.0× |
| OrderBook | 576 | 160 | 3.6× |
| mixed-stream average | 320 | 101 | **3.2×** |
| largest frame (sets ring slot size) | 576 | 160 | 3.6× |
| shm ring slot | 640 | 192 | 3.3× |

Per 1500-byte datagram, order books go from 2 to 9 and trades from 7 to 18. The
effect on throughput is larger than the byte ratio alone suggests, because fewer
packets per event *and* smaller ring slots both matter: the relay previously
collapsed at 2.2M msg/s with 20% loss, and now sustains **6M msg/s with zero loss**
— three times the top of the required rate range.

### One honest caveat

**Generator artifacts were deliberately not exploited.** In this harness `venue` is constant,
`match_engine_ts_ns` always equals `exchange_ts_ns`, the boolean flags are always zero and `reserved`
is always zeroed. Compressing on those facts would score well here and fail on any real feed, so the
instrument dictionary is sized for genuinely multi-venue streams, every flag bit is carried, and the
match-engine delta is transmitted even though it is always zero in this data.

## Results

Two paths, both real networks, mixed message stream, redundancy disabled so rate is the only
variable. The **direct path** is two hosts on one L2 subnet, one hop; the **routed path** is a pair
across regions at three orders of magnitude more latency. Both are reported here, because a design
that only works on one of them is not characterised.

### Direct path

**Read the cross-host absolutes as ±4 µs.** A reflector probe measured the two hosts' clock
offset wandering across 8.8 µs over an hour while the path itself stayed within 708 ns, so any
"receiver's stamp minus sender's stamp" carries that wander. Bracketing a run with idle probes
cuts it to ±0.6 µs, and done that way the 200k median is **36,531 ns ± 564** against the 32,798
the same run reports uncorrected — the table below is understated by a few microseconds, by an
amount that differed per session. Single-host legs (source-ring wait, receiving-kernel delivery,
ring publish) and the drain-rate measure are exact and unaffected. Method in
[LATENCY_INVESTIGATION.md](LATENCY_INVESTIGATION.md).

**Every number here is cross-host.** Loopback understates latency by roughly 15× because it has no
driver, NIC or wire, so it is used only to check that code paths function and never to produce a
figure.

| offered rate | samples/run | p50 | p99 | p99.9 | p99.99 | p99.999 | drop |
|---|---|---|---|---|---|---|---|
| 100 | 5,400 | 46,470 | 60,880 | 66,590 | — | — | 0% |
| 10,000 | 383,616 | 42,249 | 46,684 | 69,943 | — | — | 0% |
| 200,000 | 19,983,616 | 35,876 | 40,479 | 50,804 | 68,406 | 1,230,358 | 0% |
| 500,000 | 19,983,616 | 35,180 | 44,549 | 301,905 | 1,186,413 | 1,404,616 | 0% |
| 1,000,000 | 19,983,616 | 39,768 | 61,730 | 99,416 | 410,958 | 3,750,224 | 0.0088% |
| 2,000,000 | 19,983,616 | 37,696 | 57,227 | 95,885 | 230,640 | 435,058 | 0% |

Latency in ns. p99.99 needs ~1M samples and p99.999 ~10M to rest on ~100
observations; the 100 and 10k rows do not have them, so their far tail is omitted
rather than quoted.

**The median and p99 are essentially flat across the task's whole stated range**, and
the lowest rate has the *highest* median -- at 100 events/second messages arrive 10 ms apart and
every one hits cold caches and a cold TLB. Throughput is not the binding constraint
anywhere from 100 to 2M events per second.

### Where the time goes

The sender stamps departure into the datagram header, the receiver stamps arrival and
publication, and joining on `seq_id` against the consumer's timestamp yields the
fourth leg. 20M messages at 200k msg/s:

| leg | p50 | p99.9 | p99.99 | p99.999 | max |
|---|---|---|---|---|---|
| source ring + sender | **305** | 670 | 731 | 748 | 4,777 |
| **wire** (kernel TX to arrival) | **34,338** | 49,143 | 67,273 | 1,055,667 | 1,960,424 |
| arrival to published | **50** | 222 | 276 | 7,072 | 12,984 |
| published to consumer read | **264** | 634 | 743 | 6,347 | 108,465 |
| end to end | 34,996 | 49,818 | 68,089 | 1,056,468 | 1,961,160 |

**Our code accounts for about 620 ns of the 35 µs median — 1.8% — and stays under 1 µs even at
p99.999.** The other 98% is kernel TX, NIC, wire and kernel RX, so there is no optimisation left
above the kernel that could move the median. Only the wire leg spans two hosts: the three legs
carrying our own cost are single-clock, so the 620 ns does not depend on synchronisation, and the
four legs sum to the independently measured end-to-end value (35,075 against 35,325 ns).

### The far tail is one repeating event

p50 through p99.99 sit between 35 and 68 µs; p99.999 jumps to about 1 ms. That gap is
not gradual degradation, and the stage data says exactly what it is: **all 213
messages over 1 ms in a 20M-message run are wire-dominated, 213 of 213.**

Plotting one event by sequence id settles the mechanism: latency falls in a straight line from
1.96 ms over ~385 consecutive messages, at 4,219 ns per message against a 5,000 ns arrival
interval. A queue stopped being served for ~2 ms, accumulated several hundred messages, then
drained. The implied drain rate — one message every ~780 ns, about 1.3M msg/s — is the receive
path's burst capacity.

**The spikes arrive in bursts, and the larger the spike the more strictly so.** Grouping messages
above a threshold into runs of consecutive arrivals, where independent spikes would be almost
entirely runs of one: above 50 µs, ~23% are isolated; above 100 µs, 0.8%; above 200 µs, 0.1%;
**above 500 µs, none at all — 100% sit inside bursts of ten or more.** A 20M-message run contains
one to three such bursts and they set p99.99 and beyond.

Two consequences. A far-tail percentile from a short run is luck rather than measurement. And the
events differ between repetitions of an identical configuration — 2 ms, 4.5 ms, 700 µs — so
averaging them would describe something that never happened.

We have **not** attributed the stall further than "the wire". Separating sender kernel
TX from NIC from receiver kernel RX needs NIC hardware timestamps; this setup collects
software timestamps only, and the hardware route was not pursued. That is the top open item.

### Loss follows packet rate, not message rate

Loss was expected to climb with message rate, since a NIC has a packets-per-second ceiling. It does
not work that way:

| offered rate | msgs per datagram | datagrams/s | drop |
|---|---|---|---|
| 500,000 | 1.00 | 500,000 | 0% |
| 1,000,000 | 1.19 | **840,336** | **0.0088%** |
| 2,000,000 | 2.53 | 790,514 | 0% |

The only loss in the sweep sits at the highest *packet* rate. Doubling the message rate from 1M to
2M *reduces* packets per second, because batching packs 2.5 messages per datagram — and loss returns
to zero. So the pps limit is real and we operate near it; batching is what keeps the higher message
rates on the safe side. It also identifies what would break first: larger messages, or disabling
batching, would push packet rate up at a given message rate and bring the loss back.

### Routed path

The same relay, unmodified apart from binding its source address, over a routed inter-region path.
Absolute latency is set by distance and the transport is invisible in it — which is exactly why this
path is worth reporting: what it tests is whether the design's *rules* hold on a network permitted to
reorder, drop and re-route.

| offered rate | datagrams | one-way p50 | p99 − p50 | p99.9 − p50 | reordered | loss | mean burst |
|---|---|---|---|---|---|---|---|
| 1,000 | 33,505 | 35.20 ms | 357 µs | 1,219 µs | **0** | 0.0119% | 1 |
| 10,000 | 285,250 | 34.01 ms | 1,670 µs | 6,855 µs | **0** | 0.0466% | 133 |
| 50,000 | 1,230,350 | 33.54 ms | 267 µs | 763 µs | **0** | 0.1789% | 20.6 |

**Zero reordering across 1,549,105 datagrams**, and nothing suppressed by the gate — so the strict
monotonicity that rules out retransmission and forward error correction costs nothing here either.
MTU is 1500 end to end on this path too, confirmed with a no-fragment probe, so nothing fragments.

**Loss is 20–80× the direct path's and rises with offered rate**, reaching 0.18% at 50k msg/s, and it
arrives in bursts rather than independently — one event of 133 consecutive datagrams at 10k, means of
20.6 at 50k. That is the regime in which redundancy could have paid and does not: see
[A second path](#a-second-path-routed-long-haul-and-what-it-settles).

Two things read backwards until the mechanism is clear. The **highest** rate has the lowest median and
the tightest tail, for the same cold-cache reason as on the direct path — the receiving kernel's
delivery leg falls from 3.5 µs to 1.2 µs across the sweep. And **absolute one-way latency is more
trustworthy here than on the short path**, not less: the inter-host clock offset is larger in absolute
terms (~27 µs) but that is 0.08% of a 33.5 ms one-way, and the measured one-way sits within 0.3% of
half the ICMP round trip.

Our own legs are unchanged from the direct path, because they do not depend on the network: the
source-ring wait is 305–454 ns and the ring publish 40–45 ns at every rate above. The relay costs the
same on a 35 µs path and a 35 ms one.

## Measurement methodology

Measuring a tail is easy to get wrong, and several of the results here only became
trustworthy after fixing the measurement rather than the code.

- **Steady state only.** The stream runs continuously; consumers attach at the live
  edge after a warm-up, so no measurement includes process startup, cold caches, or
  the first-touch page faults of the shared-memory ring. The first samples of each
  run are discarded in analysis.
- **Percentiles are pooled, not averaged across files.** A percentile of N×M samples is an
  estimate from N×M samples; the median of N per-file percentiles is a different quantity, and at
  a far tail it is wrong by multiples, because the median across files discards exactly the files
  holding the rare events. Both analysis paths used to do the latter. Correcting it moved figures
  by up to **115×** at the extreme tail — one `max` went from 155,450 ns to 18,144,863 ns, checked
  straight from the raw column with `sort -n` — while the median change at p50 through p99.999 was
  **0.0%**. The pooled rank is computed exactly without concatenating, by binary search over value
  space using `searchsorted` on the already-sorted per-file arrays, because pooling the largest
  configurations in memory costs gigabytes.
- **Repetitions inside one session are not an uncertainty.** A spread across repetitions describes
  variation *within* one window, and is blind to the two things that move a comparison: the clocks
  drifting apart between windows, and the environment's own between-window mood. Both withdrawn
  results quoted non-overlapping per-repetition spreads as evidence. Those columns are now labelled
  `within-session`, and a single-repetition run says so instead of printing `lo == hi` as though it
  were precision.
- **Configurations are interleaved.** `scripts/ab_bench.sh` measures each arm once per *block*,
  repeats blocks, and **reverses the arm order on alternate blocks** so the linear component of any
  drift cancels across a pair. `scripts/paired_stats.py` reports the per-block differences and — the
  column whose absence caused both retractions — how far the reference arm *alone* moves between
  blocks. An effect smaller than that is not resolved, whatever the medians say.
- **Six blocks is the floor, and it is arithmetic.** The verdict is an exact two-sided sign test
  over per-block differences, needing no assumption about the shape of the noise. With every block
  agreeing the smallest attainable p is 2/2ⁿ, so three blocks bottom out at 0.250: **a three-block
  comparison cannot be significant however large the effect.** Six is the first n that can, so it is
  the default and fewer prints a warning rather than a verdict.
- **Every run records its own provenance.** Commit, working-tree dirtiness, the exact
  flags of every arm, kernel, core assignment and the chrony state of both hosts are
  written to a manifest beside the results, so a number can be traced to the code and
  conditions that produced it.
- **A measurement that did not happen is recorded, not dropped.** Short runs, runs
  that produced nothing, suppression without redundancy configured, and reordering
  with it, are each written to the results file as an invalid row with a reason.
  Every one of those has fired on a real run; a silently missing block is
  indistinguishable from one that was never scheduled, and a previous comparison was
  published from a single surviving repetition whose spread therefore read as zero.
- **Nothing is written to a file while a measurement runs.** Samples go into memory reserved *and
  page-touched* before the first sample, `record()` is two stores, and the data reaches the
  filesystem in one pass afterwards; samples exceeding the reserved capacity are counted loudly
  rather than silently biasing the percentiles to a prefix of the run. Writing one CSV line per
  message from inside the loop puts a `write()` on the timing path, and that alone measures ~1 ms at
  p99.99 on a disk-backed filesystem — about 150× the transport's real tail, and invisible below
  p99.9.
- **Sample counts are sized to the percentile claimed.** A percentile needs roughly a hundred
  observations beyond it, so p99.999 needs ~10M samples. Where a rate cannot supply that in a sane
  run length, the far tail is omitted rather than quoted.
- **Cores are verified, not assumed**, by `scripts/check_cores.sh` before every run — see
  [Environment](#environment-and-host-tuning) for why that is the easiest thing here to get wrong.
- **Latency is attributed per stage**, and the sender can report producer→pre-send on a single
  clock, which localises a problem to before or after the wire with no cross-host clock question.
  That is what identified an 11 µs difference between two host pairs as environmental rather than
  ours (0.387 µs against 0.379 µs of relay cost).

## Environment and host tuning

Everything below was measured on two hosts in a **single VPC and the same subnet**, with
direct layer-2 connectivity on the measured path and both in the same availability zone.

| | |
|---|---|
| Sending host | `m7i.metal-24xl` (bare metal) |
| Receiving host | `m7i.24xlarge` (virtualised) |
| OS | Ubuntu 20.04 LTS |
| Kernel | Linux 6.9.0 |
| CPU | Intel Xeon Platinum 8488C, SMT disabled — 48 online cores |
| Topology | single NUMA node |
| NIC | ENA adapter, MTU 1500, verified end to end with a no-fragment probe |
| Receivers | up to 10 independent receivers, each with its own socket and its own consumer |

This mirrors the environment named in the task statement (m7i/m8a, Ubuntu 24.04 or
20.04, ENA, two servers in one subnet with L2 connectivity, 1 to 3 receivers).

### Host tuning, and the part that is easy to get wrong

The kernel on both machines was configured for low latency: cores isolated from the scheduler with
a separate housekeeping set, and NIC interrupts pinned to the housekeeping cores so device
interrupts never preempt a spinning measurement thread. What the solution itself tunes is confined
to its own processes — core pinning, socket buffer sizes, `IP_TOS` for low delay, and the receive
mode. It changes no sysctl and no `ethtool` setting, notably not interrupt coalescing, which stays
at the adapter's adaptive default.

`scripts/check_cores.sh` encodes the trap. It verifies before every run that each core is genuinely
idle, is in the isolated set, and has no active SMT sibling, and refuses to measure otherwise.
`isolcpus` stops the scheduler *migrating* work onto a core but not work explicitly pinned there,
and a core shared with another spinning thread produces latency quantised to the scheduler
timeslice — 4 ms here — which is indistinguishable from a network problem in the results.

**The virtualisation asymmetry was tested rather than assumed.** Because one host is virtualised,
two streams ran simultaneously in opposite directions over the same wire for 15 minutes, 18M
samples per side, so both directions share one time window. There is no periodicity —
autocorrelation of per-second worst latency at 300 s lag is −0.02 and −0.09 — and the bare-metal
receiver showed 2.4× *more* spike-seconds than the virtualised one. The residual outliers are not
attributable to virtualisation.

### Network path

Direct L2 on the measured path, no router hop, MTU 1500 confirmed end to end with a no-fragment
probe at 1472 bytes. That is what licenses not fragmenting: the largest message is 160 bytes after
compaction, so any MTU from ~600 upward carries every message whole. Refusing to fragment is a
resilience decision as much as a simplification — a message split across six fragments is six times
as likely to be lost, since losing any one destroys the whole message.

ICMP round-trip had a minimum of 0.173 ms and mean deviation 0.013 ms, used only as an upper bound
on one-way latency since ICMP is handled in softirq and deprioritised; the one-way UDP figures sit
well below half of it, in the expected direction. The measured path deliberately avoids the
interface carrying management traffic, so the benchmark cannot interfere with access to the hosts.

## Clock synchronisation

Every cross-host figure is a receive stamp on one host minus a send stamp on the other, so it is
only as good as the agreement between two clocks. chrony holds the direct pair to well within 3 µs
with a measured offset of a few hundred nanoseconds; `chronyc tracking` is captured before *and*
after each long run, and an idle reflector probe measures the offset directly.

**A static offset is mostly harmless** — it shifts every sample equally, so it biases the absolute
median but cancels exactly in anything comparative. **Drift is the hazard**, because an offset that
moves during a run smears the distribution and can pass for tail latency. On the direct pair it
wanders 8.8 µs over an hour, which is why comparisons are blocked rather than run sequentially.

**Which results depend on it at all.** Of the four legs in the stage breakdown only the wire leg
spans two hosts, so the finding that our code costs ~620 ns needs no clock agreement. Three
cross-checks: producer-to-pre-send is single-clock and is what localised an 11 µs difference
between host pairs as environmental rather than ours; ICMP round-trip halved is an upper bound
owing nothing to synchronisation, and our one-way figures sit below it as expected since ICMP is
deprioritised; and the drain-event slope matches the known inter-message interval, which it could
not if the clocks were drifting materially.

**The two paths sit in opposite regimes.** On the direct path the offset is sub-microsecond but its
*drift* is the same size as the effects being measured. On the routed path the offset is far larger
in absolute terms (~27 µs) but that is 0.08% of a 33.5 ms one-way, and the measured one-way sits
within 0.3% of half the ICMP round trip — long haul is the one regime here where absolute one-way
latency is trustworthy. Tightening the direct pair further would mean hardware timestamping or a
reflector design where one host stamps both departure and return; neither was attempted.

## Next steps

The "reduce the latency" line of work is finished, and finished negatively: io_uring and AF_XDP
were both built and measured, neither beats kernel UDP end to end, and the receiving kernel was
priced at ~908 ns so the AF_XDP receive path could be declined on evidence rather than guessed at.
What remains, in order of payoff:

1. **A shaped-loss rig.** Both available paths lose too little to exercise the redundancy design
   as drawn: 2.2e-05 on the direct path, 1.6e-03 on the routed one, against the 1-10% the task
   contemplates. Injecting loss with a controlled rate *and a controlled burst length* would settle
   in an afternoon what waiting for a bad day on a real link cannot — and burst length is the
   parameter that decides whether redundancy pays at all.
2. **A temporal stagger for the redundant copy.** Both paths agree loss arrives in bursts longer
   than the gap between a datagram and its copy, which is why copies fail together: 69-80
   consecutive datagrams on the direct path, a single ~39 ms outage on the routed one. Holding the
   copy back by longer than a burst is a small sender change and the receiver needs none. It trades
   latency for completeness, so it belongs behind a switch with both numbers published. Paired with
   item 1, this is the experiment that finds the crossover.
3. **Cut the receive-side cost of redundancy** — ~5-6 µs at p99, entirely inside the receiving
   kernel and identical across four submission mechanisms including AF_XDP on its own transmit
   queue, so it is the cost of taking delivery of twice the datagrams rather than of how they are
   sent. Two routes: a second receive socket polled on its own core, and an XDP program running the
   monotonic gate so the losing copy is discarded before skb allocation. The second is the one
   place XDP genuinely fits this problem, and it is an operator action.
4. **Hardware timestamps to open the last leg.** The 32.8 µs between the sending and receiving
   kernels is one number because software stamps cannot reach inside it. The only remaining question
   whose answer needs a different measurement technique rather than more analysis, and cheaper to
   try than any further bypass work.
5. **Multicast or a relay tree for fan-out.** The packet budget divides achievable message rate by
   receiver count, so unicast replication cannot reach fifty receivers at any interesting rate. The
   only item here that moves an asymptote rather than a constant.
6. **Higher rates and fan-out on the routed path.** Loss there rises with offered rate, so the
   question is where the knee sits — a judgement for whoever owns a shared long-haul link, which is
   why this stops at one receiver and 50,000 msg/s.

## Known limits of these results

- **The ~2 ms drain events are attributed to the wire and no further.** All 213 messages over 1 ms
  in a 20M-message run are wire-dominated and the shape identifies a queue draining rather than
  independent stalls. At 1M they are entirely upstream of the receiving kernel; at 200k and 500k
  about a third of the excess is the receiving kernel. Splitting the rest needs item 4 above.
- **No send mechanism is separated from another on end-to-end latency.** Inter-host clock drift of
  a few microseconds lives in the wire leg, is the same size as the differences being measured, and
  reversed the sign of the io_uring comparison between two sessions. Comparisons therefore run as
  interleaved counterbalanced blocks, which report how far the reference arm alone moves between
  blocks — 1.1 ms at the median on the routed path, so a few microseconds of send-mechanism
  difference is not resolvable there at all.
- **The io_uring receive penalty is unexplained in magnitude.** That it loses is arithmetic — both
  shapes cost one transition per datagram — but 69% at the median is more than ring and
  provided-buffer bookkeeping should account for, and no mechanism is claimed for the remainder.
- **AF_XDP could not test kernel interference**, because copy mode keeps skb allocation and the
  driver transmit path. The hypothesis that the kernel causes the burst events is untested, not
  refuted.
- **The SQPOLL drop burst was seen once and not repeated.** Two of ten receivers lost about 1,123
  datagrams each in one of three repetitions where no other configuration lost any. Not called a
  defect without repeat runs; not recommended meanwhile.
- **Fan-out is measured to ten independent receivers sharing one host**, so one NIC carries N
  copies where N hosts would each carry one. These figures are an upper bound on the design's cost
  rather than a prediction for N machines. Fifty receivers is not reachable by unicast at the
  task's upper rates: the packet budget caps roughly 101k msg/s there.
- **p99.999 rests on ~200 observations per run** and varies by a factor of two between
  repetitions. It is reported, but the drain event dominates it and it is not a property of the
  transport.
- **Duplication's premise is measured false on both paths.** Same-tuple copies fail together in a
  burst; four-tuple copies are resolved as a small net cost on the routed path (+170 undelivered
  frames, sign p = 0.031) and unresolved on the direct one. The two differ in burst length relative
  to the copy gap, which item 1 would sweep.
- **One host is virtualised and one bare metal.** Tested specifically for hypervisor-induced
  periodicity with simultaneous opposite-direction streams over 15 minutes and found none
  (autocorrelation at 300 s lag: −0.02 and −0.09).
