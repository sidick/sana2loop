# SANA-II reference notes (loopback.device)

Distilled from `devices/sana2.h` (Commodore-Amiga, 1991/1994, Rev 4.1) and the
AmigaOS wiki's Revision 2/3/7 pages. Committed here so the device source
doesn't need to re-derive field names from memory. Not a full spec copy --
just the parts `loopback.device` actually touches, plus the conventions that
aren't obvious from the header alone.

## `struct IOSana2Req`

```c
struct IOSana2Req {
    struct IORequest ios2_Req;
    ULONG ios2_WireError;          /* wire type specific error */
    ULONG ios2_PacketType;         /* packet type (ethernet "type" field) */
    UBYTE ios2_SrcAddr[SANA2_MAX_ADDR_BYTES];  /* 16 bytes, only 6 used for Ethernet */
    UBYTE ios2_DstAddr[SANA2_MAX_ADDR_BYTES];
    ULONG ios2_DataLength;
    VOID *ios2_Data;
    VOID *ios2_StatData;
    VOID *ios2_BufferManagement;   /* copy-hook pointer; see below */
};
```

`SANA2_MAX_ADDR_BYTES` is 16 (128 bits), even though every wire type
`loopback.device` cares about (Ethernet) only uses 6. Fields beyond byte 6
should be zeroed, not left as garbage, when a driver fills an address.

## Commands used in Phase 1

| Command | Value | Direction | Notes |
|---|---|---|---|
| `CMD_READ` | 2 | caller waits for a packet | completes later if none queued |
| `CMD_WRITE` | 3 | caller sends a packet | echo: swaps into a queued/pending read |
| `CMD_FLUSH` | 8 | caller | aborts queued reads with `IOERR_ABORTED` |
| `S2_DEVICEQUERY` | `CMD_NONSTD+0` | caller | fills `struct Sana2DeviceQuery` via `ios2_StatData` |
| `S2_GETSTATIONADDRESS` | `CMD_NONSTD+1` | caller | fills both `ios2_SrcAddr`/`ios2_DstAddr` with the current address |
| `S2_ONLINE` / `S2_OFFLINE` | `CMD_NONSTD+16/17` | caller | flips unit state, fires queued `S2_ONEVENT`s |
| `S2_ONEVENT` | `CMD_NONSTD+14` | caller | see below |

Deferred to later phases (accepted-but-unimplemented -> `IOERR_NOCMD` for
now): `S2_CONFIGINTERFACE`, `S2_ADD/DELMULTICASTADDRESS`, `S2_MULTICAST`/
`S2_BROADCAST` (send flags), `S2_TRACKTYPE`/`S2_UNTRACKTYPE`,
`S2_GETTYPESTATS`/`S2_GETSPECIALSTATS`/`S2_GETGLOBALSTATS`, `S2_READORPHAN`.

## `S2_DEVICEQUERY` convention (easy to get backwards)

The caller pre-sets `ios2_StatData->SizeAvailable` to the size of *its own*
buffer before the call. The driver:

1. Reads that incoming value (the caller's buffer size).
2. Fills as many fields as fit.
3. Sets `SizeSupplied` = how many bytes it actually wrote.
4. Sets `SizeAvailable` = the *driver's* full struct size (so a caller with
   an old/small struct can detect there's more available on a future
   Sana2DeviceQuery version).

`AddrFieldSize` is in **bits** (48 for Ethernet's 6-byte address), not bytes.

## `S2_ONEVENT` is edge-triggered

A queued `S2_ONEVENT` (mask in `ios2_WireError`) completes on the *next*
matching transition, not the current state. Sending `S2_ONEVENT` for
`S2EVENT_ONLINE` to an already-online unit blocks until the unit goes
offline and back online again -- it is a wait-for-change primitive, not a
status poll (`S2_DEVICEQUERY`, and later `S2_GETGLOBALSTATS`, are the
status-poll path).

## Buffer management hook (`ios2_BufferManagement`)

On real hardware, `CMD_READ`/`CMD_WRITE` copy through a caller-supplied
function pointer (so the copy can happen from the right context -- e.g. an
interrupt copying out of a hardware FIFO). `loopback.device` has no
hardware and no interrupt-context copy to honor, so Phase 1 copies directly
via `CopyMem()`. The proposal calls out implementing the hook protocol
properly, as an *opt-in* fault-testing feature (so consumers can be tested
against real interrupt-context copy discipline) -- that's later-phase work,
not a Phase 1 gap.

## Error codes actually used

`S2ERR_OUTOFSERVICE` (10) -- unit not online. `S2ERR_MTU_EXCEEDED` (6) --
`ios2_DataLength` over the unit's MTU. `S2ERR_NO_RESOURCES` (1) -- `AllocMem`
failed. `S2ERR_BAD_ARGUMENT` (3) -- bad unit number at Open. Full list is in
`devices/sana2.h`; the wiki's Revision 7 page documents the newer ones this
device doesn't use yet.

## Why the device is hand-rolled (no libnix `devinit.o`)

See the top-of-file comment in `src/loopback_device.c`. Short version:
libnix's `devinit.o` hands back a fixed-size, opaque device base with no
room to add fields -- fine for a device with no cross-open shared state
(narrator.wyoming's `narrator.device`), wrong for a loopback device where
two independent opens of the same unit *must* see each other's packets.
Hand-rolling the Resident/auto-init table (as bebbo's own toolchain examples
do) lets the device base be sized to fit a per-unit state table, reached
explicitly via the `dev` (a6) parameter every hook already receives -- no
global needed, no `-fbaserel`/A4 instance-data hazard to worry about.
