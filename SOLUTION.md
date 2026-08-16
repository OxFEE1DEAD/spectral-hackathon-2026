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

## Contents

- [Delivery guarantees](#delivery-guarantees)
- [Redundancy: why duplication and not retransmission](#redundancy-why-duplication-and-not-retransmission)
- [Batching](#batching)
- [Fan-out to many receivers](#fan-out-to-many-receivers)
- [What redundancy actually bought](#what-redundancy-actually-bought)
- [Message format: what each field is for](#message-format-what-each-field-is-for)
- [Results](#results)
- [Measurement methodology](#measurement-methodology)
- [Environment and host tuning](#environment-and-host-tuning)
- [Clock synchronisation](#clock-synchronisation)
- [Next steps](#next-steps)
- [Open items](#open-items)

## Delivery guarantees

The consumer must observe a **strictly increasing** `seq_id`; anything outside that
sequence counts as a drop, and late delivery is worse than no delivery. The whole
delivery policy is therefore one rule, in `transport/include/delivery.h`:

> Publish a frame if and only if its `seq_id` exceeds the highest already published.

That single comparison provides four things at once: de-duplication, protection
against reordering, the late-arrival drop policy, and correct gap accounting. It
also decides the loss-recovery strategy for us, which is the subject of the next
section.

We accept a non-zero drop rate rather than delaying the stream to repair it. The
justification is not "loss is cheap" — it is that under this rule a repair which
arrives late **cannot be delivered at all**, so paying latency for it buys nothing.
What we do instead is make loss (a) rare, via redundancy that costs no latency, and
(b) harmless, via a message format from which a consumer can recover state. Both
are described below, and the drop rate is reported alongside latency for every
configuration so the trade is never made silently.

## Redundancy: why duplication and not retransmission

Under the monotonic rule, a retransmitted frame is worthless by construction: by
the time a NACK completes a round trip, later `seq_id`s have already been published,
so the gate must reject the repair. The round trip is spent for nothing.

Forward error correction fails for a weaker but related reason. Parity lets you
reconstruct a missing frame only after enough *later* frames have arrived — by
which point the stream has already moved past it.

The only recovery with no latency cost is **immediate duplication**: send the same
datagram twice. If the first copy is lost, the second still arrives before the next
message exists, so it passes the gate and genuinely rescues the loss. Independent
loss probability `p` becomes `p²`, which covers the 0.01–1% range comfortably and
degrades gracefully beyond it.

Sending everything twice unconditionally would double the packet rate at every
load, including exactly when the path can least afford it. So duplication is
**opportunistic**: having sent a datagram, the sender looks for more work, and only
if the source ring is empty does it send the second copy. Redundancy therefore
consumes only time the sender had nothing better to do with, and it is
load-adaptive with no knob and no policy to tune.

Measured over the real network, one receiver, showing how the ratio responds to load:

| offered rate | msgs/datagram | duplication |
|---|---|---|
| 1,000 | 1.00 | 100% |
| 10,000 | 1.00 | 100% |
| 100,000 | 1.00 | 100% |
| 500,000 | 1.00 | 77.7% |
| 1,000,000 | 1.19 | 0.4% |
| 2,000,000 | 2.53 | 0.0% |

Duplication **stands down on its own** as the rate rises, reaching effectively zero by
1M msg/s — before packet rate becomes the constraint identified under
[Results](#results). As it backs off, batching takes over.

Whether that redundancy is *worth* anything is a separate question, and the measured
answer is largely no on this path — see [What redundancy actually
bought](#what-redundancy-actually-bought) below.

## Batching

Several events are packed into one datagram, at every rate, through one code path
with no rate threshold. The sender drains whatever the source ring already holds
and sends immediately.

Crucially it never *waits* for a datagram to fill. At a low rate that yields one
message per datagram — not because batching is disabled, but because only one
message exists. Waiting for a second message at 1,000 msg/s would mean a 1 ms
delay against an end-to-end latency of roughly 2.2 µs, some 450× the entire budget,
to save one packet. Batching that packs what is available costs nothing; batching
that waits costs everything.

As duplication backs off under load, batching takes over (1.00 → 10.22
frames/datagram in the table above). Both mechanisms follow the same principle —
use spare capacity, never delay a message — which is why they compose without
configuration.

## Receive mode

Kernel busy-poll lets the receiving thread pull packets off the device queue on its
own core instead of waiting for a softirq. Measured at 200k msg/s:

| mode | p50 | p99 | p99.9 |
|---|---|---|---|
| **busy-poll**, real NIC | **34,521** | **39,194** | **54,517** |
| spin, real NIC | 58,965 | 82,893 | 184,721 |

Busy-poll is 1.7x better at the median and 3.4x better at p99.9 over a real NIC. On
loopback the same comparison runs the *other* way by 2.5x, because there is no NAPI
instance to poll and the blocking receive only adds a scheduler wake-up per message.

That is why the mode is selected from the bound address rather than exposed as a
flag: the right answer is a property of the path, and shipping both would mean asking
the operator to know which one they are on.

## Fan-out to many receivers

Each receiver is an independent destination: its own socket, its own shared-memory
ring, its own consumer. The sender transmits a **separate datagram to every one of
them**, so the cost that grows with receiver count is datagram replication.

### Replicating to N destinations — measured, not assumed

There is more than one system call shape for putting the same bytes on N addresses. Note
one API constraint that rules out an obvious hybrid: `sendmmsg` takes **one socket** plus
an array of messages, not an array of sockets. N destinations therefore need either N
`msg_name`s on one unconnected socket, or N separate sockets — you cannot `connect()`
several sockets and then reach them all in a single `sendmmsg`.

| method | syscalls per datagram | destination resolution |
|---|---|---|
| `sendmmsg` | 1 | per message, in-kernel |
| `sendto` × N | N | per call |
| connected sockets, `send` × N | N | once, at `connect()` |

Measured at **10 destinations** and 200k msg/s, a rate none of them saturates:

| method | msgs/datagram | p50 | p99 | source-ring leg | wire leg | skew p50 |
|---|---|---|---|---|---|---|
| **connected** | 2.27 | **47,764** | **59,451** | **5,841** | 36,014 | **10,234** |
| `sendmmsg` | 3.16 | 52,328 | 65,822 | 8,182 | 35,687 | 13,839 |
| `sendto` × N | 3.23 | 50,533 | 64,794 | 8,320 | 34,231 | 13,938 |

**The stage split identifies the mechanism, and it is not the one we first assumed.** The
wire leg is the same for all three within 5% — 34.2 to 36.0 µs. The entire difference sits
in the **source-ring leg**: how long a message waits before the sender gets to it. A
cheaper send call lets the sender drain its ring faster, so messages queue less (5.8 µs
against 8.2 µs) and fewer of them accumulate into each datagram (2.27 against 3.16).
End-to-end latency follows from that, not from anything happening on the wire.

An earlier version of this document explained the same result as the kernel resolving each
destination per `sendmmsg` element while `connect()` resolves once. **That explanation was
wrong** — a route lookup costs on the order of 100–200 ns and cannot account for a 4.6 µs
difference. The measurable cause is sender throughput.

Two further observations, both against `sendto`:

- **`sendto` × 10 could not sustain 1M msg/s at 10 destinations at all.** A workload that
  takes about 4 seconds for the other two methods had not completed after 12 minutes. It
  is ruled out for fan-out on throughput grounds, not just latency.
- An earlier comparison at 3 destinations produced the same ordering but was not
  trustworthy: the runs differed in messages per datagram (3.57 against 5.34) with no
  stage capture to separate send cost from queueing. The N=10 comparison above was run at
  an unsaturated rate with stage capture precisely to remove that ambiguity.

`kAuto` is therefore connected sockets at every destination count. Note that the argument
which *predicts* the opposite — one syscall must beat N — has now been tested at both 3
and 10 destinations and lost both times.

### Cost per receiver, and the limit it implies

Measured at 500k msg/s with redundancy disabled so receiver count is the only variable:

| receivers | p50 | p99 | skew p50 | skew p99 |
|---|---|---|---|---|
| 1 | 32,569 | 41,701 | — | — |
| 2 | 40,295 | 47,449 | 790 | 1,611 |
| 3 | 36,585 | 44,593 | 3,103 | 5,188 |
| 10 | 47,663 | 59,322 | 11,669 | 11,691 |

**Skew is the clean signal** and it grows steadily: about **1.3 µs per additional
receiver**, reaching 11.7 µs at ten. The medians rise too — roughly 15 µs from one
receiver to ten — but they do not order cleanly between two and three, where the
difference is smaller than the burst events described under [Results](#results).

**The binding limit is not the skew, it is the packet-rate budget.** Every receiver costs
one more packet per datagram, so N receivers multiply packets per second by N, against a
ceiling this path puts at roughly 840k datagrams per second. At ten receivers and 500k
msg/s the sender was already at **97% of that ceiling** (81k datagrams/s × 10 = 812k pps).
The ceiling therefore divides the achievable message rate by the receiver count:

| receivers | datagram budget | message rate at ~6 msgs/datagram |
|---|---|---|
| 1 | 840,000 | ~5,000,000 |
| 3 | 280,000 | ~1,680,000 |
| 10 | 84,000 | ~504,000 |
| 20 | 42,000 | ~252,000 |
| 50 | 16,800 | **~101,000** |

So **serial unicast replication cannot deliver the task's top rate to anything like 50
receivers**, and no amount of optimisation above the socket layer changes that: the limit
is packets, and unicast fan-out needs N of them per datagram. Batching pushes the ceiling
out by a constant factor and is already doing so — 6.2 messages per datagram at ten
receivers — but it cannot change the linear dependence on N.

Getting to 50 receivers means not sending N copies from one host:

- **Network-level multicast**, where one packet reaches every receiver and packet rate
  stops depending on N entirely. Whether the fabric supports it is the question; a VPC
  generally does not without additional machinery.
- **A relay tree**, where each tier fans out to a bounded number of children, so no single
  host pays more than its branching factor and the pps cost per host stays constant. This
  adds a hop of latency per tier.

Neither is implemented here, and the measurements above are the argument for why one of
them would be necessary.

## What redundancy actually bought

The duplication scheme was designed against independent loss, where sending each
datagram twice turns loss probability p into p². That reasoning is only as good as the
independence assumption, so it needs measuring rather than asserting. The result is
mostly negative, and the reason is interesting.

### Loss actually observed on this path

Across the whole rate sweep with redundancy disabled — **241 million messages** delivered
over an L2 path in one VPC:

| offered rate | messages | lost | loss fraction |
|---|---|---|---|
| 100 | 16,200 | 0 | 0 |
| 10,000 | 1,150,848 | 0 | 0 |
| 200,000 | 59,950,848 | 0 | 0 |
| 500,000 | 59,950,848 | 0 | 0 |
| 1,000,000 | 59,950,848 | 5,262 | 8.8 × 10⁻⁵ |
| 2,000,000 | 59,950,848 | 0 | 0 |
| **total** | **240,970,440** | **5,262** | **2.2 × 10⁻⁵** |

**On this path loss is essentially absent.** Zero across 181 million messages at four of
the six rates, and the only loss sits at the highest packets-per-second point, consistent
with the pps limit discussed under [Results](#results). Nothing here comes near the
0.01%–1% the task treats as realistic, which means **this environment cannot exercise a
loss-recovery mechanism at all**. That is a property of the test path, not a claim about
the design.

### The loss that does occur is bursty, and duplication misses it

With duplication enabled the receiver reports both first-copy loss and how often a
redundant copy carried data its original never delivered:

| offered rate | duplication | gap events | datagrams lost | rescued by a copy |
|---|---|---|---|---|
| 500,000 | 77.7% | 7 | 485 | **2** |
| 1,000,000 | 0.4% | 50 | 4,007 | **14** |

Read the middle two columns together: **485 lost datagrams arrived in 7 events** — about
69 consecutive datagrams per event — and 4,007 in 50 events. Loss on this path is not
independent at all; it comes in bursts, exactly like the latency spikes.

That is why only 2 of 485 were rescued. A redundant copy is sent microseconds after its
original, so it lands **inside the same burst** and dies with it. At 500k msg/s a
69-datagram burst spans roughly 140 µs, and the copy follows its original by a few
microseconds — three orders of magnitude too close together to escape.

**So the honest conclusion is that duplication as implemented buys almost nothing against
the loss this path actually exhibits**, and the p → p² argument does not apply because
its independence premise is false here. It costs nothing at low rate — it uses only idle
capacity and stands down under load — but "harmless" is not "useful".

The fix it points to is a **temporal stagger**: hold the copy back by more than the burst
duration, so the two copies fail independently. That is a small change to the sender and
the receiver needs none, since the monotonic gate already absorbs a late copy. It is not
implemented, because on a path with 2 × 10⁻⁵ loss there is nothing to measure the
improvement against. It needs an L3 path, or a path with deliberately induced loss and a
realistic burst structure.

## Message format: what each field is for

The original harness format carried 320 bytes per message on a mixed stream. Much
of that was padding, values derivable from other values, values sent twice in two
representations, and static reference data repeated on every message.

**The reworked format is in `harness/include/message.h`, not in a separate wire
codec.** That is deliberate: the messages themselves are the optimised form, so the
producer emits them, the shared-memory rings carry them, the wire carries them
byte-for-byte, and the relay does no conversion at all on the hot path. It also
means the change is visible to anyone reading the harness rather than buried in a
transport-private encoder. A consequence worth noting is that the ring slot shrinks
with the largest frame, from 640 to 192 bytes, so every shared-memory hop moves a
third of the memory it used to.

### The rule that constrains everything

**Nothing is encoded relative to a previous message.** Encoding a trade's price as
an offset from the previous trade, or a book as a delta against the last one, would
be among the largest savings available — and it would mean that losing one message
corrupts every message after it. The only relative encoding used is **within a
single frame** (an order book's lower levels against the top of that same side),
where loss cannot separate the parts.

Every frame is therefore independently interpretable, with no decoder state for a
gap to corrupt. That is also why `seq_id` and `send_ts_ns` stay absolute rather than
becoming deltas against a datagram base: the 6 bytes a delta would save are not
worth making a frame meaningless on its own, and keeping them absolute means the
frame in the shared-memory ring is byte-identical to the frame on the wire, so the
relay performs no conversion at all on the hot path.

### Framing header — 24 B → 32 B, absorbing 48 B from every body

The header is the one part that grew, and it pays for itself immediately: it now
carries the instrument id and both venue timestamps that previously sat in each
body as 32 bytes of text and 16 bytes of absolute timestamps.

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

### Trade — 192 B → 80 B

A trade is an **event**, not state, so this is the type where loss genuinely
destroys information. It is the only type that gains fields.

| field | bytes | decision | reasoning |
|---|---|---|---|
| `symbol` | 16 | **dropped** → 2 B id | Reference data. Real venues publish an instrument list out of band and put a numeric code on the wire. |
| `venue` | 16 | **dropped** | Folded into the same instrument id. |
| `base_currency` | 8 | **dropped** | Derivable from the instrument; static reference data. |
| `quote_currency` | 8 | **dropped** | Same. |
| `price` | 8 | **dropped** | Exactly `price_ticks × 0.01`. The same number twice. |
| `quantity` | 8 | **dropped** | Exactly `quantity_lots × 0.001`. |
| `notional` | 8 | **dropped** | `price × quantity`. Purely derived. |
| `reserved[19]` | 19 | **dropped** | Padding. |
| `price_ticks` | 8 | **kept**, absolute | The canonical price. Integer and exactly representable; floating-point prices are a well-known source of comparison and rounding bugs. |
| `quantity_lots` | 8 | **kept**, absolute | Canonical size. |
| `trade_id` | 8 | **kept**, absolute | The venue's identity for the print — needed to reconcile against the venue's own feed and to spot venue-side gaps. Not derivable. |
| `buyer_order_id` | 8 | **dropped** | Post-trade reconciliation identity. No latency-sensitive consumer can act on a counterparty's order id. |
| `seller_order_id` | 8 | **dropped** | Same. |
| `exchange_ts_ns` | 8 | **kept**, 4 B delta | Venue event time: needed to order across feeds and to measure venue-to-us latency. Delta from this message's own `send_ts_ns`, so the message stays self-contained. |
| `match_engine_ts_ns` | 8 | **kept**, 4 B delta | On venues that publish both, the gap between matching-engine and gateway time is real information. Delta from `exchange_ts_ns`. |
| `aggressor_side` | 1 | **kept**, 1 bit | Core signal: which side lifted. |
| `is_block_trade` | 1 | **kept**, 1 bit | Off-book size changes how the print should be read. |
| `is_rpi` | 1 | **kept**, 1 bit | Retail price improvement is different liquidity. |
| `is_liquidation` | 1 | **kept**, 1 bit | Forced flow is a strong signal. |
| `tick_direction` | 4 | **kept**, 2 bits | Four states. Also useful redundancy: a consumer that missed the previous trade cannot derive direction itself. |
| `flags` | 1 | **merged** | Same bitfield. |
| `cum_quantity_lots` | — | **added**, 8 | Running total traded quantity. |
| `cum_notional_ticks` | — | **added**, 8 | Running total notional. |
| `cum_trade_count` | — | **added**, 8 | Running trade count. |

The three added fields are the point of the exercise. A consumer that accumulates
per-trade quantity is permanently wrong after a single lost message and cannot
detect it; a consumer that reads the running total is exactly correct again on the
very next trade. `test_cumulative_totals_survive_loss` in the harness tests demonstrates
both: dropping 1 of 10 trades leaves the incremental total at 509 against a true
560, while the cumulative view reads 560. Counter wrap is harmless — differences remain exact
modulo 2⁶⁴, which is all a consumer needs.

Net: 111 bytes of padding, duplication and reference data removed, 24 bytes of
recovery state added.

### Bbo — 192 B → 64 B

Top-of-book is already a **state snapshot**, so a lost update self-corrects on the
next one and no cumulative fields are needed.

| field | bytes | decision | reasoning |
|---|---|---|---|
| `symbol`, `venue` | 32 | **dropped** → 2 B id | Reference data. |
| `bid_price`, `ask_price`, `bid_size`, `ask_size` | 32 | **dropped** | Duplicates of the tick and lot integers. |
| `reserved[23]` | 23 | **dropped** | Padding. |
| `bid_price_ticks` | 8 | **kept**, absolute | Canonical. |
| `ask_price_ticks` | 8 | **kept**, 4 B spread | Carried as `ask − bid`. The spread is small by nature, and the result is still exact. |
| `bid_size_lots`, `ask_size_lots` | 16 | **kept**, 4 B each | `int32` covers these sizes with room to spare. |
| `bid_order_count`, `ask_order_count` | 8 | **kept**, 2 B each | Order counts are small. |
| `update_id` | 8 | **kept**, absolute | Book version. Must not be a cross-message delta, or a gap desynchronises it. |
| `exchange_ts_ns`, `match_engine_ts_ns` | 16 | **kept**, 8 B of deltas | As for Trade. |
| `flags` | 1 | **kept** | |

### OrderBook — 576 B → 160 B

Also a snapshot (`is_snapshot` is set), so also self-correcting.

| field | bytes | decision | reasoning |
|---|---|---|---|
| `symbol`, `venue` | 32 | **dropped** → 2 B id | Reference data. |
| `Level.price`, `Level.size` (doubles) | 160 | **dropped** | Duplicates of ticks and lots. |
| `Level.reserved` | 40 | **dropped** | Padding. |
| `reserved[26]` | 26 | **dropped** | Padding. |
| `Level.price_ticks` ×10 | 80 | **kept**, 8 B + 4×4 B per side | Top of each side absolute, the four levels below it as offsets from that top. Both live in the same message, so loss cannot split them. |
| `Level.size_lots` ×10 | 80 | **kept**, 4 B each | `int32` is ample. |
| `Level.order_count` ×10 | 40 | **kept**, 2 B each | Counts are small. |
| `update_id` | 8 | **kept**, absolute | |
| `prev_update_id` | 8 | **kept**, 2 B gap | The gap (normally 1) is the informative part, and it is the venue's own continuity check. |
| `checksum` | 4 | **kept** | The snapshot's integrity check — precisely the sort of field that earns its bytes. |
| `is_snapshot` | 1 | **kept**, 1 bit | |
| `exchange_ts_ns`, `match_engine_ts_ns` | 16 | **kept**, 8 B of deltas | |

**Deliberately not done:** delta-encoding each book against the *previous* book.
Consecutive updates differ in only a level or two, so this is the single largest
saving still on the table — and it is the wrong trade. A lost delta is
unrecoverable, whereas a lost snapshot self-corrects on the next update. Paying
bytes to keep every book self-contained is the same decision as refusing to
fragment, for the same reason.

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

**Generator artifacts were deliberately not exploited.** In this harness `venue` is
always the same, `match_engine_ts_ns` always equals `exchange_ts_ns`, the three
boolean flags are always zero, and `reserved` is always zeroed. Compressing on
those facts would score well here and fail on any real feed, so the instrument
dictionary is sized for genuinely multi-venue streams, the flag bits are all
carried, and the match-engine delta is transmitted even though it is always zero in
this data.

## Results

Two hosts on one L2 subnet, real NIC, mixed message stream, redundancy disabled so
rate is the only variable. **Everything below crosses the network** -- an earlier
revision of this document reported loopback figures, which understated latency about
15x because loopback has no driver, NIC or wire.

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

**Our code accounts for about 620 ns of the 35 µs median -- 1.8% -- and stays under
1 µs even at p99.999.** The other 98% is kernel TX, NIC, wire and kernel RX. There is
no remaining optimisation in our own code that could move the median, which is the
clearest possible argument that the next lever is bypassing the kernel network stack
rather than tuning anything above it.

Note which legs need clock agreement: only the wire leg spans two hosts. The three
legs carrying our own cost are each measured on a single clock, so the 620 ns figure
does not depend on synchronisation at all. The four legs also sum to the
independently measured end-to-end value (35,075 against 35,325 ns), which is a useful
check that the instrumentation is consistent.

### The far tail is one repeating event

p50 through p99.99 sit between 35 and 68 µs; p99.999 jumps to about 1 ms. That gap is
not gradual degradation, and the stage data says exactly what it is: **all 213
messages over 1 ms in a 20M-message run are wire-dominated, 213 of 213.**

Plotting one such event by sequence id settles the mechanism. Latency falls in a
straight line from 1.96 ms over about 385 consecutive messages, at 4,219 ns per
message against a 5,000 ns arrival interval. That is a queue that stopped being served
for roughly 2 ms, accumulated several hundred messages, and then drained. The implied
drain rate -- one message every ~780 ns, about 1.3M msg/s -- is the receive path's
burst capacity.

**The spikes are not uniformly distributed. They arrive in bursts**, and the larger
the spike the more strictly that holds. Grouping the messages above a threshold into
runs of consecutive arrivals -- where independent spikes would be almost entirely runs
of length one -- gives:

| spike size | isolated occurrences | inside bursts of >=10 messages |
|---|---|---|
| above 50 us | ~23% | ~12% |
| above 100 us | ~0.8% | ~92% |
| above 200 us | ~0.1% | ~98% |
| **above 500 us** | **0%** | **100%** |

The small excursions just past p99.9 are a mix of isolated and clustered, but **every
large spike belongs to a burst: not one isolated occurrence above 500 us in any
repetition.** A 20M-message run contains only one to three such bursts, and they are
what set p99.99 and beyond. This is directly visible in the scatter plots in the
notebook, as discrete vertical stripes against an otherwise flat baseline.

Two consequences worth stating. A far-tail percentile from a short run is meaningless,
because whether it captures a burst is luck rather than measurement. And the events
differ between repetitions of an identical configuration -- 2 ms in one run, 4.5 ms in
another, only 700 us in a third -- so averaging them would describe something that
never happened.

We have **not** attributed the stall further than "the wire". Separating sender kernel
TX from NIC from receiver kernel RX needs NIC hardware timestamps, which this setup
does not collect. That is the top open item.

### Loss follows packet rate, not message rate

The expectation going into the real-network runs was that loss would climb sharply
with message rate, since a NIC has a packets-per-second limit that loopback does not.
It does not happen that way:

| offered rate | msgs per datagram | datagrams/s | drop |
|---|---|---|---|
| 500,000 | 1.00 | 500,000 | 0% |
| 1,000,000 | 1.19 | **840,336** | **0.0088%** |
| 2,000,000 | 2.53 | 790,514 | 0% |

The only loss in the entire sweep sits at the highest *packet* rate. Doubling the
message rate from 1M to 2M *reduces* packets per second, because batching packs 2.5
messages per datagram at the higher rate -- and loss returns to zero.

So the packets-per-second limit is real and we operate near it; batching is what keeps
the higher message rates on the safe side. That also identifies what would break
first: larger messages, or disabling batching, would push packet rate up at a given
message rate and bring the loss back.

## Measurement methodology

Measuring a tail is easy to get wrong, and several of the results here only became
trustworthy after fixing the measurement rather than the code.

- **Steady state only.** The stream runs continuously; consumers attach at the live
  edge after a warm-up, so no measurement includes process startup, cold caches, or
  the first-touch page faults of the shared-memory ring. The first samples of each
  run are discarded in analysis.
- **Repetitions, with the spread reported.** A single run cannot establish a p99.9,
  let alone a p99.99. Percentiles are reported as a median across repetitions with
  the observed lo..hi range beside them; when that range spans an order of
  magnitude, the number is not yet a measurement and the spread says so.
- **Configurations must be interleaved, not run in blocks.** Running all
  repetitions of one configuration and then all of another confounds configuration
  with drift over time. We reached a 30× wrong conclusion this way once before
  catching it.
- **Nothing is written to a file while a measurement runs.** Samples go into memory
  that is reserved *and page-touched* before the first sample, `record()` is two
  stores, and the data reaches the filesystem in one pass after the run finishes.
  Samples that exceed the reserved capacity are counted and reported loudly rather
  than silently biasing the percentiles to a prefix of the run. An earlier version
  wrote one CSV line per message from inside the measurement loop, which put a
  `write()` on the timing path: on a disk-backed filesystem that alone cost ~1 ms at
  p99.99 — about 150× the transport's real tail, and invisible below p99.9 — and even
  on tmpfs a `write` can stall unpredictably.
- **Sample counts are sized to the percentile being claimed.** A percentile needs
  roughly a hundred observations beyond it to mean anything, so p99.999 needs ~10M
  samples. Where a rate cannot supply that in a sane run length, the far tail is
  omitted rather than quoted.
- **Cores are verified, not assumed.** `isolcpus` only stops the scheduler
  *migrating* work onto a core; anything explicitly pinned there still runs, and
  load average will not show it. Sharing a core with another busy-spinning thread
  produces latency quantised to the scheduler timeslice — 4 ms on a `CONFIG_HZ=250`
  kernel — which is indistinguishable from a network problem in the results and
  cost us a long misdiagnosis. `scripts/check_cores.sh` now refuses to measure on a
  busy, non-isolated, or SMT-shared core, and reports what is pinned there.
- **Latency is attributed per stage.** The sender can report
  producer→pre-send using a single clock, which localises whether a problem is
  before or after the wire without any cross-host clock question. This is what
  identified an 11 µs difference between two host pairs as environmental rather than
  ours (0.387 µs versus 0.379 µs of relay cost).

## Environment and host tuning

Everything below was measured on two hosts in a **single VPC and the same subnet**, with
direct layer-2 connectivity on the measured path and both in the same availability zone.

| | |
|---|---|
| Sending host | `m7i.metal-24xl` (bare metal) |
| Receiving host | `m7i.24xlarge` (virtualised) |
| OS | Ubuntu 20.04 LTS |
| Kernel | Linux 6.9 |
| CPU | Intel Xeon Platinum 8488C, SMT disabled — 48 online cores |
| Topology | single NUMA node |
| NIC | ENA adapter, MTU 1500, verified end to end with a no-fragment probe |
| Receivers | up to 3 independent receivers, each with its own socket and its own consumer |

This mirrors the environment named in the task statement (m7i/m8a, Ubuntu 24.04 or
20.04, ENA, two servers in one subnet with L2 connectivity, 1 to 3 receivers).

### Host tuning we relied on

The kernel on both machines was configured for low latency:

- Cores isolated from the scheduler, with a set of housekeeping cores left outside the isolated range.
- NIC interrupts pinned to the housekeeping cores, so device interrupts never preempt a spinning measurement thread.

What the solution tunes is confined to its own processes: which core each one is pinned
to, socket buffer sizes, `IP_TOS` for low delay, and the receive mode. It changes no
sysctl and no `ethtool` setting — notably not interrupt coalescing, which was left at
the adapter's adaptive default.

`scripts/check_cores.sh` encodes the part of this that is easiest to get wrong. It
verifies, before every run, that each core to be used is genuinely idle, is in the
isolated set, and has no active SMT sibling, and it refuses to measure otherwise.
`isolcpus` stops the scheduler *migrating* work onto a core but not from running work
pinned there, and a core shared with another spinning thread produces latency quantised
to the scheduler timeslice — 4 ms here — which is indistinguishable from a network
problem in the results.

### The virtualisation asymmetry, tested

Because one host is virtualised, periodic hypervisor interference was tested directly:
two streams running simultaneously in opposite directions over the same wire for 15 minutes,
18M samples per side, so that both directions share one time window.
There is **no periodicity** — autocorrelation of per-second worst latency at a 300 s lag
is −0.02 and −0.09 for the two directions — and the bare-metal receiver showed 2.4x *more*
spike-seconds than the virtualised one. The residual outliers are therefore not attributable
to virtualisation. This may require further testing later on.

### Network path

- Direct L2 between the two hosts on the measured path; no router hop.
- MTU 1500, confirmed end to end with a no-fragment probe at a 1472-byte payload. That is
  what licenses not fragmenting: the largest message is 576 bytes before compaction and
  160 after, so any MTU from roughly 600 bytes upward carries every message whole.
  Refusing to fragment is a resilience decision as much as a simplification — a message
  split across six fragments is six times as likely to be lost, since losing any one
  fragment destroys the whole message.
- ICMP round-trip on the path had a minimum of 0.173 ms and a mean deviation of
  0.013 ms. That is used only as an upper bound on one-way latency, since ICMP is
  handled in softirq and deprioritised; our one-way UDP figures sit well below half of
  it, in the expected direction.
- The measured path deliberately does not use the interface carrying management traffic,
  so the benchmark cannot interfere with access to the hosts.

## Clock synchronisation

Every cross-host figure is a receive timestamp taken on one host minus a send timestamp
taken on the other, so the measurement is only as good as the agreement between two
clocks. The hosts are synchronised by chrony to well within 3 microseconds, and the
offset measured during these runs was a few hundred nanoseconds. That is not taken on
trust per run: `chronyc tracking` is captured before *and* after each long measurement.

**A static offset is mostly harmless.** A constant offset shifts every sample equally, so
it biases the absolute median but cancels exactly in anything comparative — distribution
shape, the gap between p50 and p99.999, and every configuration-versus-configuration
result in this document. Since nearly all the conclusions here are comparative, a fixed
offset cannot manufacture or hide any of them.

**Drift is the real hazard**, because an offset that moves during a run smears the
distribution and can pass for tail latency. That is why the capture is before and after
rather than once at the start.

**Which results depend on it at all.** Of the four legs in the stage breakdown, only the
wire leg spans two hosts. Source-ring, publish and post-publish are each measured on a
single host, so the finding that our own code costs about 620 ns needs no clock agreement
whatsoever. Three further cross-checks:

1. The sender can measure producer-to-pre-send using one clock. No synchronisation is
   involved, and that figure is what localised an 11 µs difference between two host pairs
   as environmental rather than ours.
2. ICMP round-trip halved is an upper bound owing nothing to clock agreement, and our
   one-way figures sit below it — the expected direction, since ICMP is deprioritised.
3. The drain-event slope matches the known inter-message interval, which it could not if
   the clocks were drifting materially, and the four legs sum to the independently
   measured end-to-end value (35,075 against 35,325 ns).

**Residual uncertainty, not claimed away.** chrony's reported root dispersion is tens of
microseconds, which bounds accuracy against absolute UTC — but the quantity that matters
is the *relative* offset between two hosts tracking the same sources, which is far
smaller and is what the sub-microsecond figures describe. Absolute one-way latencies
should be read as carrying a small systematic uncertainty; the comparative results should
not. Tightening it further would mean NIC hardware timestamping, PTP against a hardware
clock instead of NTP, or a reflector design where a single host stamps both departure and
return so no cross-host comparison is needed.

## Next steps

Two independent lines of work, in cost order within each.

### Reduce the latency itself

The stage decomposition leaves **98% of the latency, and 100% of the large spikes, in
the kernel network path**, with our own code at about 620 ns. Nothing above the kernel
can move the median, so the work is to establish what the kernel is contributing and
then remove it.

1. **io_uring — cheapest of the three, and worth doing first.** No privileges, no XDP
   program, no interface binding; `liburing` is packaged, and the change is confined to
   how the sender and receiver issue their I/O. Multishot receive with provided buffers
   removes the per-packet syscall from the receive path entirely, and `DEFER_TASKRUN`
   moves kernel task work to a controlled point rather than an arbitrary one.
   **What it can and cannot answer:** it does *not* bypass the kernel network stack, so
   it cannot rule out interference from that stack, and it cannot move a 34 µs wire leg
   by removing a ~100 ns syscall. What it can plausibly do is raise the burst drain rate
   — measured at about 1.3M msg/s — which would shorten the drain events that dominate
   the far tail. That makes it a cheap test of a specific hypothesis rather than a
   general speed-up.
2. **AF_XDP — the experiment that actually tests kernel interference.** It bypasses the
   network stack on a stock kernel with no hugepages and no driver replacement, so if the
   burst events disappear the kernel was the cause, and if they survive the NIC or the
   fabric is. Binding an XDP socket to a live NIC queue is disruptive, so this is gated
   on explicit confirmation and a dedicated interface.
3. **DPDK — the primary late-game solution.** If bypass proves to be the lever, this is
   where the lowest and most predictable latency lives: a full userspace driver with no
   per-packet kernel involvement. It costs materially more to set up and to reproduce,
   so it follows the AF_XDP result rather than preceding it.

### Make the loss story real

Everything the design says about loss is currently untestable on this path, which loses
2 × 10⁻⁵ of messages and loses those in bursts.

4. **Measure on an L3 path, and on a path with a realistic loss pattern.** The
   independence premise behind duplication is false here; whether it is false everywhere
   is exactly what an L3 path with different loss characteristics would show.
5. **Stagger the redundant copy** by more than a burst duration, and measure whether the
   rescue rate moves off 0.4%. This is a small sender change and needs nothing from the
   receiver, but it is only worth doing where there is loss to rescue.

## Open items

- **The ~2 ms drain events are attributed to the wire but no further.** All 213
  messages over 1 ms in a 20M-message run are wire-dominated, and the shape identifies
  them as a queue draining rather than independent stalls. Separating sender kernel TX
  from NIC from receiver kernel RX needs NIC hardware timestamps, which this setup does
  not collect. Top open item.
- Fan-out is measured to **three independent receivers**, each receiving its own
  datagram, and the trend is extrapolated rather than measured at the 50 the task
  contemplates. Skew grows linearly with receiver count, implying roughly 150 us of
  replication cost at 50 receivers -- serial unicast is the wrong mechanism at that
  scale, and multicast or a relay tree would be needed. Neither is implemented.
- The three receivers share one host, so a single NIC carries N copies where N separate
  hosts would each carry one. That inflates the receive side as N grows, so the
  extrapolation is an upper bound on this design's cost rather than a prediction for 50
  machines.
- Loss resilience has been verified functionally (duplication rescues, the gate
  suppresses duplicates and stragglers exactly) but not yet under injected packet
  loss, so the p -> p^2 claim is reasoned rather than measured.
