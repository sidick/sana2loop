# SanaDump and SanaSend

Companion Shell tools for capturing and sending traffic through a
`loopback.device` unit without writing your own program against the
device. Like [SanaInfo](SanaInfo-Reference.md), these are normal AmigaOS
2.0+ programs (V36+ Exec calls -- `ReadArgs()`, `CreateMsgPort()`,
`CreateIORequest()`), not bound to the device's own Kickstart 1.3 floor.

Both open the device through a small shared helper that installs real,
explicit `S2_CopyToBuff`/`S2_CopyFromBuff` hooks (rather than leaving
`ios2_BufferManagement` unset and relying on the device's own default) --
worth knowing if you're reading their source as a reference for writing
your own SANA-II consumer.

## SanaDump

Captures whatever a unit actually receives into a `.pcap` file.

```
SanaDump UNIT/A/N,FILE/A,TYPE/K,PROM/S,COUNT/K/N
```

- `UNIT` -- unit number to open.
- `FILE` -- output `.pcap` path.
- `TYPE` -- hex packet type (e.g. `0800`) to capture only that type via a
  typed `CMD_READ`; omitted = `S2_READORPHAN` (any type).
- `PROM` -- open promiscuous (`SANA2OPF_PROM`) -- meaningful for a
  crossover peer's own destination filtering, a no-op on an echo unit
  (see [Echo and Crossover Modes](Echo-and-Crossover.md)).
- `COUNT` -- stop after this many packets; omitted = run until Ctrl-C.

Forces the unit online at startup -- a capture tool that isn't online
captures nothing. Each captured record's timestamp is the real host
wall-clock time it arrived (unlike the device's own tick-relative
`record=` timestamps -- see [Replay and Record](Replay-and-Record.md)),
since this is meant to capture *real* traffic from a real stack talking
through the device, not the device's own deterministic replay.

```
> SanaDump 3 RAM:capture.pcap COUNT=10
SanaDump: capturing unit 3 to RAM:capture.pcap (Ctrl-C to stop)
SanaDump: captured 10 packets
```

## SanaSend

Writes every record in a `.pcap` file to a unit as a real `CMD_WRITE`.

```
SanaSend UNIT/A/N,FILE/A,DEST/K,TYPE/K,DELAY/K/N
```

- `UNIT` -- unit number to open.
- `FILE` -- input `.pcap` path.
- `DEST` -- override destination address, `aa:bb:cc:dd:ee:ff`; omitted =
  use each record's own captured destination.
- `TYPE` -- override packet type in hex; omitted = use each record's own
  captured type.
- `DELAY` -- `dos.library` `Delay()` ticks between writes; omitted = 0
  (as fast as the device accepts them).

**A real constraint worth knowing before reaching for this tool**:
SANA-II (and this device) never lets `CMD_WRITE` fake an arbitrary
*source* address -- the delivered frame's source is always the unit's
own real station address, or, for echo mode specifically, the
destination you wrote (per that mode's own swapped-echo semantics -- see
[Echo and Crossover Modes](Echo-and-Crossover.md)). SanaSend replays a
capture's destination, type, and payload faithfully; the resulting
frame's source is whatever the device itself assigns, never the original
capture's source byte-for-byte. This models a real driver correctly --
you can't spoof your own MAC on real hardware either -- it just means
"replaying a capture" isn't a byte-for-byte wire replay of the original
frame.

```
> SanaSend 3 RAM:capture.pcap DELAY=5
SanaSend: sending RAM:capture.pcap to unit 3
SanaSend: sent 10 packets
```

## Building a `.pcap` file to send

Any standard tool that writes classic-format `.pcap` (Ethernet link type)
works -- see [Replay and Record](Replay-and-Record.md#building-a-capture-file)
for a `scapy` example. The project's own canonical corpus
(`host/make_fixtures.py`, generating `fixtures/*.pcap`: an ARP
request/reply, a DHCP discover, a TCP handshake) is a ready-made example
if you just want something real to try SanaSend against.

## A note on stack size

Both tools carry a `$STACK: 16384` cookie (the AmigaDOS convention a
Shell/Workbench launch uses to raise a process's stack above the default
4 KB if needed) and keep their own largest buffers in static storage
rather than on the stack. If you're adapting this pattern for your own
SANA-II tool: a plain Shell CLI process's default 4 KB stack is
genuinely tight once you add `ReadArgs()`, a per-open buffer-management
cookie, and any real packet-sized buffer -- worth being deliberate about,
not just something that "should be fine".
