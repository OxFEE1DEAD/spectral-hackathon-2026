# Fan-out transport — decisions, evidence, and what is missing

## Where this starts

This repository is a fork of the task project with the published Claude-agent
baseline merged into it, `agent-solution` at commit
`c86cc26ab2e84996137f922cf175d33e9a622c29`. The task statement offers that baseline
for exactly this use. Merging rather than copying keeps its history attached, so the
diff from `c86cc26` to `HEAD` is this repository's own contribution and nothing else:

```bash
git diff c86cc26 HEAD -- transport scripts
```

`harness/` is byte-identical to `c86cc26` and was never touched. Upstream's write-up,
its notebook and its `data/plots/` are deliberately absent: they are public at that
commit, and **no number in this repository comes from them.** Everything in `data/` is
our own, written by `scripts/ab_bench.sh`, one row per arm-run, committed unedited
including the runs marked invalid.

## The bench, and why absolute numbers are absent

Two `m7i.2xlarge`, one subnet, one availability zone, cluster placement group, Ubuntu
24.04, SMT off, `isolcpus/nohz_full/rcu_nocbs=1,2,3`, IRQs pinned to core 0. Terraform for
the whole thing is in `infra/`.

The baseline was measured on `m7i.metal-24xl` sending to `m7i.24xlarge` — 96 vCPU against
our 8. **No absolute latency from this work is comparable to the baseline's**, and none is
compared. Everything claimed here is a within-block A/B measured on one bench against a
control arm that reproduces the baseline's behaviour in the same binary, minutes apart.

Two hosts' clocks are also not stable enough to publish a cross-host absolute figure. An
idle `clock_probe` bracketing each block put the offset movement between two arms of one
block at up to ±1,400 ns — the same order as the p99 difference being looked for. That
number is measured, not assumed, and it is why several results below are reported rather
than claimed.

## The numbers

End-to-end latency in nanoseconds, median across blocks, this bench, `--type mixed`:

| | min | p50 | p99 | p99.9 | p99.99 | max |
|---|---|---|---|---|---|---|
| 100k msg/s, baseline | 9,978 | 12,130 | 19,984 | 35,670 | 965,068 | 2,084,559 |
| 100k msg/s, **ours** | 9,534 | 11,977 | **18,397** | **30,744** | **57,032** | **220,408** |
| 200k msg/s, baseline | 11,624 | 14,894 | 27,245 | 56,568 | 1,346,343 | 1,546,106 |
| 200k msg/s, **ours** | 12,795 | 15,334 | **26,173** | **46,420** | **98,829** | **258,656** |
| 500k msg/s, baseline | 14,318 | 20,420 | 37,879 | 61,358 | 127,888 | 181,760 |
| 500k msg/s, ours | 14,808 | 20,956 | 41,438 | 65,688 | 127,691 | 227,590 |

**Read these as this bench's numbers, not as a comparison with the baseline's published
figures.** An end-to-end value is `receiver clock − sender clock`, so it carries whatever
constant separates the two hosts; the offset between two arms of a single block was
measured moving by up to ±1,400 ns, and its absolute size is unknown. The baseline was
measured on `m7i.metal-24xl` sending to `m7i.24xlarge`, 96 vCPU against our 8. The rows
above are directly comparable to each other because both arms ran minutes apart in the same
block on the same pair of machines, and to nothing else.

What the table shows and the deltas below confirm: the gain is concentrated **above p99**,
it is largest at p99.99 where it is more than an order of magnitude, and p50 does not move.
That is the shape the mechanism predicts — the changes remove datagrams from the receive
path and remove a self-inflicted hole, neither of which touches the median.

## What is claimed

Every row is a counterbalanced block A/B against a control arm that reproduces the
baseline's behaviour in the same binary. `p` is a two-sided sign test over blocks; its
floor is `2 / 2^N`, so six blocks cannot go below 0.031 and twelve reach 0.0005.

| | | blocks | p |
|---|---|---|---|
| first-copy loss, 100k msg/s | −0.78 pp | 12/12 | 0.000 |
| end-to-end p99, 100k | −2,046 ns | 10/12 | 0.039 |
| end-to-end p99.9, 100k | −6,389 ns | 11/12 | 0.006 |
| end-to-end p99.99, 100k | −784,097 ns | 10/12 | 0.039 |
| `rx_delivery` p99, Trade-only vs all copies | −1,417 ns | 6/6 | 0.031 |
| under 0.5 % independent loss: frames rescued | 18,462 | 6/6 | 0.031 |
| the same, Trade-only | 6,132, i.e. its exact third | 6/6 | 0.031 |
| cost of copying everything, e2e p99 | +4,955 ns | 6/6 | 0.031 |
| under burst loss: staggered copy rescues | 5,538 frames, 96.7 % of protected Trades | 6/6 | 0.031 |
| the same, cost at p99.9 | +298,410 ns | 6/6 | 0.031 |
| fixed destination order puts the same peer behind, two receivers | +1,356 ns median | 6/6 | 0.031 |
| at three receivers, medians are monotone in peer index | `c0 < c1 < c2` | 6/6 | 0.031 |
| rotation removes that ordering | 0/6 monotone | 6/6 | 0.031 |
| rotation halves the spread across receivers | 2,808 → 1,217 ns | 6/6 | 0.031 |

Deliberately absent: end-to-end p50, which never resolves; and the burst-loss rescue from
an immediate copy, which is 3.8 % and indistinguishable from nothing.

## Four changes

### 1. Instrumentation is allocated before the socket is bound

`receiver.cpp` bound its socket and then built the stage-timing buffers. At
`--stage-capacity 25000000` that is ~600 MB zeroed on construction: several hundred
milliseconds in which the thread never calls `recv`, while the kernel fills and then
overflows the receive buffer. `ab_bench.sh` passes `--stages` on every run, so this
happened in every measured run, and it appears in the published output as channel loss.

The fix is to construct the buffers in `main()` before `net.open()`. The previous order is
kept behind `--late-alloc` so both can be measured in one binary, back to back, instead of
across two builds minutes apart.

**Resolved**: first-copy loss −0.77 pp at 100k msg/s and −1.30 pp at 200k, six blocks
each, every block agreeing, sign p = 0.031 — the floor for six blocks.

**Not claimed**: the latency percentiles. The hole opens during receiver startup, and
`bench.sh` waits 8 s of warm-up before the consumer's first sample, so the delayed backlog
is never measured. Both runs confirm this: loss resolves, latency does not. An earlier
draft of this document asserted the tail was affected; that was wrong and is corrected
here.

### 2. Redundancy is restricted to Trade

The baseline copies every datagram. The format's own header explains why that is more than
is needed: BBO and OrderBook are *state snapshots*, and OrderBook is deliberately a full
five-level snapshot rather than a delta so that a loss self-corrects from the next one.
Trade carries `cum_quantity_lots`, `cum_notional_ticks` and `cum_trade_count` so volume
and VWAP also re-derive. What does not re-derive is `trade_id` and the price and size of
that one print — the only field in the format its own author marks "not derivable".

`--dup-trades` copies a datagram only if it carries a Trade. Under `--type mixed` that is
one datagram in three, taking redundant load from 2.0x to 1.33x with the protection left
where loss is permanent.

This also removes a semantic problem rather than papering over it. A BBO copy arriving
after a newer one is stale state and must not be applied; a Trade copy is a historical
record and is correct however late it lands. Restricting redundancy to Trade makes a late
copy safe **by construction**, not by tuning a staleness threshold.

**Resolved**: `rx_delivery` p99 — the gap between the receiving kernel's timestamp and our
loop reading the datagram, measured entirely on one clock — falls by 1,417 ns for
Trade-only and 2,036 ns for no redundancy at all, six blocks, every block agreeing,
sign p = 0.031. Trade-only captures about 70 % of the benefit of switching redundancy off
while keeping the protection.

**Measured under loss, not only argued.** With `netem loss 0.5 %` injected, Trade-only
rescues frames at exactly the ratio the design predicts: `undelivered` falls to 0.673 of
the no-redundancy arm against a predicted 2/3, because Trade is one message in three. Full
duplication rescues more but costs +4,955 ns at end-to-end p99 and +2,616 ns at
`rx_delivery` p99 — both unanimous over six blocks — where Trade-only costs +888 ns.

**Measured under bursts, where an immediate copy is worthless.** With
`netem loss gemodel` supplying runs of ~20 datagrams, a copy sent immediately rescues
3.8 % — indistinguishable from nothing, 4/6 blocks, sign p = 0.688 — which reproduces the
baseline's own report of duplication rescuing 2 losses out of 485. Held back 64 datagrams
and admitted by the bitmap gate, the same copy rescues 32.2 % of all losses against a
ceiling of 33.3 %: **96.7 % of the Trades it protects**. The baseline proposes precisely
this fix and states the gate "already absorbs a late copy"; `MonotonicGate::admit` rejects
any `seq_id <= last_`, so the fix cannot work in the code proposing it. `BitmapGate` is the
missing half.

The cost of the stagger is +298,410 ns at p99.9, unanimous — and that figure needs reading
rather than quoting. The rescued frames are 0.308 % of samples, so they occupy everything
above p99.692; p99.9 is inside that band and p99, which shows no effect at all, is outside.
More importantly the two arms are not measuring the same population: the frames that make
the staggered arm look slower do not exist in the other arm, having been lost. Below
p99.69 the stagger costs nothing measurable; above it, 0.31 % of messages arrive ~320 µs
late **instead of not arriving.**

**Not claimed**: end-to-end p99 under redundancy, which moves the same way by 4.7–5.1 µs
and never resolves.

### 3. Fan-out rotates its destination order

The baseline sends to peers in a fixed order, so the peer served last waits behind the
other n−1 on *every* datagram. Its published skew — growing to 11.7 µs at ten receivers —
is therefore a fixed penalty attached to a fixed receiver, not jitter that averages out.
Advancing the starting index by one per datagram costs an add and a compare and leaves
every receiver with the same mean.

**Resolved, for the part that matters.** `ab_bench.sh` hard-wired `--receivers 1`, which
makes the axis the task judges most explicitly unmeasurable with its own harness; adding the
flag was the prerequisite. At two receivers, with `--fixed-order` restoring the baseline's
behaviour in the same binary:

* fixed order puts the second peer behind the first in **6 of 6 blocks**, median +1,356 ns,
  sign p = 0.031. The penalty is systematic and attached to one receiver, which is the
  claim the change is built on;
* with rotation the direction is gone — 3 of 5 blocks, median +46 ns.

At two receivers the magnitude did not resolve — `|skew|` fell in 4 of 5 usable blocks,
p = 0.375 — because two receivers need four isolated cores and that bench had three, so one
consumer ran on the housekeeping core and one block's median went into the milliseconds.

**At three receivers, on a host resized so six isolated cores were available and
`check_cores.sh` passes without an override, it resolves and the prediction sharpens.** A
fixed order does not merely produce "more skew": it produces medians **monotone in peer
index**, because each peer waits behind every peer served before it. Nothing else about the
system predicts that ordering, and a single block could have refuted it.

| | fixed order | rotating |
|---|---|---|
| medians monotone `c0 < c1 < c2` | **6 of 6 blocks** | **0 of 6** |
| spread across receivers, median | 2,808 ns | **1,217 ns**, smaller in 6 of 6 |

Both at sign p = 0.031. The slope — about 1.4 µs per additional receiver, from 1,356 ns at
two to 2,808 ns at three — is the one behind the baseline's reported 11.7 µs at ten.

**Not claimed**: anything above three receivers. The slope is consistent with the
baseline's figure; consistent is not measured.

### 4. A build failure fails the run

`ab_bench.sh` ran `make ... ; echo BUILD_OK` unconditionally, so a compile error left the
previous binaries in place and the matrix was measured against stale code that silently
ignored the arm's flags. This is not hypothetical: it happened here, and cost a three-arm
run whose new-flag arm recorded "no-data" in every block. A build that does not build is a
failed run.

## What the measurements say about where the time goes

At p99 the end-to-end budget splits, across six blocks of 3.67M samples each, as:
source-ring wait ~460 ns, ring publish ~195 ns, wire leg ~22,000–26,000 ns. **Our own code
is ~660 ns of ~24,000.**

An idle `clock_probe` — round trip on one clock, turnaround on the other, so no clock
offset survives — measures 13,364 ns one-way at 20,000 pings/s. Our loaded `wire_ns` p50 is
13,4xx ns.

That agreement says **the transport adds nothing measurable on top of a bare UDP echo on
this path**, and no more than that. The probe opens `SOCK_DGRAM` with `SO_BUSY_POLL`, so
the kernel's UDP receive and transmit cost is inside *both* numbers and cancels rather than
being bounded. It does not establish that 13.4 µs is the physical path, and it does not
bound what a kernel-bypass transmit path might save. Splitting it would take hardware
`SO_TIMESTAMPING` on both sides — the receive half is already in `udp_backend.h`, the
transmit half is not — and that was not run.

## Measured and did not work

Reporting these is not modesty. Each was expected to help, and the design that survives is
the one whose failures are known.

| idea | result |
|---|---|
| temporal stagger + bitmap gate, aimed at the `memset` hole | six blocks, nothing resolved; 130 frames rescued out of 47,543. The mechanism was sound and the target was wrong — under burst loss it resolves, see change 2 |
| two-threaded receive (`--split-poll`) | nothing resolved: `rx_delivery` p99 3/6 blocks, e2e p99 4/6. It also lost frames where the single-threaded path lost none, 199 and 300 in two blocks — the handoff queue overflows. The baseline reports it worth 24 µs at the median on a 96-core host; on four cores the extra thread and the extra copy of every datagram pay for themselves and no more |
| ENA interrupt moderation off | 5k/s: 17,030 → 16,963 ns, unchanged. 20k/s: 13,364 → 14,813 ns, worse. Busy-poll drives NAPI directly and never waits for the interrupt |
| `IP_TOS` = `IPTOS_LOWDELAY` | +6.6 µs at p50 over four runs. Removed |
| io_uring send backend | e2e p50 median difference −402 ns against a between-block spread of 6,319; sign p = 0.625 |
| hand-applied clock-offset correction | produced a clean 6/6 on p50 and p99, and was withdrawn: the correction assigned to each arm correlated with that arm's position in the block, and the jump between adjacent probes minutes apart was as large as the drift being interpolated |

## Two corrections to earlier drafts

**The wire-leg floor argument was circular.** An earlier draft said the transport's wire leg
equals a bare ping-pong, therefore the path has no software left in it and a kernel-bypass
transmit path would save nothing. `clock_probe` opens `SOCK_DGRAM` with `SO_BUSY_POLL` — the
same kernel path — so that cost sits on *both* sides and cancels rather than being bounded.
The agreement establishes that this transport adds nothing over a bare UDP echo, and no
more. AF_XDP was dismissed on the wrong reasoning and is recorded below as untested.

**The sign test counted tied blocks as losses.** Six blocks of which four are exactly equal
would read 0/6, sign p = 0.031, when the honest answer is 0/2, p = 0.5. Ties are discarded
now. No claim in this document was affected — every published comparison has non-zero
differences in every block — but the split-poll result was, and it is reported above at its
true strength rather than its inflated one.

## Constraints not accounted for

* **A 1–3 ms stall on the receive path is unexplained.** It is the largest single number in
  the system. Its frequency tracks datagram rate rather than elapsed time, so halving the
  copy stream halves it, but that is a consequence and not a cure. Excluded, each with the
  counter that excludes it: the sender (`send_all` max 656 µs, two calls over 100 µs in
  9.47M datagrams); consumer scheduling (`SCHED_FIFO` changed nothing); IPIs and TLB
  shootdowns (0 shootdowns and ~0.05 IPIs/s on the isolated cores over 16 hours);
  timer ticks (`nohz_full` working); ENA PPS shaping (`pps_allowance_exceeded` did not move
  under load); interrupt moderation, THP and systemd timers (none matched the period); and
  **the host taking the core away** — a thread whose only instruction is a clock read, on
  the spare isolated core, saw zero gaps above 1 ms over ~160 s across idle, partial and
  full load, with `/proc/stat` steal moving 0–1 ticks. That narrows it to the receive path
  below our userspace and does not identify it.
* **Loss was modelled, not observed.** Independent and bursty loss are injected with
  `netem`, and the burst parameters were chosen to resemble the baseline's reported
  ~69-datagram runs rather than measured on a channel that behaves that way. A real lossy
  path may correlate loss with load, which neither model does.
* **Fan-out was measured at two receivers on the wrong number of cores.** Two receivers need
  four isolated cores and this bench has three, so one consumer ran on the housekeeping core
  alongside the interrupts. Enough for the mechanism and its sign, not for the magnitude,
  and nothing here extrapolates to ten receivers.
* **Multicast was not tried.** One send for N receivers is the right shape for fan-out. EC2
  does not carry multicast between instances inside a VPC, but Transit Gateway multicast
  domains exist; that route adds a gateway hop and was not measured, so "unavailable" would
  overstate it. On a physical LAN multicast would likely dominate every fan-out result here.
* **AF_XDP is untested, not rejected.** The backend's own notes record this driver refusing
  `XDP_ZEROCOPY`, leaving `XDP_COPY` through `xsk_generic_xmit`. Whether that still saves
  anything is unknown, because the measurement that would have bounded it was the circular
  one corrected above.
* **The 13.4 µs wire leg is not split.** Hardware `SO_TIMESTAMPING` would separate
  NIC-to-NIC from kernel stack, and `ethtool -T` reports `PTP Hardware Clock: none` on this
  driver — so on this hardware it cannot be done at all, and software stamps are the ceiling.
* **Nothing was done to reduce bytes or packet rate.** Frames are relayed verbatim; at
  1M msg/s the run hits the ENI's `pps_allowance_exceeded` and the consumer collects zero
  samples, so packet rate is the ceiling and field-level compaction is the untouched lever.
* **The stagger is a constant, not a measurement.** 64 datagrams is longer than the burst
  that was injected. Expressed in microseconds and derived from an observed burst-length
  histogram it would carry across rates; it does not.
* **The measured surface is narrow**: 100k–500k msg/s, fan-out 1 and 2.

## Reproducing

### The bench

`infra/` brings up the pair with Terraform. Two `m7i.2xlarge` in one subnet, one AZ, one
cluster placement group; cloud-init applies the kernel command line and the IRQ pinning.

```bash
cd infra && terraform init && terraform apply
```

`scripts/check_cores.sh` must pass **without** `--skip-core-check` on both hosts. If it
does not, nothing below is worth running: two spinning threads sharing a core produce
latency quantised to the scheduler timeslice, which looks exactly like a network problem.

The kernel command line matters and is easy to lose. `/etc/default/grub.d/50-cloudimg-settings.cfg`
is sourced *after* `/etc/default/grub` and overwrites `GRUB_CMDLINE_LINUX_DEFAULT`, so the
settings go in `/etc/default/grub.d/99-bench.cfg`; `infra/user_data.sh.tftpl` does this.
Verify with `cat /proc/cmdline` and `cat /sys/devices/system/cpu/isolated`.

### Building

```bash
make -C harness && make -C transport && make -C tools
```

### One A/B

`ab_bench.sh` handles both hosts. It syncs the tree, builds on each, and refuses to run if
either build fails.

```bash
scripts/ab_bench.sh \
  --send-host bench-tx --recv-host bench-rx --recv-ip 172.31.47.71 \
  --producer-core 1 --sender-core 2 --receiver-core 1 --consumer-core 2 \
  --arm 'base | | --late-alloc' \
  --arm 'ours | --dup-trades | ' \
  --blocks 12 --rate 100000 --samples 2000000 --out data/ab_100k_n12
```

Six blocks is the minimum that can support a claim: the two-sided sign test floor is
`2 / 2^N`, so N=4 gives 0.125 and N=6 gives 0.031. Twelve blocks reach 0.006 at 11/12.

Arms are interleaved and counterbalanced by the script — A,B then B,A — because the two
hosts' clocks drift and a cross-host figure measured fifteen minutes after its baseline
carries an unknown offset the size of the effect.

### The path, without trusting either clock

Round trip on one clock, turnaround on the other, so no clock offset survives. Note this
measures a UDP echo, not the physical path — see the correction above:

```bash
# on the receiver
tools/bin/clock_probe --reflect --bind 172.31.47.71 --port 51902 --core 1 --idle-s 300
# on the sender
tools/bin/clock_probe --probe --peer 172.31.47.71:51902 --count 40000 --rate 20000 --core 2
```

Run it at more than one rate. The path is rate-dependent: 17,030 ns one-way at 5,000/s and
13,364 ns at 20,000/s, because at the lower rate the receive path goes cold between
packets.

### The notebook

```bash
scripts/build_notebook.py
python3 -c "import nbformat; from nbclient import NotebookClient; \
  nb=nbformat.read('analysis.ipynb',as_version=4); \
  NotebookClient(nb,timeout=300,resources={'metadata':{'path':'.'}}).execute(); \
  nbformat.write(nb,'analysis.ipynb')"
```

Every table in it is computed from `data/ab_*/blocks.csv` at execution time. Those CSVs are
committed unedited, one row per arm-run, including the arm-runs that were marked invalid.

### Fan-out

Two receivers need four isolated cores and three need six, so the receiving host was
resized to `m7i.4xlarge` for those runs. Changing the instance type discards the launch-time
`cpu_options`, so SMT comes back on and the siblings must be taken offline again or the
isolated cores share physical cores with housekeeping work:

```bash
for c in $(seq 8 15); do echo 0 | sudo tee /sys/devices/system/cpu/cpu$c/online; done
```

`isolcpus`/`nohz_full`/`rcu_nocbs` also have to be widened in
`/etc/default/grub.d/99-bench.cfg` and the machine rebooted, which the stop/resize/start
cycle provides. Verify with `scripts/check_cores.sh 1 2 3 4 5 6`.

### Cost

Both instances stopped rather than destroyed between sessions: about $0.10/day for the
volumes against ten minutes of provisioning. `m7i.2xlarge` in `eu-central-1` is
$0.483/instance-hour.

```bash
aws ec2 stop-instances  --region eu-central-1 --instance-ids <sender> <receiver>
aws ec2 start-instances --region eu-central-1 --instance-ids <sender> <receiver>
```

Private IPs survive stop/start; public ones do not.
