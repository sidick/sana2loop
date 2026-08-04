# Fault Injection

Real links aren't perfect: packets get lost, duplicated, truncated,
reordered, or silently swallowed above a certain size (a classic PMTUD
black hole), and links themselves flap online and offline. `loopback.device`
can simulate all of this on demand, off by default, so you can test how
your code copes with a bad link and not just a perfect one.

All of the following are `unitN.config` keys — see
[Configuration Reference](Configuration-Reference.md) for exact ranges and
defaults.

## The pipeline

Every `CMD_WRITE` (and `S2_BROADCAST`/`S2_MULTICAST`) that reaches a live
(echo or crossover) unit runs through this fixed order:

1. **MTU check** — the writer's own MTU, always; for crossover, *also* the
   peer's MTU (see "PMTUD black hole" below). (`mintu=`, if set, is
   checked here too — but that's a plain validity floor, not a fault
   knob.)
2. `errors` — a 1-in-N chance the write **fails visibly** with
   `S2ERR_TX_FAILURE`/`S2WERR_TOO_MANY_RETRIES`, firing
   `S2EVENT_TX|S2EVENT_ERROR` to any queued `S2_ONEVENT`. The only knob
   that produces a *loud* failure — everything below succeeds the write
   and misbehaves silently. Exercises a stack's error-handling/retry
   path rather than its timeout recovery.
3. `droppct` — a percent chance the packet is accepted (the write
   succeeds, `io_Error` stays 0) but never delivered anywhere.
4. `loss` — the same silent vanishing as `droppct`, expressed as 1-in-N
   instead of a whole percent, for rates rarer than 1-in-100 (e.g.
   `loss=1000`). The two compose; each draws independently.
5. `truncate` — caps the packet's length before delivery.
6. `duppct` — a percent chance the packet is delivered a **second**,
   genuinely independent time.
7. `reorder` — if enabled, delivery is buffered and released out of
   strict arrival order.
8. `delay`/`deviation` — if either is set, delivery is deferred by a
   simulated transit latency (plus optional jitter) instead of happening
   synchronously.

This fixed order matters when combining knobs: a duplicate (from
`duppct`) is made **after** truncation (so both copies are the same,
already-truncated length) but **before** reordering (so a duplicate is just
as subject to being reordered as the original — you can get two copies of
the same packet delivered in either order, or with other traffic
interleaved between them). `delay`/`deviation` run **last**, after
reorder: whatever a packet's simulated transit latency ends up being, it
applies to whatever the rest of the pipeline already decided to deliver
(a duplicate, a reordering-evicted original, or a plain single write).

One pipeline exception worth knowing: `CMD_FLUSH`, `S2_OFFLINE` (manual
or auto via `offlineafter`), and last-close don't run new packets
through this pipeline at all — they instead **flush** whatever's already
buffered in the reorder and delay stages, delivering everything
immediately, bypassing any further wait. See
["Simulating out-of-order delivery"](#simulating-out-of-order-delivery)
and ["Simulating latency and jitter"](#simulating-latency-and-jitter-delay-and-deviation)
below for what that means for each.

`offlineafter`/`onlineafter` aren't part of this packet-shaping pipeline —
they're side effects ticked once per write attempt, covered
[below](#flaky-links-offlineafter-and-onlineafter).

## Randomness

All PRNG-driven decisions (`errors`, `droppct`, `loss`, `duppct`,
`reorder`'s eviction choice) use a per-unit xorshift32 generator, seeded
from `seed=` (`0` is
accepted but coerced to `1` — an all-zero xorshift32 state never changes).
**Re-seeded fresh every time the unit's open count transitions 0→1** —
same timing as everything else configuration-related — so a fixed `seed=`
gives you deterministic, repeatable fault behavior across runs, useful for
regression tests that need to reproduce an exact sequence of drops/dups/
reorders.

`0` and `100` for `droppct`/`duppct` are exact boundaries (guaranteed never
or always), not just "very likely" — a naive percentage approximation
would let a sliver of writes through even at `droppct=100`, which isn't
what "100%" should mean for a test tool. `errors`/`loss` get the same
treatment at *their* boundaries: `0` is exactly disabled (and doesn't
even draw from the PRNG, so adding it to a config never perturbs the
other knobs' seeded sequences), and `1` is exactly every write.

## Simulating a lossy link

```
droppct=10
seed=1
```

10% of writes vanish silently — the writer sees `io_Error=0` (a real link
doesn't tell the sender when a frame is lost), but nothing is ever
delivered for those.

## Simulating duplicate frames

```
duppct=100
```

Every write is delivered **twice** — useful for testing that your protocol
handles duplicate frames correctly (sequence numbers, idempotent handling,
etc.) rather than assuming SANA-II guarantees exactly-once delivery (it
doesn't, on any real hardware either).

## Simulating truncated frames

```
truncate=64
```

Every delivered packet is capped to 64 bytes, regardless of how much was
actually written — simulates a corrupted or partially-received frame.

## Simulating out-of-order delivery

```
reorder=4
seed=7
```

Packets are buffered in a fixed window; once it fills, one buffered packet
(any of them, PRNG-chosen) is released, and the remaining ones shift down,
preserving their own relative arrival order. This bounds how far out of
order a packet can arrive: at most `reorder - 1` *later* packets can
overtake it before it's forced out — not an unbounded shuffle. Anything
still buffered is flushed, in original arrival order, on `S2_OFFLINE`
(manual or auto via `offlineafter`), `CMD_FLUSH`, or the unit's last close
— nothing is ever silently stuck there forever.

## Simulating latency and jitter (`delay` and `deviation`)

```
delay=25
deviation=5
seed=3
```

Instead of delivering synchronously (the default), a written packet is
held for `delay` VBlank ticks (50/sec — `delay=25` is roughly half a
second) before it becomes deliverable. `deviation` adds symmetric
jitter: the actual latency for each write is independently drawn from
`[delay - deviation, delay + deviation]`, clamped so it never goes
negative (a packet can't arrive before it was sent) — so `delay=25
deviation=5` gives each write somewhere between 20 and 30 ticks. Setting
only `deviation` (leaving `delay=0`) is legal too — it models pure
scatter with no floor, each write's latency drawn from `[0,
deviation]`.

Because jitter can make a later write's due tick arrive sooner than an
earlier write's, `delay`/`deviation` can reorder delivery on their own,
independently of the `reorder` knob — the two compose if both are set.

Latency is measured in whole VBlank ticks, not wall-clock time or
microseconds — the same deliberately coarse, deterministic-under-
emulation granularity `replaymode=scheduled` uses (see
[Replay and Record](Replay-and-Record.md)), not an attempt at
real-world timing fidelity.

A unit with `delay`/`deviation` configured needs to be **online** for
time to pass at all — `CMD_FLUSH`, `S2_OFFLINE` (manual or auto via
`offlineafter`), and last-close all immediately deliver everything still
waiting rather than leaving it stuck with no further ticks coming (going
offline stops the clock for that unit entirely).

## PMTUD black hole (`oversilent`, crossover only)

```
# unit A
mode=crossover
peer=<B>
oversilent=1

# unit B (smaller MTU)
mode=crossover
peer=<A>
mtu=576
```

A's own MTU check passes a write, but the frame is too big for what's
actually on the other end (B's smaller MTU). With `oversilent=1`, the write
is accepted and the frame silently vanishes — `io_Error` stays 0, nothing
ever tells A's writer that delivery failed — exactly the failure mode real
Path MTU Discovery black holes produce when an intermediate router drops an
oversized packet without sending back the ICMP message that's supposed to
shrink future packets. Without `oversilent` (the default), the same
situation instead fails the write outright with `S2ERR_MTU_EXCEEDED`.

## Flaky links: `offlineafter` and `onlineafter`

```
offlineafter=5
onlineafter=3
```

`offlineafter=N`: after N successful writes, the unit auto-forces itself
offline, as if `S2_OFFLINE` had been issued — subsequent writes fail
`S2ERR_OUTOFSERVICE` until something brings it back online.

`onlineafter=N`: after N **failed** write attempts while offline, the unit
auto-forces itself back online, as if `S2_ONLINE` had been issued.

Combined, a unit with both set simulates a link that flaps: online for a
while, drops offline, and eventually recovers on its own — useful for
testing reconnection/retry logic without touching real hardware or timers.
