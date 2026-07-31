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
reader sees the unit's own station address as the source, and whatever
address you wrote *to* as the destination. This is deliberate — there's no
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

1. A queued, **typed** `CMD_READ` whose `ios2_PacketType` matches — first
   priority, per SANA-II's own typing convention.
2. Else a queued **untyped** `S2_READORPHAN` ("give me whatever's next").
3. Else, only if `rxqueue` is nonzero, a bounded convenience queue.
4. Else the packet is **dropped** and `Sana2DeviceStats.UnknownTypesReceived`
   is incremented.

`rxqueue` defaults to **0** — strict drop, no buffering — matching the
teaching point that real SANA-II drivers hold no internal buffers of their
own. See [Configuration Reference](Configuration-Reference.md) if you want
a convenience queue instead.
