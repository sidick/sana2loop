# SanaDump and SanaSend

Companion Shell tools for capturing and sending traffic through a
`loopback.device` unit without writing your own program against the
device. Like [SanaInfo](SanaInfo-Reference.md), these require AmigaOS
2.04 (V37) or later (`ReadArgs()`, `CreateMsgPort()`, `CreateIORequest()`
are V36+ Exec calls), not bound to the device's own Kickstart 1.3 floor.

Both open the device through a small shared helper that installs real,
explicit `S2_CopyToBuff`/`S2_CopyFromBuff` hooks (rather than leaving
`ios2_BufferManagement` unset and relying on the device's own default) --
worth knowing if you're reading their source as a reference for writing
your own SANA-II consumer.

## SanaDump

Captures whatever a unit actually receives into a `.pcap` file.

```
SanaDump UNIT/A/N,FILE/A,TYPE/K,PROM/S,COUNT/K/N,DEVICE/K,CONFIG/S,ONLINE/S,DUMP/S,LEN/K/N
```

- `UNIT` -- unit number to open.
- `FILE` -- output `.pcap` path.
- `TYPE` -- hex packet type (e.g. `0800`) to capture only that type via a
  typed `CMD_READ`; omitted = `S2_READORPHAN` (any type).
- `PROM` -- open promiscuous (`SANA2OPF_PROM`) -- meaningful for a
  crossover peer's own destination filtering, a no-op on an echo unit
  (see [Echo and Crossover Modes](Echo-and-Crossover.md)).
- `COUNT` -- stop after this many packets; omitted = run until Ctrl-C.
- `DEVICE` -- SANA-II device to open; omitted = `loopback.device`. See
  ["Other drivers"](#other-drivers) below.
- `CONFIG` -- issue `S2_CONFIGINTERFACE` first, using whatever address
  `S2_GETSTATIONADDRESS` already reports as the driver's own factory
  default. sana2loop's own units never need this (usable from a factory
  default with zero setup) -- it matters for a real driver with no
  protocol stack already running to configure it. A no-op (reported, not
  fatal) if the unit is already configured.
- `ONLINE` -- bring the unit online before capturing and back offline on
  exit, i.e. own the whole online lifecycle for this run. **Omitted is
  the default**, and means "assume a protocol stack already manages
  online state, don't touch it" -- see
  ["Bringing a unit online"](#bringing-a-unit-online) below for why this
  changed from the pre-M19 always-force-online default.
- `DUMP` -- also print each captured packet live to stdout as it's
  captured (type, length, error/wire-error, BCAST/MCAST/HOST
  classification, addresses, and up to `LEN` bytes of hex), not just
  write it to `FILE`. Modeled on Aminet's `sanamon` (`comm/net/sanamon`)
  output format.
- `LEN` -- bytes of hex to print per packet under `DUMP`; omitted = 16.
  Ignored without `DUMP`.

Each captured record's timestamp is the real host wall-clock time it
arrived (unlike the device's own tick-relative `record=` timestamps --
see [Replay and Record](Replay-and-Record.md)), since this is meant to
capture *real* traffic from a real stack talking through the device, not
the device's own deterministic replay.

```
> SanaDump 3 RAM:capture.pcap COUNT=10 ONLINE
SanaDump: capturing unit 3 on loopback.device to RAM:capture.pcap (Ctrl-C to stop)
SanaDump: captured 10 packets
```

Live console view instead of (or alongside) the file:

```
> SanaDump 3 RAM:capture.pcap COUNT=1 ONLINE DUMP LEN=8
SanaDump: capturing unit 3 on loopback.device to RAM:capture.pcap (Ctrl-C to stop)
in 42 bytes, type=0806, err=0, werr=0, HOST, 02:53:32:4c:00:03->02:53:32:4c:00:03
ff ff ff ff ff ff
SanaDump: captured 1 packet
```

## SanaSend

Writes every record in a `.pcap` file to a unit as a real `CMD_WRITE`.

```
SanaSend UNIT/A/N,FILE/A,DEST/K,TYPE/K,DELAY/K/N,DEVICE/K,CONFIG/S,ONLINE/S
```

- `UNIT` -- unit number to open.
- `FILE` -- input `.pcap` path.
- `DEST` -- override destination address, `aa:bb:cc:dd:ee:ff`; omitted =
  use each record's own captured destination.
- `TYPE` -- override packet type in hex; omitted = use each record's own
  captured type.
- `DELAY` -- `dos.library` `Delay()` ticks between writes; omitted = 0
  (as fast as the device accepts them).
- `DEVICE` -- SANA-II device to open; omitted = `loopback.device`. See
  ["Other drivers"](#other-drivers) below.
- `CONFIG` -- issue `S2_CONFIGINTERFACE` first, using the driver's own
  factory address -- see `SanaDump`'s matching `CONFIG` entry above.
- `ONLINE` -- bring the unit online before sending and back offline on
  exit. Omitted (the default) assumes a stack already manages online
  state -- see ["Bringing a unit online"](#bringing-a-unit-online) below.

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

## Other drivers

Neither tool is actually limited to `loopback.device` -- `DEVICE=` opens
whatever SANA-II driver you name (a real Ethernet card's driver, PLIP,
whatever's installed in `DEVS:`), same command-line otherwise:

```
> SanaDump 0 RAM:capture.pcap DEVICE=a2065.device COUNT=20 ONLINE
> SanaSend 0 RAM:capture.pcap DEVICE=a2065.device
```

A real driver often needs a bit more setup than sana2loop's own units,
which are usable with zero configuration by design -- see `CONFIG` and
`ONLINE` above, and the next section.

## Bringing a unit online

Before M19, both tools unconditionally forced `S2_ONLINE` at startup.
That's harmless against sana2loop's own units (a test device, no
real-world consequence either way) but a real risk once `DEVICE=` can
point at hardware a protocol stack is actively managing -- forcing a
unit online (or, worse, taking it back offline on exit) out from under a
stack that deliberately left it in a particular state (e.g. mid
error-recovery) is exactly the kind of interference a capture/send tool
shouldn't cause by default.

So the default changed: **omit `ONLINE` and neither tool touches online
state at all**, assuming a stack already manages it -- the same posture
Aminet's `sanamon` takes by default. Pass `ONLINE` for the standalone,
no-stack-running case (testing directly against `loopback.device`, or
probing a real driver with nothing else attached) -- it then owns the
whole lifecycle: online at start, back offline at exit.

**If you're driving two of these tools against the *same* unit in one
session** (e.g. `SanaDump` capturing while `SanaSend` writes into it),
only give `ONLINE` to whichever one is genuinely first to use the unit
and stays open longest -- giving it to a *later* tool too would take the
unit back offline at that tool's own exit, potentially while the first
one is still relying on it staying online. This project's own on-target
test hits exactly this shape (`tests/copperline/tools-startup-sequence.txt`):
`SanaDump` gets `ONLINE`, `SanaSend` doesn't.

## A note on stack size

Both tools carry a `$STACK: 16384` cookie (the AmigaDOS convention a
Shell/Workbench launch uses to raise a process's stack above the default
4 KB if needed) and keep their own largest buffers in static storage
rather than on the stack. If you're adapting this pattern for your own
SANA-II tool: a plain Shell CLI process's default 4 KB stack is
genuinely tight once you add `ReadArgs()`, a per-open buffer-management
cookie, and any real packet-sized buffer -- worth being deliberate about,
not just something that "should be fine".
