# SANA-II Conformance (Rev 2/3/4/7) and SanaConform

`loopback.device` implements the post-Commodore SANA-II extensions --
Revisions 2, 3, 4, and 7 -- documented informally on wiki.amigaos.net
(largely authored by Olaf Barthel), since these were never part of any
vendor NDK release. Real-world stacks (Roadshow, AmiTCP-derived stacks)
probe for them, so a test device is only as useful as its own coverage of
them.

## What's implemented

### Rev 2/3 -- wider buffer-management hooks

Beyond the base `S2_CopyToBuff`/`S2_CopyFromBuff` (the hooks a caller
installs at `OpenDevice()` time to control exactly how packet payloads
are copied in and out of its own buffers, rather than leaving the device
to `CopyMem()` by default — see
[SanaDump and SanaSend](SanaDump-and-SanaSend.md#sanadump)'s mention of
`sana2open.c` for a worked example of installing them), a caller may
also offer `S2_CopyToBuff16`/`S2_CopyFromBuff16` and
`S2_CopyToBuff32`/`S2_CopyFromBuff32` at `OpenDevice()` time -- same
calling convention as the base hooks, just an alignment hint. Offering
them is always accepted; whether the device actually *uses* one instead
of the base hook is controlled per-unit by the
[`buffhooks=` config key](Configuration-Reference.md) (default `8`,
meaning "always the base hook" -- opt in with `buffhooks=16` or
`buffhooks=32` to see the wider hook actually get invoked). An opener
that didn't negotiate the configured width falls back to its own base
hook transparently.

### S2_PacketFilter

Part of the original SANA-II buffer-management tag family (alongside
`S2_CopyToBuff`/`S2_CopyFromBuff` above), but genuinely **dispatched**
only since M18: a caller may offer an `S2_PacketFilter` hook at
`OpenDevice()` time, and the device now calls it for every candidate
delivery on that open, immediately before the copy hook — exactly the
"filter before you pay to copy" efficiency the tag exists for. The
calling convention (sourced from wiki.amigaos.net's Autodocs, since this
toolchain's own `<devices/sana2.h>` carries no signature for it, the same
gap M13 already found for the Rev 2/3/7 tag values) is
`keep = PacketFilter(hook, ios2, data)` / registers `d0 = a0(hook)
a2(ios2) a1(data)` — note the register order does **not** match C
parameter order, a documented quirk of the standard Amiga Hook
convention, not a typo. `ios2` arrives with `ios2_DataLength`/`SrcAddr`/
`DstAddr` already populated as they'd appear on a completed read; `data`
points at the packet payload directly (no hardware header to exclude on
this device). Returning `FALSE` rejects the packet for that specific
candidate only — see
[Echo and Crossover: Multiple opens and monitoring](Echo-and-Crossover.md#multiple-opens-and-monitoring)
for the full per-candidate fan-out semantics this composes with.

### Rev 4 -- peer/DNS address queries, `RawMTU`

- `S2_GETPEERADDRESS` -- returns the real peer's station address for a
  crossover-paired unit (`ios2_SrcAddr` = your own address,
  `ios2_DstAddr` = the peer's). Echo and replay units have no real peer,
  so this fails `S2ERR_NOT_SUPPORTED` there.
- `S2_GETDNSADDRESS` -- this device has no DNS-server concept on any
  mode, so it always succeeds with both addresses zero-filled, rather
  than failing outright. (A real driver on a PPP-style dynamically
  addressed link would return real values here.)
- `S2_DEVICEQUERY`'s `RawMTU` field -- the maximum raw packet size
  (`SANA2IOF_RAW` writes), which for this device is always `MTU + 14`
  (the same allowance [`SANA2IOF_RAW` writes already got](Configuration-Reference.md)
  before Rev 4 gave it a name) for echo/crossover units, `0` for
  replay-mode units (no raw-write support there at all).

### Rev 7 -- DMA-capable buffer hooks

A caller may offer `S2_DMACopyToBuff32`/`S2_DMACopyFromBuff32` (and this
device's own best-guess continuation, `S2_DMACopyToBuff64`/
`S2_DMACopyFromBuff64` -- Rev 7's own documentation never published a
numeric value for the 64-bit variants). These are always accepted and
recorded, but **never invoked** -- a hardware-free RAM loopback has
nothing to hand a DMA-capable buffer address to, and declining is fully
spec-legal ("up to the driver to decide if it can use DMA ... and it
shall fall back to the standard CPU callbacks if necessary"). To make
that a verifiable claim rather than a silent no-op, every open that
offers a DMA hook is counted per-unit and surfaced via
`S2_GETSPECIALSTATS`.

## SanaConform

A Shell tool that probes a running unit against all of the above,
reporting what it finds in plain English rather than requiring you to
write your own test program:

```
SanaConform UNIT/N,DEVICE/K,CONFIG/S,ONLINE/S
```

- `UNIT` -- unit number to probe (default 0).
- `DEVICE` -- SANA-II device to probe; omitted = `loopback.device`. Not
  limited to this project's own device -- probing an unknown real
  driver's actual conformance is arguably more useful than confirming
  this project's own, which it already controls.
- `CONFIG` -- issue `S2_CONFIGINTERFACE` first, using the driver's own
  factory address -- see
  [SanaDump and SanaSend's matching `CONFIG` entry](SanaDump-and-SanaSend.md#sanadump).
- `ONLINE` -- bring the unit online for the probe and back offline on
  exit. Omitted (the default) assumes a stack already manages online
  state and doesn't touch it; the self-echo round-trip probe below
  already tolerates "not online" as a graceful, reported skip rather
  than a failure. See
  [SanaDump and SanaSend: Bringing a unit online](SanaDump-and-SanaSend.md#bringing-a-unit-online)
  for the full reasoning (unconditional forcing was the pre-M19 default).

```
> SanaConform 0 ONLINE
SanaConform: probing loopback.device unit 0
OpenDevice: OK (Rev 2/3/7 buffer/DMA hooks all accepted without error --
  advisory tags are legal to offer even to a driver that ignores them)
S2_ONLINE: OK
S2_DEVICEQUERY: MTU=1500 BPS=10000000 HardwareType=1
  Rev 4 RawMTU: 1514 (supported)
Station address: 02:53:32:4c:00:00
S2_GETPEERADDRESS: not supported for this unit's mode (no real peer)
S2_GETDNSADDRESS primary  : 00:00:00:00:00:00
S2_GETDNSADDRESS secondary: 00:00:00:00:00:00
Self-echo round trip: OK
Buffer hooks actually invoked: base 8-bit (to=1 from=1)
S2_PacketFilter invoked: YES (1 call)
DMA hooks invoked: 0 (0 expected -- DMA is always advisory, a driver
  may legitimately decline every buffer and fall back to CPU copies)
sana2loop quirk stat: 2 open(s) on this unit have offered a DMA hook
```

It offers every Rev 2/3/7 hook it knows about, plus a real
`S2_PacketFilter` hook, at `OpenDevice()` time, then runs a real
self-echo round trip (a second open on the same unit as a write partner)
through instrumented counting hooks to report *which* negotiated buffer
width the driver actually chose, and whether the filter hook was
actually called before delivery -- not just whether the driver accepted
the tags. `S2_PacketFilter invoked` is exactly the check Aminet's
`sanamon` readme calls out most SANA-II drivers as never having actually
implemented, despite it existing since the SANA-II V2.0 spec -- see
[Echo and Crossover Modes: Multiple opens and monitoring](Echo-and-Crossover.md#multiple-opens-and-monitoring)
for what a genuinely-dispatched filter enables. If the probed unit is
crossover-paired (or otherwise doesn't self-deliver), the self-echo step
gracefully times out (~1 second) and reports that plainly instead of
hanging -- that's a meaningful result about the unit's mode, not a tool
failure (and means the buffer-hook and filter checks are skipped too,
since neither can be observed without a completed round trip).

Every Rev 4 check is deliberately reported with **graceful fallback** in
mind: an older driver, or a unit in a mode where a given addition
genuinely doesn't apply, is reported as such, not treated as a failure.
The one exception is the trailing "sana2loop quirk stat" line, which is
this project's own internal bookkeeping (not a SANA-II spec value) and
will simply show nothing useful against any other SANA-II driver.

Like SanaDump/SanaSend, SanaConform requires AmigaOS 2.04 (V37) or later
(V36+ Exec calls), not bound to the device's own Kickstart 1.3 floor.
