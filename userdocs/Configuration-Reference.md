# Configuration Reference

Every unit can have its own config file, read **once**, the first time
it's opened (open count transitions 0→1 — see
[below](#when-configuration-is-read)). An unconfigured unit works with
sensible compiled-in defaults; you only need a config file to change
something.

## File location

```
ENV:sana2loop/unit<N>.config
```

falling back to

```
S:sana2loop/unit<N>.config
```

if `ENV:` doesn't have one — useful on a minimal system with no `ENV:`
assign at all. `<N>` is the unit number with no leading zero (`unit0.config`,
`unit10.config`, ...).

A **missing file is not an error** — the unit just uses its compiled-in
defaults. A file that exists but has a problem (an unrecognized key, or a
malformed line) fails the entire `OpenDevice()` call with
`S2ERR_BAD_ARGUMENT` — this device treats a config typo as something that
should be loud, not silently ignored or silently reinterpreted.

## File format

Plain text, one `key=value` per line:

```
mode=crossover
peer=3
mtu=1400
# a comment
```

- Lines starting with `#` are comments.
- Blank lines are ignored.
- No surrounding whitespace tolerance — `key = value` (with spaces) is
  malformed, not forgiving.
- Every value below is validated (range-checked for numeric keys); an
  out-of-range or malformed value fails the whole file exactly like an
  unrecognized key does.

## Keys

### General

| Key | Values | Default | Meaning |
|-----|--------|---------|---------|
| `mtu` | 1–1500 | 1500 (1006 for a non-Ethernet `wire=`) | Maximum Transmit Unit reported by `S2_DEVICEQUERY` and enforced on `CMD_WRITE`/`S2_BROADCAST`/`S2_MULTICAST`. `SANA2IOF_RAW` writes get 14 extra bytes of headroom (Ethernet header size) — see the SANA-II spec's raw-frame convention. This same number (MTU+14) is also reported as `RawMTU` (SANA-II Rev 4) via `S2_DEVICEQUERY` — except on a `mode=replay` unit or a non-Ethernet `wire=` unit, which always report `RawMTU=0` (no raw writes supported there). |
| `mintu` | 0–1500 | 0 (no minimum) | Minimum accepted write size. A `CMD_WRITE`/`S2_BROADCAST`/`S2_MULTICAST` shorter than this fails with `S2ERR_BAD_ARGUMENT` — **rejected, never silently padded** (padding would change the bytes a reader sees, against this device's byte-exact delivery contract). Useful for catching a stack that forgets real Ethernet's 60-byte minimum-frame padding, which works fine on a loopback but fails on real hardware. A `mintu` above the unit's (resolved) `mtu` fails the whole config. `0` (the default) disables the check entirely. |
| `bps` | any nonzero number | 10000000 (see `wire=` for its per-wire defaults) | The link speed `S2_DEVICEQUERY` reports in its `BPS` field. **Reported only** — nothing in this device throttles or delays traffic based on it. |
| `wire` | `ethernet`, `slip`, `cslip`, `ppp` | `ethernet` | What kind of wire this unit pretends to be — see [Wire types](#wire-types) below. |
| `rxqueue` | 0–64 | 0 | Bounded convenience receive queue. **0 means strict drop**: a packet with no queued `CMD_READ`/`S2_READORPHAN` waiting for it is dropped immediately (and counted in `Sana2DeviceStats.UnknownTypesReceived`) — real SANA-II drivers hold no internal buffers, and this default matches that. A nonzero value lets up to that many undelivered packets sit in a per-unit queue instead. |
| `buffhooks` | `8`, `16`, `32` | `8` | Which SANA-II Rev 2/3 buffer-management hook width this unit actually **invokes**, for whichever opener negotiated it (offered the matching `S2_CopyToBuff16/32`/`S2_CopyFromBuff16/32` tag at `OpenDevice()`). An opener that didn't negotiate the configured width falls back to its own base hook transparently — this is never a hard requirement, only a preference. `8` (the default) always uses the base hook, matching every version of this device before this knob existed. |

### Wire types

`wire=` selects the hardware type `S2_DEVICEQUERY` reports and, with it,
how addressing works on the unit:

| | `ethernet` (default) | `slip` / `cslip` / `ppp` |
|---|---|---|
| `HardwareType` | `S2WireType_Ethernet` | `S2WireType_SLIP` / `_CSLIP` / `_PPP` |
| Address size | 6 bytes (48 bits) | 4 bytes (32 bits) — an IP address stands in for a hardware address, the established convention for serial-wire SANA-II drivers |
| Factory address | `02:53:32:4C:00:<unit>` | `0A:53:32:<unit>` (i.e. `10.83.50.<unit>`) |
| Default `mtu` / `bps` | 1500 / 10000000 | 1006 / 19200 (`ppp`: 1006 / 115200) — an explicit `mtu=`/`bps=` always wins, wherever it sits in the file |
| Broadcast / multicast | supported | `S2_BROADCAST`, `S2_MULTICAST`, and `S2_ADD/DELMULTICASTADDRESS` all fail `S2ERR_NOT_SUPPORTED` — a point-to-point wire has no such addressing |
| `SANA2IOF_RAW` writes | allowed (MTU+14) | fail `S2ERR_NOT_SUPPORTED`; `RawMTU` reports 0 |
| `mode=replay` / `record=` | supported | fails the config — the pcap side of this device is built around `LINKTYPE_ETHERNET` framing |

A **crossover pair should configure the same `wire=` on both ends** — a
write while the two ends' wire types differ fails with `S2ERR_BAD_STATE`
(loudly, rather than silently never matching the peer's differently-sized
address). Since config is only read at a unit's own first open, that also
means opening both ends of a non-Ethernet pair before writing.

### Mode and pairing (see [Echo and Crossover Modes](Echo-and-Crossover.md), [Replay and Record](Replay-and-Record.md))

| Key | Values | Default | Meaning |
|-----|--------|---------|---------|
| `mode` | `echo`, `crossover`, `replay` | `echo` (`crossover` for units 2 and 3) | What the unit does with traffic. |
| `peer` | a unit number | itself | Required for `mode=crossover` — the other unit this one is paired with. A `crossover` unit with no distinct peer fails to open. |
| `replay` | a `dos.library`-openable path | — | Required for `mode=replay` — the `.pcap` capture to load. |
| `replaycap` | 1–256 | 4 | Maximum records a `mode=replay` unit will load — a capture with more fails the whole open (`S2ERR_BAD_ARGUMENT`). The default (4) is deliberately small (see [Replay and Record](Replay-and-Record.md#replay) for why); raise it per-unit if your capture is bigger and your target has the memory to spare — a value that doesn't actually fit fails with `S2ERR_NO_RESOURCES` instead, since the device can't know your target's free memory in advance. |
| `replaymode` | `consume`, `scheduled` | `consume` | How a replay unit delivers its loaded records — see [Replay and Record](Replay-and-Record.md). |
| `record` | a `dos.library`-openable path | — | If set, every packet this unit actually receives (in any mode) is captured to this path as `.pcap`, flushed at last close. |
| `recordmax` | 1–262144 | 65536 | RAM buffer size (bytes) for `record=`. Once full, further packets are silently not appended (counted, not fatal) — see `S2_GETSPECIALSTATS` in [Replay and Record](Replay-and-Record.md). |

`replay=`/`record=` buffers are held in RAM for as long as the device stays
loaded, even after every opener of the unit has closed it — see
[Replay and Record: Memory usage, and freeing it manually](Replay-and-Record.md#memory-usage-and-freeing-it-manually)
if that matters for your setup.

### Fault injection (see [Fault Injection](Fault-Injection.md) for the full pipeline and examples)

| Key | Values | Default | Meaning |
|-----|--------|---------|---------|
| `seed` | any number | — | PRNG seed for this unit's fault injection (xorshift32). `0` is accepted but coerced to `1` when actually used (an all-zero xorshift32 state never changes). Leave unset for a fixed default seed. |
| `droppct` | 0–100 | 0 | Percent chance a written packet is silently lost (accepted by the writer, never delivered). |
| `duppct` | 0–100 | 0 | Percent chance a written packet is delivered **twice** (a genuine second copy, independently subject to reordering). |
| `truncate` | 0–1500 | 0 (disabled) | Caps delivered packet length to this many bytes. `0` disables truncation. |
| `reorder` | 0–8 | 0 (disabled) | Reorder window size. `0` disables reordering; delivery order matches write order. |
| `oversilent` | 0 or 1 | 0 | Crossover only. When a written packet exceeds the **peer's** MTU (not the writer's own, which is checked unconditionally), `1` accepts the write and silently vanishes the packet — simulating a PMTUD black hole — instead of failing the write with `S2ERR_MTU_EXCEEDED`. |
| `offlineafter` | any count | 0 (disabled) | Auto-forces the unit offline after this many successful writes, as if `S2_OFFLINE` had been issued. |
| `onlineafter` | any count | 0 (disabled) | Auto-forces the unit back online after this many **failed** write attempts while offline, as if `S2_ONLINE` had been issued — simulates a flaky link recovering on its own. |

## When configuration is read

A unit resets to a fully clean state (offline, factory station address,
zeroed stats, `rxqueue` back to `0`, mode/peer/fault-injection knobs back
to their compiled-in defaults) **every time its open count transitions from
0 to 1** — i.e. the first `OpenDevice()` after the last close. Its config
file is read at that same moment. A **busy unit's live configuration is
never disturbed** by a config file changing on disk; you have to let every
opener close first (dropping the count to 0) before a config edit takes
effect on the next open.

The config file is only read when the **opener is a real Process**
(`FindTask(NULL)->tc_Node.ln_Type == NT_PROCESS`) — a bare Exec Task
opening the device gets compiled-in defaults with no `dos.library` call at
all, preserving the device's Kickstart 1.3 guarantee that its core
open/read/write path never depends on DOS being available.

## Example: a crossover pair with a lossy link

`ENV:sana2loop/unit4.config`:

```
mode=crossover
peer=5
droppct=5
seed=42
```

`ENV:sana2loop/unit5.config`:

```
mode=crossover
peer=4
```

Opening units 4 and 5 now gives you a virtual null-modem link where ~5% of
what unit 4 writes never arrives at unit 5 — useful for testing retry logic
without touching real hardware.
