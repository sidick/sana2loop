# Replay and Record

Instead of live echo/crossover traffic, a unit can play back a captured
`.pcap` trace (`mode=replay`), and/or capture whatever it actually receives
to its own `.pcap` file (`record=`, independent of mode — works for echo
and crossover units too).

## Replay

```
mode=replay
replay=S:sana2loop/mycapture.pcap
```

`replay=` names a classic-format `.pcap` file (either byte order is
accepted — the file's own magic number decides, so a capture made with a
little-endian host tool like `scapy` loads fine on this big-endian 68000
device), Ethernet link type. It's loaded **once**, at first open, same
"config re-read on next open" timing as everything else — see
[Configuration Reference](Configuration-Reference.md#when-configuration-is-read).
A missing or malformed capture file fails the whole `OpenDevice()` call
(`S2ERR_BAD_ARGUMENT`) — unlike a missing plain config file, there's no
sensible default capture to fall back to.

**A replay unit can hold at most `replaycap` records — 4 by default.**
This default is a real, deliberately small limit — not a nice round
number — settled on after two rounds of empirical `AllocMem()` failures
under a memory-constrained test profile (see the [Changelog](Changelog.md)
for the story). A capture with more records than the active cap (default
or configured) fails to load. If your capture has more than 4 records and
your target has the memory to spare, raise the ceiling per-unit:

```
mode=replay
replay=S:sana2loop/mycapture.pcap
replaycap=16
```

See [Configuration Reference](Configuration-Reference.md) for
`replaycap`'s valid range and the memory-budgeting responsibility that
comes with raising it (the device only fails loudly with
`S2ERR_NO_RESOURCES` if a raised value doesn't actually fit — it doesn't
know your target's own free memory in advance).

`CMD_WRITE`/`S2_BROADCAST`/`S2_MULTICAST` on a replay unit always fail
`S2ERR_NOT_SUPPORTED` — there's no live traffic to write into a scripted
playback.

### `replaymode=consume` (the default)

Each `CMD_READ` or `S2_READORPHAN` hands back the next loaded record,
**in order**, ignoring `ios2_PacketType` entirely — this is a scripted,
in-order playback of whatever the capture actually contains, not a
type-filtered read. Once the sequence is exhausted, further reads queue
exactly like any other unit with no traffic waiting (no special
"end of stream" signal — SANA-II has no such concept).

### `replaymode=scheduled`

```
mode=replay
replay=S:sana2loop/mycapture.pcap
replaymode=scheduled
```

Each record is delivered on its **own captured timestamp**, relative to
the first record's timestamp, at whole-second granularity (not
microsecond-exact — driven by a VBlank interrupt tick, 50 ticks per
virtual second). A queued `CMD_READ`/`S2_READORPHAN` submitted **before** a
record's due time genuinely waits, and completes only once that time
arrives — through the exact same delivery precedence as live echo/crossover
traffic (a typed `CMD_READ` matches by `ios2_PacketType`; `S2_READORPHAN`
takes whatever's next). This is the mode to reach for when timing between
packets matters to what you're testing, not just their order and content.

## Record

```
record=S:sana2loop/captured.pcap
recordmax=131072
```

Independent of mode: if `record=` is set, every packet the unit actually
receives — echo self-delivery, crossover delivery from its peer, or a
scheduled replay's own manufactured deliveries — is captured, pcap-
formatted, into a RAM buffer, and **flushed to `record=`'s path when the
last opener closes** (only if that closer is a real Process — same
`dos.library`-needs-a-process reasoning as config/replay file access).

Timestamps in a recorded capture are the device's own **virtual tick**
counter (whole seconds, `ts_usec` always 0) — not real wall-clock time,
consistent with this device's whole deterministic-by-construction design.

`recordmax` bounds the RAM buffer (bytes, default 65536, see
[Configuration Reference](Configuration-Reference.md)). Once full, further
packets are **silently not appended** — not a fatal error, and whatever was
already captured stays intact rather than being truncated or corrupted to
make room. Query how many packets were dropped this way via
`S2_GETSPECIALSTATS`: one record, type `1`
(`SANA2LOOP_SPECIALSTAT_RECORD_OVERFLOW`), `Count` = packets dropped —
present only if recording is active and at least one packet has actually
overflowed; zero records otherwise.

## Memory usage, and freeing it manually

Replay and record buffers (`su_ReplayPackets`/`su_ReplayDueTicks`/
`su_ReplayDeliveryCopies` for `mode=replay`, `su_RecordBuf` for `record=`)
are allocated once, at the unit's first open (the 0→1 open-count
transition), and are **only ever freed at that same transition the *next*
time the unit is opened from idle — never when the last opener closes it.**
This is a deliberate, documented design choice, not an oversight: freeing
at close would need extra bookkeeping to guarantee nothing in-flight (a
scheduled replay's VBlank-driven delivery, a not-yet-flushed record) still
references the buffer, and this device's whole approach is to keep that
kind of lifetime reasoning as simple as possible. The cost is real, though
— a unit that has ever used `mode=replay` or `record=` holds that memory
for as long as the device stays loaded, even while fully closed and idle.

**`loopback.device` also does not participate in AmigaOS's low-memory
recovery protocol** — it never calls `AddMemHandler()` to register a
callback Exec could invoke when some *other* task's `AllocMem()` is about
to fail. From the system's point of view this device is invisible to
memory pressure right up until it makes its own `AllocMem()` call, at
which point a failure is handled locally and immediately (`S2ERR_NO_RESOURCES`,
or failing the whole `OpenDevice()`) — there's no retry, no attempt to
free anything of its own to help the request along. (Every allocation in
the device also asks for plain `MEMF_CLEAR` only — no `MEMF_CHIP`/`MEMF_FAST`
— so Exec satisfies it from fast RAM first if any is fitted, falling back
to chip only when fast is absent or exhausted; the device has no
chip-memory-specific requirement of its own.)

**To free a unit's replay/record memory manually**, without unloading the
whole device:

1. Close every opener of that unit (its open count must reach 0).
2. Edit `ENV:sana2loop/unit<N>.config` (or `S:sana2loop/unit<N>.config`) to
   remove the `replay=`/`record=` lines you no longer need — or delete the
   file entirely to fall back to compiled-in defaults.
3. Open and close the unit once more (`SanaInfo <N>` is a convenient way to
   do this without writing/sending any traffic). The 0→1 transition this
   triggers runs the same `reset_unit()` that frees the old buffers — and
   since the freshly-read config no longer asks for replay/record, nothing
   gets reallocated in their place.

This works per-unit and doesn't require closing or reloading any *other*
unit's state. If you only want to shrink a record buffer rather than drop
recording entirely, lowering `recordmax=` and repeating the same
close/edit/reopen cycle replaces the old allocation with a smaller one
instead.

To release **every** unit's held memory at once instead of going unit by
unit, close every opener of every unit (the device-wide open count, not
just one unit's, must reach zero) and run `Avail FLUSH` from the Shell —
its documented job is expunging currently-unused libraries/devices/fonts,
and with no unit open anywhere this device qualifies. That runs
`expunge()`, which frees the whole device base (and, implicitly, every
`AllocMem()`'d buffer any unit was still holding) in one call — though note
the device itself is then gone until something opens it again, at which
point AmigaOS reloads it from `DEVS:` as if for the first time.

## Building a capture file

Any standard tool that writes classic-format `.pcap` (Ethernet link type)
works — `scapy` on the host is a convenient choice:

```python
from scapy.all import wrpcap, Ether

pkts = [
    Ether(dst="aa:bb:cc:dd:ee:01", src="aa:bb:cc:dd:ee:02", type=0x0800) / b"hello",
]
wrpcap("mycapture.pcap", pkts)
```

Copy the resulting file to `S:sana2loop/` (or wherever your `replay=` path
points) on the target Amiga (or its emulated disk image).
