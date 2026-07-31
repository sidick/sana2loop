# Getting Started

This walks through the simplest possible use of `loopback.device`: opening
unit 0 (standalone echo, the default), bringing it online, and using
`SanaInfo` to query it.

## Query a unit with SanaInfo

With `loopback.device` in `DEVS:` (see [Installation](Installation.md)):

```
> SanaInfo
MTU: 1500
BPS: 0
HardwareType: 1 (1=Ethernet)
AddrFieldSize: 48 bits
Station address: 02:53:32:4c:00:00
```

`SanaInfo` with no arguments opens unit 0, issues `S2_DEVICEQUERY`, and
prints the station address. That address — `02:53:32:4c:00:00` — is the
**factory default**: a locally-administered Ethernet address (the
low bit pattern of the first byte marks it as such) spelling `S2L` in the
middle bytes, with the last byte set to the unit number. Every unit gets
one of these unless a program calls `S2_CONFIGINTERFACE` to change it.

Bring the unit online and query again:

```
> SanaInfo 0 ONLINE
SanaInfo: unit 0 set ONLINE (io_Error=0)
MTU: 1500
BPS: 0
HardwareType: 1 (1=Ethernet)
AddrFieldSize: 48 bits
Station address: 02:53:32:4c:00:00
```

See [SanaInfo Reference](SanaInfo-Reference.md) for its full command line.

## A first echo round-trip (from your own program)

The point of **echo mode** (unit 0's default) is that whatever you write to
a unit comes straight back to a reader on that same unit — a queued
`CMD_READ` or `S2_READORPHAN` completes as soon as a `CMD_WRITE` arrives,
with source and destination addresses swapped so the reader sees itself as
the recipient. This is the standard SANA-II pattern every real driver
supports, so any code written against it needs no `loopback.device`-specific
branches:

1. Open the device on unit 0 (`OpenDevice("loopback.device", 0, io, 0)`).
2. Queue an `S2_READORPHAN` (or a typed `CMD_READ`) with `SendIO()` — this
   is what a real consumer does when it wants to be *waiting* for the next
   packet, not just polling.
3. Issue `S2_ONLINE` if you haven't already (writes fail
   `S2ERR_OUTOFSERVICE` while offline).
4. `CMD_WRITE` some bytes with any destination address and packet type you
   like.
5. `WaitIO()` the queued read — it completes with your data, the packet
   type you wrote, and addresses swapped (your own station address as the
   source, whatever you wrote to as the destination).

No special configuration is needed for this — echo is unit 0's (and every
unconfigured unit's) default mode. See
[Echo and Crossover Modes](Echo-and-Crossover.md) for the full precedence
rules (what happens when nothing is queued to receive a packet) and for
pairing two units together instead of echoing to yourself.

## Next steps

- [Configuration Reference](Configuration-Reference.md) — give a unit its
  own `ENV:sana2loop/unitN.config` to change its MTU, mode, or enable fault
  injection.
- [Fault Injection](Fault-Injection.md) — make a unit drop, duplicate,
  reorder, or truncate packets on purpose, to test how your code copes.
- [Replay and Record](Replay-and-Record.md) — play back a captured `.pcap`
  trace instead of live traffic, or capture what a unit actually sees.
