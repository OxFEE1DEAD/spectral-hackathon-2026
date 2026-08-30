# Where the latency actually is — investigation notes

Companion to [SOLUTION.md](SOLUTION.md). The main document describes the transport and the
decisions behind it; this one records the experiments that went looking for the remaining
latency, including the ones that failed and what they cost to learn. It is separate because
it is a lab notebook rather than a design description, and because two of its findings
change how every cross-host number in the main document should be read.

Everything here is cross-host over a real NIC. No same-host figures.

## Contents

- [The budget, and why it drives everything](#the-budget-and-why-it-drives-everything)
- [Experiment 1: split the wire leg at the receiving kernel](#experiment-1-split-the-wire-leg-at-the-receiving-kernel)
- [Experiment 2: latency and clock offset without trusting the clocks](#experiment-2-latency-and-clock-offset-without-trusting-the-clocks)
- [Experiment 3: correcting an absolute figure, by bracketing](#experiment-3-correcting-an-absolute-figure-by-bracketing)
- [Experiment 4: the software transmit stamp is unusable](#experiment-4-the-software-transmit-stamp-is-unusable)
- [Experiment 5: a receive path whose polling never stops](#experiment-5-a-receive-path-whose-polling-never-stops)
- [Experiment 6: path diversity, measured twice, and mostly retracted](#experiment-6-path-diversity-measured-twice-and-mostly-retracted)
- [Experiment 7: the redundancy penalty is on the receive side, and no send mechanism touches it](#experiment-7-the-redundancy-penalty-is-on-the-receive-side-and-no-send-mechanism-touches-it)
- [Experiment 8: auditing the measurement instead of the transport](#experiment-8-auditing-the-measurement-instead-of-the-transport)
- [Experiment 9: a routed long-haul path, where the design's real risk lives](#experiment-9-a-routed-long-haul-path-where-the-designs-real-risk-lives)
- [What this invalidates](#what-this-invalidates)
- [Two approaches that failed, and why](#two-approaches-that-failed-and-why)
- [Where this leaves the design](#where-this-leaves-the-design)

## The budget, and why it drives everything

At 200k msg/s, one receiver, medians in ns:

| leg | ns | share | measured how |
|---|---|---|---|
| our sender: wait in the source ring | 292 | 0.9% | one clock, exact |
| **sender kernel TX + NIC + fabric + NIC + receiving driver** | **32,836** | **96.2%** | two clocks |
| receiving kernel: demux, socket queue, busy-poll pickup | 908 | 2.7% | one clock, exact |
| our receiver: publish into the ring | 51 | 0.1% | one clock, exact |

**Our own code is about 1% of the path.** That single fact decides where effort can pay:
not in the relay. Every remaining question is either *what is that 96%* or *how do we route
around it*. It is also why the submission-mechanism work — kernel UDP against io_uring
against AF_XDP — could not move the median much whatever it did to send cost.

## Experiment 1: split the wire leg at the receiving kernel

**Question.** An AF_XDP receive path runs in the driver's poll routine *before* the kernel
allocates a socket buffer, so it can only win whatever the receiving kernel's delivery path
costs. Attaching an XDP program moves an interface from kernel-managed to userspace-managed,
so it was worth pricing the prize before paying for it.

**Method.** `SO_TIMESTAMPING` with `SOF_TIMESTAMPING_RX_SOFTWARE`. The kernel stamps each datagram as
it enters the receive path, splitting the wire leg into *to RX stamp* (sender transmit, both NICs,
fabric, receiving driver) and *RX delivery* (that stamp to our `recv` returning). **RX delivery is two
readings of one clock on one host**, so unlike the rest of the wire leg it needs no synchronisation and
is exact, and no privilege is required. Every timestamp in this document is a software timestamp;
hardware timestamping was not attempted, so the driver-and-fabric portion stays opaque.

Measured at four rates, each twice — with timestamping on and off — because asking for timestamps turns
`recv()` into `recvmsg()` with control data and perturbs what it measures. Medians in ns, one receiver
except the last row:

| rate | shm | wire | to RX stamp | RX delivery | RX share | wire, no timestamps | cost of measuring |
|---|---|---|---|---|---|---|---|
| 200k | 292 | 33,744 | 32,836 | **908** | 2.7% | 32,733 | 1,011 |
| 500k | 289 | 32,258 | 30,223 | **2,035** | 6.3% | 31,823 | 435 |
| 1M | 740 | 65,445 | 46,571 | **18,874** | **28.8%** | 58,752 | 6,693 |
| 2M | 853 | 55,812 | 39,021 | **16,791** | 30.1% | 41,689 | 14,123 |
| 200k, 10 receivers | 5,967 | 34,103 | 33,132 | 971 | 2.9% | — | — |

**Result.** The receiving kernel is negligible where this task centres and substantial only
at the top of the range. At 200k it is 908 ns of a 33.7 µs path, and the measurement costs
1,011 ns — more than the thing it measures, which is itself the answer.

The high-rate figures need their measurement cost subtracted honestly. If the whole timestamping
overhead landed inside the leg measured, RX delivery at 1M would be 18,874 − 6,693 = 12.2 µs, so the
true value lies between **12.2 and 18.9 µs**, 21–29% of the wire leg. At 2M the overhead is 14.1 µs
against a 16.8 µs reading, bracketing it between 2.7 and 16.8 µs — too wide to support a claim, so
none is made. **The 1M point is the only high-rate figure this experiment supports.**

Fan-out changes none of it: at ten destinations the source-ring wait grows from 292 to 5,967 ns —
replication cost, exactly where the send-mechanism work put it — while RX delivery stays at 971 ns.

**And the burst events are mostly not there either.** Taking the worst 0.01% of wire legs and asking
how much of their excess over the median sits in RX delivery:

| rate | wire excess during a spike | of which RX delivery | share | burst size |
|---|---|---|---|---|
| 200k | 2,117,687 | 761,636 | 36% | 206 messages |
| 500k | 247,558 | 75,481 | 31% | 86 messages |
| 1M | 3,793,591 | **−6,999** | **−0.2%** | 625 messages |

At 200k and 500k about a third of a spike is the receiving kernel stalling — 908 ns rising to 762 µs,
an 800-fold jump. At 1M the spikes are **entirely upstream**: RX delivery during a spike is *lower*
than its own median, 11.9 µs against 18.9. Which follows from what a spike is at that rate — a
datagram held up before the receiver, arriving at a receiver that is consequently idle.

**Verdict.** The AF_XDP receive path is not worth its privileged step: at the rates that matter it
could win about a microsecond of thirty-four, and at 1M the events that dominate the far tail are
upstream of it and would survive the change.

### An artefact removed rather than reported

The N=10 run first produced an RX delivery p99.9 of **117 ms**. Every value above a millisecond — all
39,284 of them — sat in rows 0 to 39,283 of the stage file and nowhere else. The receiver preallocates
and page-touches its per-stage buffer at startup, hundreds of megabytes when sized for a high-rate
run, and datagrams queue behind that: an artefact of the instrumentation, not the transport. The
aggregation now drops one percent of each stage file and the corrected p99.9 is **9.5 µs**. The fixed
16k-sample drop the latency series uses is not enough for a file whose opening rows are contaminated
by the measurement's own allocation.

## Experiment 2: latency and clock offset without trusting the clocks

**Question.** Every cross-host figure above is computed as

```
wire leg = (receiver's stamp, receiver's clock) − (sender's stamp, sender's clock)
```

which is the true transit time *plus* whatever offset separates the two clocks. The same
configuration measured 29,920, 31,839, 32,836 and 36,014 ns in four sessions — a 6 µs spread
— and there was no way to tell whether the path had changed or the clocks had drifted. Chrony
reports a couple of hundred nanoseconds of dispersion, but that is an estimate of its own
jitter, not a bound on the systematic offset between two machines.

**Method.** `tools/clock_probe.cpp`. A round trip is measured entirely on one clock and the
reflector's turnaround entirely on the other, so neither quantity depends on the two
agreeing:

```
initiator: t0 = now                        (initiator clock)
reflector: t1 = now on arrival             (reflector clock)
reflector: t2 = now just before replying   (reflector clock)
initiator: t3 = now on the reply           (initiator clock)

round trip    rtt    = t3 − t0      one clock, exact
turnaround    turn   = t2 − t1      one clock, exact
one way       ow     = (rtt − turn) / 2
clock offset  offset = (t1 − t0) − ow
```

A ping-pong with one datagram outstanding, so it measures the path rather than queueing behind our
own traffic, and both ends busy-poll — the turnaround is subtracted from the round trip, so a
scheduler wake-up in the reflector would land directly in the one-way figure. Its median turnaround
was 19–28 ns, so that concern is settled.

One reflector cannot separate clock offset from path asymmetry, so the probe was also run in reverse:
an offset measured from A is `true_offset + asym/2`, from B it is `−true_offset + asym/2`. Half the
difference is the offset, half the sum the asymmetry.

**Results.** Four measurements over roughly an hour:

| # | direction | round trip | turnaround | one way | clock offset |
|---|---|---|---|---|---|
| 1 | A→B | 84,606 | 28 | 42,289 | **−6,281** |
| 2 | A→B | 85,177 | 28 | 42,574 | **−2,783** |
| 3 | B→A | 84,483 | 19 | 42,232 | **+2,489** |
| 4 | A→B | 85,908 | 28 | 42,940 | **+228** |

From the bidirectional pair (2 and 3): **true clock offset = −2,636 ns** (the receiver's clock reads
behind the sender's) and **path asymmetry = −294 ns total**, so the path is symmetric to within about
150 ns each way and RTT/2 is a sound estimator.

The two quantities behave completely differently. **One-way latency is stable — 42,232 to 42,940 ns,
a spread of 708 ns (1.7%)** across four measurements and both directions. **The clock offset wanders
across 8,770 ns**, from −6,281 to +2,489, on a timescale of minutes to tens of minutes.

**The path is steady. The clocks are not.**

## What this invalidates

The relay's cross-host figures equal true transit *plus* an offset that wanders by roughly ±4 µs
around zero. Three consequences, and they are why this document exists.

**1. It explains the io_uring reversal mechanically.** The same two configurations measured −3,005 ns
and +2,909 ns in two sessions, which was noticed empirically. It is now explained: an 8.8 µs wander is
larger than the ~3 µs being compared, so a design measuring one backend then the next cannot resolve
it — and neither can an interleaved one unless the interleaving is fast relative to the drift.

**2. Absolute cross-host latency carries ±4 µs of clock noise** unless the offset is measured in the
same window. The reported 200k median of roughly 34 µs is right to within a few microseconds and no
better, and should be read that way.

**3. The single-clock legs are unaffected and remain exact.** `shm` (292 ns), `RX delivery` (908 ns)
and `publish` (51 ns) are each two readings of one clock on one host, and the frames-per-datagram
drain measure is likewise clock-free — which is precisely why it reproduced to a few percent across
twelve runs and three sessions while the medians reversed. **Prefer clock-free metrics for
comparisons; treat cross-host absolutes as ±4 µs.**

A note on the claim that the hosts are synchronised "within 3 microseconds": that is consistent with
what was measured, but it is the *bound*, not the accuracy, and a bound of that size is larger than
most of the differences the backend comparisons were trying to resolve.

## Two approaches that failed, and why

Both were wrong in ways worth recording, because each was a plausible design.

**Measuring the offset concurrently with the load does not work.** Running the probe *during* the
relay run would give an offset from the same window. Under a 200k msg/s one-way load it reported
**−68,750 ns** against −2,783 idle, while the probe's own round trip went from 85 µs to **217 µs**.
RTT/2 recovers the offset only if the path is symmetric, and a one-way load is exactly what breaks
that: half the load-induced queueing leaks straight into the "offset". Bracketing the run with idle
probes either side, and taking their difference as the uncertainty, is the workable form.

**Two busy-polling sockets on one NAPI do each other's receive work.** `napi_busy_loop` polls the
*device queue*, not the socket, so the probe's reflector was processing the relay's 200,000 packets a
second as well as its own — most of why its round trip tripled. That disqualifies any probe sharing a
NAPI with the traffic it measures around, and it is also a real fan-out finding: the receiving host has
8 combined queues and the ten-receiver configuration runs 10 busy-polling receivers, so at least two
share a queue and poll on each other's behalf. Giving each receiver its own queue is worth measuring.

## Experiment 3: correcting an absolute figure, by bracketing

**Method.** The offset cannot be measured *during* the run, so each configuration is bracketed: idle
probe, run, idle probe. The mean of the two is the correction, their difference its uncertainty.

At 200k msg/s with one receiver the offset read −4,297 ns before and −3,168 after, giving a correction
of **−3,733 ns** and a **drift across the run of 1,129 ns**. So the wire leg goes from 32,464 as
measured to **36,197 corrected**, and end to end from 32,798 to **36,531 ± 564**.

Two things follow. **The corrected 200k median is 36.5 µs, not the ~34 µs reported earlier** — those
figures were understated because the receiving clock happened to be behind. And **the offset drifts only
1.1 µs across a two-minute run against 8.8 µs across an hour**, so bracketing turns a ±4 µs uncertainty
into ±0.6 µs — precise enough to compare configurations differing by a few percent, which no earlier
cross-session comparison could do.

## Experiment 4: the software transmit stamp is unusable

**Question.** The sending kernel's protocol path looked separable the same way the receiving one was
— single clock, exact, no privilege.

**Method.** `SOF_TIMESTAMPING_TX_SOFTWARE` with `OPT_ID` and `OPT_TSONLY`. The kernel reports
transmit stamps on the socket's error queue, labelled with a per-send counter, so the relay records
when it handed each datagram over and differences the two. Draining happens in the relay's idle
branch, on the same principle as the redundant copy: only when nothing waits.

**Result: the stamp is not taken where it needs to be.** Over 2,097,152 matched samples at 200k
msg/s, with nothing unmatched or expired, p50 is 45,776 ns and p99/p99.9 are 70,967 and 71,173.

**This cannot be a transmit-path measurement, and the proof is arithmetic.** It claims 45.8 µs from
our pre-send stamp to the kernel's "transmit" stamp, but the corrected wire leg — pre-send all the way
to the *receiver's* arrival stamp — is 36.2 µs. A stamp on the path cannot come after the arrival.

The shape says what it is instead. p99 and p99.9 sit 206 ns apart: a ceiling, not a tail. Transmit
completions are coalesced on a timer of the same order, and the plateau lands just above it. So the
software transmit stamp reflects *completion processing* rather than the moment the frame reached the
driver, and it cannot split the leg.

The clock bases were checked before concluding that: `SOF_TIMESTAMPING_SOFTWARE` reports
`CLOCK_REALTIME` and the relay uses `std::chrono::system_clock`, the same base, so the interval is
genuine in units even though meaningless in interpretation.

**Consequence.** Both *software* routes into the remaining 32.8 µs are now exhausted — the receive
side because it can only reach its own kernel boundary, the transmit side because its stamp is taken
too late.

## Experiment 5: a receive path whose polling never stops

**Question.** Busy-poll happens only while the thread is inside `recv`, so a single-threaded
receiver stops polling exactly when it falls behind — and delivery then falls back to a softirq on a
housekeeping core, measured 24 µs worse at the median. Falling behind should therefore make delivery
slower and cause it to fall further behind. Experiment 1 found the signature: at 200k the receiving
kernel's delivery leg rises from 908 ns to 762 µs during a spike, while at 1M — where the receiver is
saturated and always inside `recv` — spikes carry no delivery excess at all.

**Method.** `transport/include/poll_split.h`. One thread does nothing but receive and hand datagrams
to a second over a bounded SPSC ring. Arrival stamps are taken in the *polling* thread, so the
queueing this introduces cannot be folded into the latency it is meant to protect.

**The mechanism works, and the hypothesis is refuted.** Both configurations at 200k msg/s with one
receiver, gate suppressing nothing:

| leg | single-threaded | split-poll |
|---|---|---|
| **ring publish p50** | **52** | **505** |
| receiving-kernel delivery p99 | 2,058 | 2,236 |
| receiving-kernel delivery p99.9 | 6,770 | 9,734 |
| end-to-end p50 | 31,817 | 31,810 |
| end-to-end p99 | 37,973 | 38,956 |

**The queue says why it cannot help: its high-water mark was 4 slots of 8,192.** The publisher never
fell behind, so the loop was never absent from `recv` long enough to matter — the feedback loop this
design exists to break was not active. With nothing to win, only the cost shows: the publish leg goes
52 → 505 ns, and p50 and p99 end to end are unchanged. The far-tail difference is **not** attributed;
one run each cannot separate a regression from which run caught a drain event, and this baseline
caught none (its delivery leg peaked at 108 µs against the 762 µs of Experiment 1).

**Verdict: not shipped.** The premise held in the Experiment 1 data and not here, so the design
targets a regime this workload does not enter at 200k with one receiver.

### The first attempt was invalid, and the reason is worth keeping

Every configuration after the first showed the gate suppressing 56–64% of arriving frames while
reporting zero sequence gaps — 11,263,800 suppressed against 7,437,621 published on one run.

**The cause was two senders, and the counter that proves it is `datagram reorder`**, which came to
11,995,603 against 11,995,625 suppressed on one run: equal to within a handful. That counter
increments when a first-copy datagram arrives carrying a `pkt_seq` the receiver has already passed,
which one monotonically numbering sender cannot produce. Two senders on one producer ring can — each
numbers its own datagrams from zero, so the streams interleave and whichever is behind reads as
reordered, while the frames inside carry identical `seq_id`s and are correctly suppressed.

The two senders came from the driver, not the transport: its remote launches went through a retry
wrapper, and **retrying a launch is not idempotent.** The tempting alternative explanation —
producer sequence origins restarting at zero — is ruled out by the ordering, since the receiver binds
some thirty seconds after the previous stream has stopped. It is worth recording because it was
plausible enough to act on, and acting on it would have meant changing the harness's sequence
numbering to work around a driver bug.

The transport-level lesson stands regardless: **a monotonic gate absorbs stream contamination
silently.** It does exactly what it should — no gaps, nothing duplicated downstream — so nothing in
the latency output looks wrong, and only the suppression and reorder counters give it away. Both are
now reported on every run, the harness refuses to start while a previous run's process is alive, and
a run whose gate suppressed anything while duplication was disabled is marked contaminated rather
than analysed.

## Experiment 6: path diversity, measured twice, and mostly retracted

`--dual-path` sends every datagram twice from **distinct source ports** to the same destination port.
That choice is the point: the receiver needs no second socket and no second thread, because what
differs is the four-tuple that receive-side steering and equal-cost path selection hash on, not the
address the datagram arrives at. `connect()` on an unbound datagram socket binds an ephemeral port, so
listing each peer twice is the whole implementation. No new delivery logic was needed either — the
monotonic gate already publishes whichever copy arrives first, which is the same strict monotonicity
that ruled out retransmission and FEC. It costs twice the packet rate, trading directly against
fan-out.

Measured cross-host at 200k msg/s with one receiver against a baseline re-run in the same window. The
gate suppressed 8,248,791 copies against 8,250,344 published — one rejection per publication, so the
mechanism genuinely ran.

**That measurement was then repeated in a second window, and two of its three headline claims
did not survive.** The repetition is the whole content of this section, so it comes first:

| | session 1 | session 2 | replicates? |
|---|---|---|---|
| p50 | +3,303 (1.10×) | **−1,183 (0.97×)** | **no — sign flips** |
| p99 | +6,371 (1.16×) | +4,495 (1.11×) | yes |
| p99.9 | +290 (1.01×) | +11,744 (1.23×) | mixed |
| p99.99 | −762,077 (**0.17×**) | +177,967 (**3.19×**) | **no — sign flips** |

**Retracted: the 10% median cost.** Session 1 showed dual path 3.3 µs *slower* at p50 with
non-overlapping per-rep spreads; session 2 shows it 1.2 µs *faster*, also with non-overlapping
spreads. Per-rep spread measures variation *inside* a window and is blind to the offset drift
*between* two configurations' windows — and that drift was separately measured at 8.8 µs over an
hour, several times the effect. This is the same trap that reversed the io_uring comparison, and
citing non-overlapping spreads was not the protection against it that I took it for.

**Retracted: the 5.8× improvement at p99.99.** It rested on one baseline whose p99.99 was
921 µs. Across three baseline runs that figure is 81,133 / 921,223 / 1,172,844 ns — a **14.5×
spread between runs of an identical configuration**. A single pairing therefore reveals which
window it sampled, not which configuration is better.

**What replicates is the p99 cost, and it is the receiving kernel.** +6,371 ns and +4,495 ns end to
end across two sessions; and on the receiver's own clock, needing no offset correction, the delivery
leg goes 1,518 → 7,437 ns and 2,041 → 7,438 — accounting for essentially the whole end-to-end p99
penalty. The median receive cost is 905 → 949 ns, i.e. nothing: every published message now arrives
with a twin that still had to be received, timestamped and gated, and that only tells when the loop
falls behind.

**On the far tail, the honest position is "suggestive, not established".** Pooling all nine runs, the
three no-redundancy p99.99 values are 81,133 / 921,223 / 1,172,844 (a 14.5× spread) against six
redundancy values from 74,961 to 368,922 (4.9×). Every redundancy run lands below the two worst
baselines and the baseline is worse in 13 of 18 pairwise comparisons — but an exact one-sided
Mann–Whitney test gives **p = 0.19**, so it does not reach significance. The direction is consistent
with two draws from the tail beating one; the evidence is not strong enough to assert it.

**Verdict: not a default, and not a justified switch.** The p99 cost is real, reproducible and
located; the far-tail benefit that would pay for it is not established. What was previously written
here as a measured trade was one window's noise on the cost side and one window's luck on the benefit
side. One thing this cannot separate: distinct source ports give both a possibly distinct path *and* a
distinct moment, and the millisecond-versus-microseconds argument makes the path explanation the
stronger one without directly observing two paths.

## Experiment 7: the redundancy penalty is on the receive side, and no send mechanism touches it

Experiment 6 leaves one cost worth attacking: a reproducible ~5–6 µs at p99, all of it in the
receiving kernel. The obvious suspicion was the *sending* side — a redundant copy sitting ahead of a
real message in a shared transmit queue — and that had an obvious remedy in the two backends already
built. So the copy was given, in turn, every submission mechanism available.

`--dup-path` is the vehicle, combining what the two existing mechanisms each had half of:
`--dual-path` gave the copy a distinct four-tuple but submitted it inline, while opportunistic
duplication scheduled it correctly — only from the branch where the source ring was empty, so it can
never delay a real message — but sent it on the *same* sockets. A dedicated leg with its own sockets,
written only into idle time, has both, and `--dup-backend` makes that leg a different mechanism from
the primary.

The point of doing it that way: **io_uring and AF_XDP were both rejected for the primary path on
grounds that do not apply to a purely redundant one.** AF_XDP's 4.6 µs end-to-end penalty is
irrelevant on a leg whose only job is to beat a millisecond-scale drain, and it transmits through
`dev_direct_xmit` — no qdisc, its own bound queue — so it shares neither software nor hardware queue
with the primary. io_uring with SQPOLL removes the last system call from the relay thread, and
SQPOLL's one known defect, an unexplained drop burst, is harmless when a dropped copy costs nothing.

The result, on the receiver's own clock so no offset correction is possible or needed:

| copy carried by | shares a qdisc with the primary | receiving-kernel delivery p99 |
|---|---|---|
| *nothing — no redundancy* | — | 1,518 / 2,041 |
| kernel UDP, inline on the primary sockets | yes | 7,437 |
| kernel UDP, its own sockets, idle time only | yes | 7,537 / 7,438 |
| io_uring + SQPOLL, its own ring | yes | 7,638 |
| **AF_XDP, its own transmit queue, no qdisc** | **no** | **7,401** |

**237 ns of spread — 3.2% — across four unrelated submission mechanisms, on a penalty of about
5,400–6,100 ns.** AF_XDP is the decisive row: it shares no software queue and no hardware queue
with the primary, and it performs identically to submitting both copies back-to-back down the same
socket. Whatever the penalty is, it is not transmit-side contention, and no amount of bypass on
the send path will reach it.

That is the hypothesis refuted about as cleanly as this equipment allows, and it disposes of a
secondary one too: moving the copy into idle time (7,537 against 7,437 inline) did not de-burst
arrivals into a shallower socket queue either. The receiver simply has twice as many datagrams to take
delivery of, and that is the cost.

### Then can the receive path absorb it?

Doubling the datagram rate doubles the number of `recv` transitions, and io_uring's multishot receive
reaps many datagrams per `enter()` — the one shape with a call to save at 2×, and the reason the
earlier verdict ("no call to save, because both shapes cost one transition per datagram") might not
hold here: that was measured at 1×.

| receive path | redundancy off | redundancy on | penalty |
|---|---|---|---|
| kernel UDP + `SO_BUSY_POLL`, p99 | 40,743 | 45,375 | +4,632 |
| io_uring multishot + provided buffers, p99 | 89,811 | 89,712 | **−99** |

io_uring is, on its face, completely immune. It is not a fix: its absolute p99 is **1.98× worse** than
the kernel path's *with redundancy already enabled*. The penalty is masked by a much larger constant
cost — the receive path is already limited by something other than transition count — so the earlier
verdict stands, for a sharper reason than before. (`rx_delivery` is unavailable on the io_uring
receiver, so this compares end-to-end deltas within each backend, which cancels any offset common to
that backend's two adjacent runs.)

### What is actually left

The cost is one socket, one thread, one core taking delivery of twice the datagrams, and both routes
to it are outside what has been tried. **More receive parallelism** — a second socket on a second
port, polled on its own core, with the gate merging both streams — is non-privileged, but needs the
gate to become safe for two publishers, and Experiment 5 is a warning about adding a handoff to this
path. **Or drop the loser before it reaches userspace:** the monotonic gate is a single comparison
against one number, expressible as an XDP program over a BPF map, so the duplicate would be discarded
in the driver's poll routine before skb allocation and the userspace cost would return to baseline
exactly. That is the one place XDP genuinely fits this problem, and it needs a program attached to the
interface — an operator action.

## Experiment 8: auditing the measurement instead of the transport

Two results here had to be withdrawn, both because of the measurement rather than the code. That is
a reason to audit the measurement the way the transport has been audited. Four defects, in rough
order of damage.

### 1. The percentile was computed wrongly, and the error was up to 719%

Both analysis paths reduced a configuration to *the median of its per-file percentiles*. That reads like
a robust estimator and is the wrong tool for a tail: the median across files discards precisely the files
holding the rare events. Against pooling, on four real ten-consumer runs, it understated p99.99 by
**1.4×, 1.7×, 3.4× and 8.2×**. `max` was worse, because the median of per-file maxima is not a maximum at
all: one configuration reported 155,450 ns where the largest sample was **18,144,863 ns**, a factor of
115 — checked independently of the analysis code with `sort -n` on the raw column before anything changed.

Both paths now pool, computing the rank *without* concatenating (binary search over value space using
`searchsorted` on the already-sorted per-file arrays, since pooling the largest configurations costs
gigabytes) and verified exact against a sorted pool on 1,800 randomised cases. Every aggregate was
regenerated: the median change at p50 through p99.999 was **0.0%**, with corrections confined to
p99.9-and-beyond of multi-file configurations. One derived claim moved two points (io_uring receive costs
69% at the median, not 71%). Two io_uring tables needed no change, because they already used
per-repetition pooling when the same defect surfaced in that comparison — so the fix brought the
aggregates into agreement with what the write-up already said.

### 2. The uncertainty column measured the wrong thing

A spread across repetitions of one run describes variation *inside one window*. It is blind to the
two things that actually move a comparison: the clocks drifting apart between windows, and the
environment's own between-window mood. Both withdrawn claims cited non-overlapping per-repetition
spreads as evidence. Those columns are now labelled `within-session`, and a single-repetition run
says so explicitly rather than printing `lo == hi` as though that were precision.

### 3. The right design was documented and never implemented

The methodology section of the write-up already said configurations must be interleaved rather than
run one after another. Every runner did the opposite. `scripts/ab_bench.sh` now implements it: each
arm once per block, blocks repeated, and **the arm order reversed on alternate blocks** so the linear
component of any drift cancels across a pair. `scripts/paired_stats.py` reports the per-block
differences and — the column whose absence caused both retractions — how far the reference arm alone
moves between blocks. An effect smaller than that is not resolved.

Fed the two withdrawn results, the tool rejects both: the median claim on an inconsistent sign
(p = 1.000, range −1,183..+3,303) and the p99.99 claim because the effect, large as it was, is
smaller than the reference arm's own 1,091,711 ns of between-block wander.

**And a floor falls out of the arithmetic.** The verdict rests on an exact two-sided sign test, which
needs no assumption about the shape of the noise. With every block agreeing the smallest attainable p
is 2/2ⁿ, so three blocks bottom out at 0.250 and five at 0.062: **a three-block comparison cannot
produce a significant result however large and consistent the effect.** Six is the first n that can.
Every comparison in this document before now used one or two.

### 4. The harness failed silently, in ways that looked like data

Four distinct defects, each of which had already produced a plausible-looking measurement. **A launch
that ran twice**, because treating an unreadable marker file as "did not launch" starts a second runner on
a flaky link — and two senders on one source ring produce a stream the gate half-suppresses while nothing
in the latency output looks wrong. **A launch that ran twice for the opposite reason**: a process-list
check followed by a six-second wait is reliably too short, since `bench.sh` runs a core check and a
stale-process preflight before spawning its first worker, so it saw zero, launched again, and the second
runner died on the first one's processes — which silently lost the AF_XDP arm twice. **A verification that
raced a short run**, discarding one block that had received 3,207,957 datagrams because the poll arrived
after the run finished. And **a start order that measured an empty stream**: the receiver's warm-up counts
from its own start, so a sender still in its preflight when that expires yields a measurement of nothing —
one block recorded 5,682,280 datagrams sent against 0 received.

The rule that survived all four: launch at most once per call, never let "I could not tell" mean "launch",
and route the launch through a *single-attempt* ssh — fixing the decision logic while the transport
underneath still retried the command fixed nothing, which is why this defect recurred three times. A run
that did not happen is now written to the results file as an invalid row with a reason.

### 5. The design's own assumption is measured

Blocking only helps if a block's two arms are close together in time, which is a property of the control
link rather than a guarantee. On a link costing ~800 ms per round trip, an early version made about
forty round trips per arm and stretched a 90-second measurement to eleven minutes — at which point the
block is a sequential comparison wearing a block's name. It now makes about ten, records the wall-clock
second each arm finished, and reports the within-block spacing.

### Does the machinery earn its complexity?

The pieces are verified individually: the pooled rank against a sorted pool, the row builder against
all seven of its failure modes, and the paired statistics against the two withdrawn claims, which it
rejects. But the test that matters is whether the design produces a verdict on a real question, and
Experiment 9 runs it end to end: six interleaved blocks, **twelve of twelve arm-runs valid**, arms two
minutes apart, and a resolved answer on a question three earlier sequential attempts had left
ambiguous.

## Experiment 9: a routed long-haul path, where the design's real risk lives

Everything above was measured on a directly-attached path: one hop, ~35 µs one way. A second
host pair was then measured over a **routed inter-region path** — ~35 *milliseconds* one way,
three orders of magnitude longer. That inverts what is worth measuring. The transport's own
cost, about 620 ns, is 0.002% of this path and cannot be seen at all. What can be seen is
whether the *delivery rule* survives, and that is the part of the design most exposed.

### Reaching the path at all was the first finding

The outgoing link on these hosts is chosen by **source address**, not by destination prefix: each
uplink has its own routing table and an `ip rule` per local address that selects it. A socket that
does not bind a source leaves by the main table — the management interface, not the link under test.
Bound to the *interface* the peer was unreachable (100% loss); bound to the source *address* the same
peer answered in 69.7 ms.

Nothing in the transport could express that, so `--src-addr` was added, applied in the shared socket
setup so the kernel-UDP and io_uring senders cannot disagree. A source that cannot be assigned is a
loud failure rather than a silent fallback, because sending from the wrong address means measuring a
different network than the one named. AF_XDP rejects the option instead of ignoring it, since it
writes its own IP header from the interface.

### The delivery rule holds: no reordering at all

The gate publishes only strictly increasing sequence ids, so anything arriving out of order is
**dropped, not delivered late**. On a one-hop path that costs nothing; on a routed multi-path network
it could discard a real fraction of the stream, and that was the open risk.

| offered rate | datagrams | reordered | gate suppressed | first-copy loss | mean burst |
|---|---|---|---|---|---|
| 1,000 | 33,505 | **0** | 0 | 0.0119% | 1 |
| 10,000 | 285,250 | **0** | 0 | 0.0466% | 133 |
| 50,000 | 1,230,350 | **0** | 0 | 0.1789% | 20.6 |

**Zero reordering across 1,549,105 datagrams**, so the strictness that rules out retransmission and
forward error correction costs nothing here either. That is the most important thing this pair had to
answer. Path MTU is also exactly 1500 end to end (1472 + DF passes, 1473 fails), so the
no-fragmentation assumption holds on a routed path and not only on an on-link one.

### Latency is dominated by distance, and gets *better* under load

| rate | one-way p50 | p99 − p50 | p99.9 − p50 | receiving-kernel delivery p50 |
|---|---|---|---|---|
| 1,000 | 35.20 ms | 357 µs | 1,219 µs | 3,470 ns |
| 10,000 | 34.01 ms | 1,670 µs | 6,855 µs | 3,381 ns |
| 50,000 | 33.54 ms | 267 µs | 763 µs | 1,197 ns |

The highest rate has the *lowest* median and the *tightest* tail — the same effect as on the short path
and for the same reason: at 1,000 msg/s messages arrive 1 ms apart and every one meets a cold cache,
while at 50,000 the receiver is warm and already spinning.

Two cross-checks. The measured one-way p50 of 34.77 ms sits within 0.3% of half the ICMP round trip
(34.85 ms), an independent confirmation that the clock offset is negligible *at this scale*; and the
offset measured directly at 27,155 ns — about 30× the short pair's, but 0.08% of a 33.5 ms path.
**Long haul is the one regime where absolute one-way latency is trustworthy**, because the quantity
measured finally dwarfs the uncertainty in the clocks. The reverse warning also applies: between-run
variation of the *median* was ~1.7 ms, so comparisons here need the block design even more than the
short path does.

### One counter had to be fixed before redundancy could be measured at all

The receiver reported "rescued: datagrams the original never delivered", and on the first
redundancy run it read **1,074,810 of 1,124,242** — obviously not recoveries. When both copies share
a socket the copy is always sent second along the same path, so a copy that passes the gate does
imply the original never came. Once the copy has a four-tuple of its own it can simply be *faster*,
winning the race while the original arrives moments later and is suppressed. The counter was
measuring path diversity, not recovery. It now reads "copies admitted before their original", and
delivery loss is taken from the gate's sequence gaps — the only figure that answers what the consumer
did not get.

### The six-block comparison, and what it resolves

Run with the block design of Experiment 8, at 50,000 msg/s. **Twelve of twelve arm-runs valid, and a
block's two arms two minutes apart** — so the assumption blocking depends on held, and is reported
rather than assumed.

| metric | reference (off) | on − off | n | sign p | verdict |
|---|---|---|---|---|---|
| first-copy loss | 0.1558% | **+0.0129 pp** (+0.0121..+0.0361) | 6 | 0.031 | **resolved** |
| frames never delivered | 1,920 | **+170** (+135..+217) | 6 | 0.031 | **resolved** |
| receiving-kernel delivery p50 | 1,214 ns | +66 | 6 | 0.688 | not resolved |
| receiving-kernel delivery p99 | 3,059 ns | −136 | 6 | 1.000 | not resolved |
| wire leg p50 | 34.20 ms | +77,528 ns | 6 | 0.688 | not resolved |
| end-to-end p50 | 34.20 ms | +77,574 ns | 6 | 0.688 | not resolved |
| end-to-end p99 | 34.36 ms | +48,792 ns | 6 | 0.688 | not resolved |
| end-to-end p99.9 | 35.19 ms | −654,650 ns | 6 | 0.688 | not resolved |
| end-to-end p99.99 | 36.30 ms | −755,682 ns | 6 | 1.000 | not resolved |

**The two resolved rows are the two that need no clock.** Loss and delivery are counters on the
receiving host, and both move the same way in all six blocks: redundancy costs about 9% more
undelivered frames. Nothing on the latency side separates, in either direction — and the reason is on
the table: the reference arm's own median moves **1.1 ms between blocks** and its p99.99 moves 4.5 ms,
so a 78 µs median difference is invisible and the honest report is "not resolved" rather than a number
with a sign. This is the same test that rejected the two withdrawn claims, applied where the noise is a
thousand times larger.

One incidental finding from the blocks. `copies admitted before their original` ranged from 1,732 to
1,227,823 across the six — from essentially never to essentially always. Which of the two four-tuples
is faster is not a property of the configuration but of routing at that moment, which is worth knowing
before treating path diversity as a stable latency mechanism anywhere.

**And one more harness defect, the same one a third time.** `launch_once` was made idempotent in its
decision logic but still issued the launch through the retrying ssh wrapper, so a slow connection
retried the *command* and started a second runner. Fixing the decision without the transport underneath
fixed nothing; launches now go through a single-attempt ssh, and the six blocks above are the first run
in which every arm produced a valid measurement.

## Where this leaves the design

- **The 32.8 µs is one number, and both software routes into it are exhausted.** Everything here is a
  software stamp, and the transmit stamp is taken at completion rather than transmit (Experiment 4).
  What remains is hardware timestamping, or an indirect method: a reflector on a third host would let
  the two path segments be compared, and a *bidirectional* load would make a concurrent offset probe
  valid again by restoring the symmetry RTT/2 depends on.
- **Redundancy across four-tuples is resolved against on the routed path** (Experiment 9: +170
  undelivered frames, sign p = 0.031, six blocks) and unresolved on the direct one, where the far-tail
  direction favours it at p = 0.19. The paths differ in one variable — burst length relative to the gap
  between a datagram and its copy — and that is the variable to sweep. A shaped-loss rig with
  controllable burst length would answer it directly, and would also say where a temporal stagger
  starts to pay.
- **The receive-side penalty is measured and not yet reduced**: ~5-6 µs at p99, entirely inside the
  receiving kernel and identical across four submission mechanisms, so it is the cost of taking
  delivery of twice the datagrams rather than of how they are sent. Two routes remain — a second
  receive socket on its own core, and an XDP program running the gate so the loser never reaches
  userspace.
- **Path diversity is not a stable latency mechanism.** Across six blocks the share of copies arriving
  before their original ranged from 1,732 to 1,227,823. Which four-tuple is faster is a property of
  routing at that moment, not of the configuration.
- **The block design's reach is bounded by the path's own variability**, and it reports that bound: on
  the routed path the reference arm's median moves 1.1 ms between blocks, so smaller effects are not
  resolvable there by any number of blocks that fits in an afternoon. Where a question needs finer
  resolution than the path allows, the answer is a single-clock metric — loss, delivery, a same-host
  leg — not more repetitions.
- **Per-receiver queue steering**, per the NAPI finding above. Needs `ethtool`, so it is an operator
  action rather than one of ours.
