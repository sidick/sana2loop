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
| `S2_GETSTATIONADDRESS` | `CMD_NONSTD+1` | caller | `ios2_SrcAddr` = current address, `ios2_DstAddr` = factory/default address (may diverge once `S2_CONFIGINTERFACE` has run) |
| `S2_CONFIGINTERFACE` | `CMD_NONSTD+2` | caller | first call adopts `ios2_SrcAddr`'s low 6 bytes as the current station address; a second call fails `S2ERR_BAD_STATE`/`S2WERR_IS_CONFIGURED` |
| `S2_TRACKTYPE` / `S2_UNTRACKTYPE` | `CMD_NONSTD+9/10` | caller | fixed 8-slot per-unit table keyed on `ios2_PacketType`; duplicate track / untrack-when-absent both fail `S2ERR_BAD_STATE` (`S2WERR_ALREADY_TRACKED`/`S2WERR_NOT_TRACKED`); table-full fails `S2ERR_NO_RESOURCES` |
| `S2_GETTYPESTATS` | `CMD_NONSTD+11` | caller | fills `struct Sana2PacketTypeStats` via `ios2_StatData` for a tracked `ios2_PacketType`; `S2ERR_BAD_STATE`/`S2WERR_NOT_TRACKED` if that type isn't tracked |
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
now): `S2_ADD/DELMULTICASTADDRESS`, `S2_MULTICAST`/`S2_BROADCAST` (send
flags) -- M4.

## `S2_CONFIGINTERFACE` and the current-vs-factory station address split (M3)

`struct IOSana2Req` has only one `ios2_SrcAddr`/`ios2_DstAddr` pair, reused
by convention (not enforced by the struct) for `S2_GETSTATIONADDRESS`:
`ios2_SrcAddr` = the **current** station address, `ios2_DstAddr` = the
**factory/default** address. They start identical (both set by
`reset_unit()`); `S2_CONFIGINTERFACE` is the only thing that can make them
diverge, by adopting `ios2_SrcAddr`'s low 6 bytes as the new current
address. A second `S2_CONFIGINTERFACE` on an already-configured unit fails
`S2ERR_BAD_STATE`/`S2WERR_IS_CONFIGURED` -- configuring is a one-shot
action per unit lifetime, not a "last write wins" setter.

Deliberately **not** cross-gated with `S2_ONLINE`: some real drivers refuse
to go online before being configured (`S2WERR_NOT_CONFIGURED` exists in the
header for exactly that), but `loopback.device` already hands out a usable
factory address at `reset_unit()` specifically so the tool works with zero
setup -- requiring `S2_CONFIGINTERFACE` first would only get in the way of
that goal.

## `S2_TRACKTYPE`/`S2_UNTRACKTYPE`/`S2_GETTYPESTATS` (M3)

A fixed 8-slot per-unit table (`SANA2LOOP_MAX_TRACKED_TYPES`), consistent
with this device's fixed-capacity-everywhere approach (rxqueue=0 default,
fixed-size `EchoPacket`) -- a caller wanting more than 8 simultaneously
tracked types gets `S2ERR_NO_RESOURCES`, the same error any other
fixed-capacity resource here returns when exhausted. `S2_TRACKTYPE` on an
already-tracked type, or `S2_UNTRACKTYPE`/`S2_GETTYPESTATS` on a type not
currently tracked, both fail `S2ERR_BAD_STATE` with the matching
`S2WERR_ALREADY_TRACKED`/`S2WERR_NOT_TRACKED`.

Once tracked, a type's `struct Sana2PacketTypeStats` (`PacketsSent`,
`PacketsReceived`, `BytesSent`, `BytesReceived`, `PacketsDropped`) is
updated from real traffic, not left at zero: `CMD_WRITE` bumps
`PacketsSent`/`BytesSent`, and `deliver_packet()` bumps
`PacketsReceived`/`BytesReceived` on either outcome that keeps the packet
alive (immediate delivery or acceptance into the `rxqueue`), reserving
`PacketsDropped` for the one outcome that's an actual loss.

## `S2_BROADCAST`/`S2_MULTICAST`, multicast membership, `SANA2IOF_RAW` (M4)

`S2_BROADCAST` is exactly `CMD_WRITE` with `ios2_DstAddr` forced to the
all-1s Ethernet broadcast address first -- confirmed against a real driver
(plipbox, `amiga/src/device.c`), which implements it as literally
`memset(dst, 0xff, ...); /* fall through */ case CMD_WRITE:`. `S2_MULTICAST`
is the same `CMD_WRITE` path but leaves the caller's `ios2_DstAddr` alone,
only validating it actually has the Ethernet group bit set (low bit of the
first octet) -- plipbox never implements `S2_MULTICAST` at all, so this
validation is this device's own choice (the standard 802.3 convention, not
a guess). A completed read's `SANA2IOF_BCAST`/`SANA2IOF_MCAST` flags
(`ios2_Flags`, masked and recomputed every read) are derived from the
delivered packet's actual destination address this same way: all-1s =
broadcast (plipbox's own detection method, confirmed identical), else the
group bit set = multicast (this device's own extension). A plain
`CMD_WRITE`'s delivered destination is unaffected -- still always this
unit's own station address, the original echo semantics -- only
`S2_BROADCAST`/`S2_MULTICAST` writes let the reader see the real
destination, or the flags would never mean anything.

`S2_ADDMULTICASTADDRESS`/`S2_DELMULTICASTADDRESS` take the group address in
`ios2_SrcAddr` (the same "single address of interest" convention
`S2_CONFIGINTERFACE` already uses) and are refcounted in a fixed
8-slot-per-unit table: joining an already-joined group just bumps the
count, since this device's delivery was never filtered per-open membership
in the first place (no per-open receive filtering exists here at all) --
the table is honest bookkeeping, not a live filter. `S2WERR_MULTICAST_FULL`
(11) is a real, dedicated wire error for a full table, unlike
`S2_TRACKTYPE`'s table-full case (M3), which had no dedicated code to reuse.

`SANA2IOF_RAW` here means only "relax the MTU-exceeded check by 14 bytes"
(Ethernet's dst+src+type header size) -- deliberately narrow. This device
does **not** reinterpret the buffer as a literal raw frame with an embedded
header the way a real hardware driver does (plipbox's `write_frame()`/
`read_frame()` genuinely copy/parse a 14-byte header in and out of the
buffer) -- there's no real wire framing here to parse a header out of, so
growing the size budget is the whole of what "raw" usefully means for a
loopback device.

### A real AmigaOS gotcha this surfaced: `SendIO()`/`DoIO()` clear `io_Flags`

Both exec.library's `SendIO()` and `DoIO()` explicitly reset `io_Flags`
before dispatching to a device -- confirmed from their own autodocs:
`SendIO()` "will be set to zero before the request is sent," `DoIO()` "is
always set to IOF_QUICK (0x01) before the internal device call." Neither
preserves a caller-set bit like `SANA2IOF_RAW`. Only `amiga.lib`'s
`BeginIO()` does ("equivalent to `SendIO()`, except that `io_Flags` is not
cleared") -- confirmed against AROS's own open-source implementation of it
(`compiler/alib/beginio.c`): a bare LVO -30 dispatch, nothing else, no
`Forbid()`/`Permit()`, no other bookkeeping. A survey of real SANA-II
client code (Olaf Barthel's `amiga-sana-ii-tftpclient`, an intentional
reference SANA-II consumer) found it never touches `io_Flags`/
`SANA2IOF_RAW`/`_BCAST`/`_MCAST` at all -- it has no need for raw-frame
mode, so there's no real-world precedent to confirm this against beyond the
autodocs and AROS's own source. `tests/copperline/sana2test.c`'s
`flagged_begin_io()` reimplements `amiga.lib`'s `BeginIO()` by hand (this
freestanding build links no amiga.lib) as a direct JSR to the device's
BeginIO vector -- the standard exec device jump-table slot at LVO -30
(`Open=-6, Close=-12, Expunge=-18, ExtFunc=-24, BeginIO=-30, AbortIO=-36`),
exactly what `SendIO()`/`DoIO()` already jump to internally, just without
their `io_Flags`-clearing side effect first.

**A second, sharper lesson from chasing this down**: the first version of
`flagged_begin_io()`'s inline asm passed `a1` as an input register (holding
the `IORequest*`) but omitted it from the clobber list. This built cleanly
and worked perfectly against Copperline's bundled AROS -- but hung
indefinitely against a real Kickstart 1.3 ROM, reproducibly, even at a very
generous benchmark timeout. `a1` is caller-scratch on m68k/Amiga (the
callee is free to trash it), so GCC must be told so explicitly or it may
make invalid assumptions about the register's value surviving the call
depending on surrounding register pressure and codegen -- exactly the kind
of undefined behavior that "happens to work" on one target/build and not
another. The already-working `raw_put()` helper's own inline asm (calling
`RawPutChar` via `jsr -516(%a6)`) already clobbers `a1` correctly, which is
precisely why it never hit this. Fix: add `"a1"` to the clobber list
alongside the other caller-scratch registers. **A hang that reproduces on
real 1.3 but not AROS is not automatically a compatibility quirk to work
around or document away (unlike the real, confirmed `CheckIO()` AROS
quirk elsewhere in this file) -- treat it as a real bug until proven
otherwise**, especially in hand-written inline asm, where AROS's emulation
may simply be more forgiving of an incomplete clobber list than real
hardware/ROM timing.

## Config-file infrastructure (M5)

`ENV:sana2loop/unit<N>.config`, falling back to `S:sana2loop/unit<N>.config`
if that doesn't exist -- read once at first-open (`unit_OpenCnt` 0->1, the
same trigger `reset_unit()` already uses), and only when the opener is a
real Process (`FindTask(NULL)->tc_Node.ln_Type == NT_PROCESS`), never from
a bare Task, since dos.library's `Open()`/`Read()`/`Close()` need
process-level context (current directory, `IoErr()` scoping, ...) a Task
doesn't have. Line-based `key=value`, `#`-led comments, blank lines
skipped, no whitespace tolerance around the `=` -- a strict format on
purpose (matches the fail-fast philosophy below). Neither `ENV:` nor `S:`
having the file at all is **not** an error -- most units on most systems
have no config file, and compiled-in defaults apply -- but a file that
**exists** and fails to parse fails the whole `OpenDevice()` call with
`S2ERR_BAD_ARGUMENT` (an unrecognized key is exactly as fatal as a
malformed line: silent typo-tolerance is wrong for a test tool).

Only `mtu` and `rxqueue` are recognized right now -- the only two per-unit
knobs that already exist as real device state. The proposal's full config
vocabulary (`mode`/`peer` for M6 crossover; `seed`/`droppct`/`duppct`/
`reorder`/`truncate`/`oversilent`/`offlineafter`/`onlineafter` for M7 fault
injection; `replay`/`record`/`replaymode`/`recordmax` for M8-M9) has
nothing to configure yet -- each key lands with the milestone that actually
consumes it, not before (a recognized key with no backing feature would be
dead code). `mtu` is bounded above by `SANA2LOOP_MAX_PACKET -
SANA2LOOP_RAW_HDR_SIZE` (1500) -- a configured MTU above that would let a
RAW write's relaxed size check (M4) exceed the fixed `EchoPacket` buffer's
actual capacity. `rxqueue` is bounded by a sanity ceiling (64), not a spec
requirement -- just a guard against a typo'd huge value exhausting real
1.3's limited memory pool. Parsing needs only multiply-and-add (`v = v*10
+ digit`), never division, so this milestone didn't end up forcing the
`-lgcc`-vs-local-div-helpers decision docs/PLAN.md flags as an M5 risk --
that stays deferred until a later milestone's percentage-threshold math
(M7) actually needs 32-bit division.

### Two real on-target findings from proving the M5 risk note

docs/PLAN.md flagged "DOS I/O breaking Forbid inside `open()` on real 1.3"
as a risk to verify at first touch. What was actually found, on-target, on
both bundled AROS and real Kickstart 1.3:

1. **The real hazard wasn't `open()`'s calling context at all -- it was
   `ENV:` not being assigned.** This project's own minimal boot ADF
   deliberately has no `Assign ENV:` (see CLAUDE.md's "Use a minimal
   from-scratch bootable ADF"). `Open("ENV:...")` against an unrecognized
   device/assign name hung indefinitely rather than failing cleanly --
   confirmed via temporary inline-`RawPutChar` tracing added directly to
   the device (the usual "leak a value through `ios2_WireError`"
   debugging trick doesn't work here, since the hang happens inside
   `OpenDevice()` itself, before any `IORequest` ever completes). DOS
   treats an unrecognized name as a possible removable-volume reference
   and puts up a blocking "please insert volume" requester -- fine
   interactively, fatal headless, and **not** something to special-case
   away for the test ADF specifically: any real deployment where `ENV:`
   happens to be unassigned would hang a caller's task the exact same way.
   Fixed at the right layer: the calling process's `pr_WindowPtr`, the
   standard AmigaOS mechanism for "suppress DOS requesters for me, fail
   cleanly instead of prompting" -- set to `(APTR)-1` for the duration of
   the `Open()` attempts in `read_unit_config()`, then restored
   unconditionally (both existing-file and no-file-at-all paths) so the
   suppression never leaks into the caller's own later DOS operations.
2. **Once that was fixed, dos.library I/O from inside `open()` worked
   correctly on both targets with no other special handling needed** --
   no Forbid()-related deadlock, no corruption. Real Kickstart 1.3 turned
   out measurably *slower* at this than AROS, though: `tests/copperline/
   run.sh`'s `KICK=` benchmark default needed raising (10s -> 30s,
   confirmed reliable across repeated runs) once M5's real dos.library
   file reads landed, while the AROS default was untouched. Plausible
   explanation (not proven by cycle-level profiling): Kickstart 1.3's
   filesystem (OFS) is a genuinely unoptimized implementation by later
   standards -- linear directory scans, minimal buffering -- and AROS's
   modern reimplementation was never built to be cycle-faithful to that,
   just functionally compatible.

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
