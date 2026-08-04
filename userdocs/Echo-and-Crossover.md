# Echo and Crossover Modes

A unit is always in exactly one of three modes (`mode=` in its config —
see [Configuration Reference](Configuration-Reference.md)): **echo**,
**crossover**, or **replay** (covered separately in
[Replay and Record](Replay-and-Record.md)). This page covers the two
live-traffic modes.

## Defaults

- **Unit 0** and every other unconfigured unit: standalone **echo**.
- **Units 2 and 3**: paired **crossover** by default (`peer=3` and `peer=2`
  respectively) — a config file can override either half of this.

## Echo mode

A `CMD_WRITE` on an echo-mode unit delivers to **the same unit** — whatever
you write comes back to a reader on that same open (or a different open of
the same unit; SANA-II has no concept of "my own" vs "someone else's"
reader on one unit). Addresses are swapped for a plain `CMD_WRITE`: the
reader sees whatever address you wrote *to* as the source, and the unit's
own station address as the destination. This is deliberate — there's no
real second party to ask "what's your address", so the simplest useful
behavior is letting the write specify what the reader should see as its
sender.

`S2_BROADCAST` and `S2_MULTICAST` on an echo unit use **real, unswapped**
addressing instead: the source is the unit's actual station address, and
the destination is the real broadcast/multicast address, so a reader's
`SANA2IOF_BCAST`/`SANA2IOF_MCAST` flags mean something.

Echo mode does **no destination filtering** — every reader sees everything,
regardless of `S2_ADDMULTICASTADDRESS` membership or promiscuous mode.
There's no separate peer to have misaddressed a packet at, so filtering
would only get in the way of the simplest test case.

## Crossover mode

A crossover pair is two units, each configured with `peer=<the other
unit's number>`. A `CMD_WRITE` on one delivers to **the peer**, with real,
unswapped addressing: the source is the writer's own actual station
address, the destination is exactly what was written. This is what makes
crossover useful for testing real protocol handshakes — each side sees a
real address on the other end, not a self-referential echo.

Crossover mode enforces standard 802.3-style receive filtering on the
**peer's** side: a candidate reader only receives a packet if its
destination is the peer's own current station address, the broadcast
address (all-1s), a multicast group the peer has joined
(`S2_ADDMULTICASTADDRESS`), **or** that specific reader opened with
`SANA2OPF_PROM` (promiscuous — accepts everything regardless of
destination).

Both halves of a pair must be configured for `mode=crossover` with a real,
distinct peer — a unit configured `mode=crossover` with no `peer=`, or a
`peer=` pointing at itself, fails to open.

### Example

```
# ENV:sana2loop/unit4.config
mode=crossover
peer=5

# ENV:sana2loop/unit5.config
mode=crossover
peer=4
```

Open unit 5, queue a read, then open unit 4 and write to unit 5's station
address — the read on unit 5 completes with the real source address of
unit 4, not a self-echo.

## Delivery precedence (both modes)

Whichever unit a packet resolves to (itself for echo, the peer for
crossover), delivery follows the same precedence, strictly:

1. Every **typed** `CMD_READ` whose `ios2_PacketType` matches, **fanned out
   to every distinct open that has one queued** — see
   ["Multiple opens and monitoring"](#multiple-opens-and-monitoring) below.
   First priority, per SANA-II's own typing convention.
2. Else (no typed reader anywhere claimed it) a queued **untyped**
   `S2_READORPHAN` ("give me whatever's next") — single delivery, to
   whichever orphan reader wants it.
3. Else, only if `rxqueue` is nonzero, a bounded convenience queue.
4. Else the packet is **dropped** and `Sana2DeviceStats.UnknownTypesReceived`
   is incremented.

`rxqueue` defaults to **0** — strict drop, no buffering — matching the
teaching point that real SANA-II drivers hold no internal buffers of their
own. See [Configuration Reference](Configuration-Reference.md) if you want
a convenience queue instead.

## Multiple opens and monitoring

Open the same unit more than once (a stack plus a passive monitor, for
example — the same pattern Aminet's `sanamon` uses against a real driver)
and every open with a matching typed read gets its **own independent
copy** of each packet, not just whichever opened first. Concretely:

- A packet is delivered once **per distinct open** with a matching typed
  `CMD_READ` — at most one fill per open per packet, even if that open has
  several reads of the same type pipelined (queue more to catch the *next*
  ones, not duplicates of this one).
- `S2_READORPHAN` deliberately stays **single-delivery and global**: it's
  the unclaimed-type catch-all SANA-II itself defines it as, not a general
  sniffing tool — if any open anywhere has a matching typed read queued,
  every orphan read is skipped entirely for that packet, exactly as if
  only one reader existed.
- An `S2_PacketFilter` hook (see
  [SANA-II Conformance](SANA-II-Conformance.md#s2_packetfilter)), if an
  open negotiated one, runs **per candidate, before the copy hook** — a
  candidate whose filter rejects a packet simply stays queued for a future
  one; delivery continues to the next candidate exactly as if that reader
  hadn't matched at all.
- Crossover's own destination filtering (station address, broadcast, a
  joined multicast group, or `SANA2OPF_PROM`) still applies per candidate,
  unchanged — fan-out and crossover filtering compose normally.
- `rxqueue` is only ever consulted if **nobody** — typed or orphan —
  received the packet, so a stack-plus-monitor setup should keep
  `rxqueue=0` regardless: buffering a packet that was already delivered
  elsewhere would just hand a later reader a stale duplicate.
- `Sana2DeviceStats`/`S2_GETTYPESTATS` counts describe traffic **arriving
  at the interface**, once per packet, regardless of how many opens got a
  copy — not a per-listener multiplier.

### SANA2OPF_MINE (exclusive access)

Opening with `SANA2OPF_MINE` claims the unit exclusively: the open
succeeds only if the unit has no other opens, and no further opens
(`MINE` or not) succeed until this one closes — `OpenDevice()` fails
`IOERR_UNITBUSY` either way. Useful for a consumer that specifically
needs to rule out the fan-out/monitoring behavior above for its own
session.
