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

## Commands implemented

| Command | Value | Direction | Notes |
|---|---|---|---|
| `CMD_READ` | 2 | caller waits for a packet | **typed**: only a packet whose type matches `ios2_PacketType` (set before submitting) satisfies it |
| `S2_READORPHAN` | `CMD_NONSTD+15` | caller waits for any unclaimed packet | untyped catch-all; see delivery precedence below |
| `CMD_WRITE` | 3 | caller sends a packet | echo: swaps into a queued/pending read via `deliver_packet()` |
| `CMD_FLUSH` | 8 | caller | aborts queued `CMD_READ`/`S2_READORPHAN` with `IOERR_ABORTED` |
| `S2_DEVICEQUERY` | `CMD_NONSTD+0` | caller | fills `struct Sana2DeviceQuery` via `ios2_StatData` (NULL-checked) |
| `S2_GETSTATIONADDRESS` | `CMD_NONSTD+1` | caller | fills both `ios2_SrcAddr`/`ios2_DstAddr` with the current address (no `S2_CONFIGINTERFACE` yet to make them diverge -- M3) |
| `S2_ONLINE` / `S2_OFFLINE` | `CMD_NONSTD+16/17` | caller | flips unit state, fires queued `S2_ONEVENT`s |
| `S2_ONEVENT` | `CMD_NONSTD+14` | caller | see below |
| `S2_GETGLOBALSTATS` | `CMD_NONSTD+13` | caller | copies `struct Sana2DeviceStats` whole (no `SizeAvailable`/`SizeSupplied` negotiation, unlike `S2_DEVICEQUERY`) |
| `S2_GETSPECIALSTATS` | `CMD_NONSTD+12` | caller | zero-record: `RecordCountSupplied = 0`, nothing tracked yet |

## Delivery precedence (`deliver_packet()`, M2)

A packet arriving on a unit is delivered in this order, each step consuming
the packet if it matches:

1. A queued **typed** `CMD_READ` whose `ios2_PacketType` matches exactly.
2. Else a queued `S2_READORPHAN` (any type).
3. Else, only if `rxqueue` (config, defaults to **0**) allows it, a bounded
   pending-data queue.
4. Else **dropped**, and `Sana2DeviceStats.UnknownTypesReceived` counts it.

`rxqueue` defaults to 0 (strict drop, no buffering at all) deliberately --
the proposal's teaching claim is specifically that "drivers hold no
internal buffers; this source shows it." A nonzero `rxqueue` is an
explicit, documented convenience divergence for casual poking, not the
default. This means callers must queue a `CMD_READ` (via `SendIO`, not
`DoIO`) **before** the matching `CMD_WRITE` happens, not after -- the
on-target test harness does exactly this.

Deferred to later phases (accepted-but-unimplemented -> `IOERR_NOCMD` for
now): `S2_CONFIGINTERFACE`, `S2_ADD/DELMULTICASTADDRESS`, `S2_MULTICAST`/
`S2_BROADCAST` (send flags), `S2_TRACKTYPE`/`S2_UNTRACKTYPE`,
`S2_GETTYPESTATS`.

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

## `S2_ONEVENT` completes immediately if already true, else waits for the next transition

**Correction from an earlier version of this doc**, which claimed this was
purely edge-triggered (a request for `S2EVENT_ONLINE` sent to an
already-online unit would block until a spurious offline-then-online
cycle). That was wrong, caught by cross-checking a real shipped SANA-II
driver (plipbox, github.com/cnvogelg/plipbox) rather than by testing the
claim: only `S2EVENT_ONLINE`/`S2EVENT_OFFLINE` map to a persistent state
that can already be "true" at submission time, and for those two, a
request that already matches the current state completes **immediately**
-- which is also the behavior a caller actually wants ("tell me once the
link is up" should return right away if it already is; requiring a
spurious down-then-up cycle first would be a real bug, not a nicety).
Only when the requested state does *not* already hold does the request
queue and wait for the next matching `S2_ONLINE`/`S2_OFFLINE` transition.
The other event bits (`S2EVENT_ERROR`/`TX`/`RX`/`BUFF`/`HARDWARE`/
`SOFTWARE`) describe momentary conditions with no current-state concept,
so they always queue.

## Buffer management hook (`ios2_BufferManagement`) -- implemented since M1

**Correction from an earlier version of this doc**, which called this an
opt-in fault-testing feature deferrable past Phase 1. It isn't optional:
`ios2_BufferManagement` is the mechanism a real caller uses to tell the
device *where its data actually lives* and how to copy it, and skipping it
means real stacks (AmiTCP, Roadshow, TheWire13) simply cannot drive this
device correctly -- CopyMem-ing straight to/from `ios2_Data` happens to work
for a raw test harness that never sets the field, but is spec-incorrect in
general.

The protocol (from `devices/sana2.h`, `utility/tagitem.h`):

1. Before `OpenDevice()`, the caller may point `ios2_BufferManagement` at a
   `struct TagItem` array containing `S2_CopyToBuff`/`S2_CopyFromBuff`
   (function pointers, ABI below) and/or `S2_PacketFilter` (a `struct
   Hook *`, recorded but not yet dispatched by this device). `NULL` means
   "no tags" -- install defaults.
2. The device's `Open()` parses that list with a **local tag walk**
   (`FindTagItem()` needs utility.library, absent on 1.3) that must handle
   the control tags correctly: `TAG_DONE` ends the list, `TAG_IGNORE` skips
   one item, `TAG_MORE` jumps to a chained array (and does **not** advance
   past the jump -- the chained array is now "the current array"),
   `TAG_SKIP` skips `ti_Data` extra items. Getting this wrong is exactly
   the class of bug `FindTagItem()` normally hides from a caller who never
   thinks about it.
3. `Open()` then **replaces** `ios2_BufferManagement` with its own per-open
   cookie. The caller is contractually required to copy that cookie into
   every subsequent `IOSana2Req` it submits on this open -- which is why a
   queued `CMD_READ` already carries the identity of the opener that will
   eventually consume it, with no side table needed.
4. Hook ABI (spec-fixed, not the device's choice): `BOOL Copy*Buff(APTR to
   [a0], APTR from [a1], ULONG size [d0])`. Verified empirically that
   bebbo's gcc honors `asm("a0")`-style register annotations on a
   function-pointer *type* at the call site (not just on a function
   definition) -- confirmed via `objdump` on an isolated test before
   wiring this into the device, since a wrong ABI here would corrupt
   memory silently rather than fault cleanly.
5. Address/type/length fields (`ios2_SrcAddr`/`DstAddr`/`PacketType`/
   `DataLength`) are always written directly by the driver -- only the
   packet *payload* goes through the hook.
6. Hook failure (returns `FALSE`) fails that specific I/O with
   `S2ERR_NO_RESOURCES` / `ios2_WireError = S2WERR_BUFF_ERROR`, not a
   crash or a silently-wrong copy.

Genuinely later-phase, and not implemented yet: **invoking these hooks from
interrupt context** as an opt-in fault knob (SANA-II requires hooks to be
interrupt-safe; testing that a consumer's hooks actually are is real
value), and the `S2_CopyTo/FromBuff16/32`/`S2_DMACopyTo/FromBuff32` Rev 7
variants (recorded nowhere -- no consumer of them exists in this device).

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
