# SANA-II Conformance (Rev 2/3/4/7) and SanaConform

`loopback.device` implements the post-Commodore SANA-II extensions --
Revisions 2, 3, 4, and 7 -- documented informally on wiki.amigaos.net
(largely authored by Olaf Barthel), since these were never part of any
vendor NDK release. Real-world stacks (Roadshow, AmiTCP-derived stacks)
probe for them, so a test device is only as useful as its own coverage of
them.

## What's implemented

### Rev 2/3 -- wider buffer-management hooks

Beyond the base `S2_CopyToBuff`/`S2_CopyFromBuff` (see
[Buffer management](Getting-Started.md) if you haven't read about those
yet), a caller may also offer `S2_CopyToBuff16`/`S2_CopyFromBuff16` and
`S2_CopyToBuff32`/`S2_CopyFromBuff32` at `OpenDevice()` time -- same
calling convention as the base hooks, just an alignment hint. Offering
them is always accepted; whether the device actually *uses* one instead
of the base hook is controlled per-unit by the
[`buffhooks=` config key](Configuration-Reference.md) (default `8`,
meaning "always the base hook" -- opt in with `buffhooks=16` or
`buffhooks=32` to see the wider hook actually get invoked). An opener
that didn't negotiate the configured width falls back to its own base
hook transparently.

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
SanaConform UNIT/N
```

- `UNIT` -- unit number to probe (default 0).

```
> SanaConform 0
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
DMA hooks invoked: 0 (0 expected -- DMA is always advisory, a driver
  may legitimately decline every buffer and fall back to CPU copies)
sana2loop quirk stat: 2 open(s) on this unit have offered a DMA hook
```

It offers every Rev 2/3/7 hook it knows about at `OpenDevice()` time,
then runs a real self-echo round trip (a second open on the same unit as
a write partner) through instrumented counting hooks to report *which*
negotiated width the driver actually chose -- not just whether it
accepted the tag. If the probed unit is crossover-paired (or otherwise
doesn't self-deliver), the self-echo step gracefully times out
(~1 second) and reports that plainly instead of hanging -- that's a
meaningful result about the unit's mode, not a tool failure.

Every Rev 4 check is deliberately reported with **graceful fallback** in
mind: an older driver, or a unit in a mode where a given addition
genuinely doesn't apply, is reported as such, not treated as a failure.
The one exception is the trailing "sana2loop quirk stat" line, which is
this project's own internal bookkeeping (not a SANA-II spec value) and
will simply show nothing useful against any other SANA-II driver.

Like SanaDump/SanaSend, SanaConform is a normal AmigaOS 2.0+ program
(V36+ Exec calls), not bound to the device's own Kickstart 1.3 floor.
