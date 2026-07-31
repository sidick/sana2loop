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

**A replay unit can hold at most 4 records.** This is a real, deliberately
small limit — not a nice round number — settled on after two rounds of
empirical `AllocMem()` failures under a memory-constrained test profile
(see the [Changelog](Changelog.md) for the story). A capture with more than
4 records fails to load.

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
