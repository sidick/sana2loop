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
| `S2_GETSPECIALSTATS` | `CMD_NONSTD+12` | caller | up to 2 records (M13 added the 2nd): `Type=SANA2LOOP_SPECIALSTAT_RECORD_OVERFLOW` if `record=<path>` (M9) has dropped a packet to `recordmax`, `Type=SANA2LOOP_SPECIALSTAT_DMA_HOOKS_OFFERED` (M13) if any open negotiated a Rev 7 DMA hook; zero records if neither |
| `S2_GETPEERADDRESS` (M13) | `0xC002` (local `#define` -- not in this toolchain's header) | caller | `ios2_SrcAddr`/`DstAddr` = local/peer station address; crossover mode only, else `S2ERR_NOT_SUPPORTED` |
| `S2_GETDNSADDRESS` (M13) | `0xC003` (ditto) | caller | always succeeds, both addresses zero-filled (no DNS concept on any mode) |

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

`mtu`/`rxqueue` (M5), `mode`/`peer` (M6, see its own section below),
`seed`/`droppct`/`duppct`/`truncate`/`oversilent`/`reorder`/`offlineafter`/
`onlineafter` (M7, fault injection), `mode=replay`/`replay` (M8), and
`replaymode`/`record`/`recordmax` (M9, see its own section below) are all
recognized now -- every key the proposal's original vocabulary named.
`mtu` is
bounded above by `SANA2LOOP_MAX_PACKET -
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

## Crossover mode (M6)

A unit is either standalone **echo** (Phase 1 behavior, unchanged) or
**crossover**-paired with exactly one other unit -- a config surface
entirely of this device's own invention (`mode`=`echo`|`crossover`,
`peer`=<unit index>; SANA-II itself has no such concept). Compiled-in
default, needing no config file at all: units 2 and 3 pair with each
other; every other unit defaults to echo. `read_unit_config()` rejects
`mode=crossover` with no valid, distinct `peer=` (an echo-mode unit's
default peer is itself, which is what makes "crossover with no peer"
concretely fail this check rather than silently self-delivering).

Crossover changes two things echo mode never had to think about, because
echo mode has no real second party:

1. **Addressing is real, not swapped.** Echo's plain `CMD_WRITE` fakes
   both ends (src = whatever address the caller wrote to, dst = this
   unit's own station) because there is no actual peer to ask. Crossover
   mode (any write type) uses real addressing instead: src is the WRITING
   unit's own actual station address, dst is exactly what the caller
   specified. This also fixed a latent bug from M4: echo's own
   `S2_BROADCAST`/`S2_MULTICAST` had reused the swap machinery's `src =
   ios2_DstAddr` line unconditionally, meaning a broadcast frame's "source"
   was the broadcast address itself -- wrong, and never caught because
   M4's own tests only ever asserted the delivered *destination*, not
   source. Fixed as a side effect of building this properly for crossover
   (`do_write()`'s `swap_to_self` now gates the swap specifically, not
   generally).
2. **Delivery filters by destination.** `deliver_packet()` targets the
   PEER unit for a crossover write (`&sld->sld_Units[unit->su_Peer]`, the
   whole reason it takes a unit parameter rather than always operating on
   the writer). For a crossover-mode target, every candidate -- a typed
   `CMD_READ`, an `S2_READORPHAN`, or the `rxqueue` itself --  must also
   pass `dest_matches_unit()`: standard 802.3 receive filtering (unicast
   to the target's own station address, the all-1s broadcast address, or
   a joined multicast group from M4's table) **unless** the specific
   candidate reader's own open has `SANA2OPF_PROM` set, which finally
   gives that flag real meaning (previously ignored entirely: "every
   opener already sees everything on its unit," true only for echo mode).
   `rxqueue` queuing never gets promiscuous treatment -- it's a shared,
   unit-wide buffer for a not-yet-known future reader, not a specific
   open's own request, so there's no meaningful "this reader is
   promiscuous" to consult at queue time. Echo mode stays fully unfiltered
   -- there is no separate peer to have misaddressed the packet at, so
   filtering would only get in the way of the simplest test scenario, the
   original Phase 1 design intent.

## Fault injection pipeline (M7)

All disabled by default (`reset_unit()`): a plain write with no fault
config behaves exactly as it did before this milestone. Fixed order, run
in `do_write()` after `CopyFromBuff` succeeds: MTU-vs-peer (`oversilent`)
-> `droppct` -> `truncate` -> `duppct` -> `reorder`. `offlineafter` is a
side effect ticked once at the very end, after the packet (and its
duplicate, if any) have already been handed off -- not really "part of"
shaping this one packet, more a consequence of having sent it at all.

- **PRNG**: xorshift32, one `ULONG` of state per unit, seeded from config
  `seed=N` (0 coerced to 1 -- xorshift32 never leaves the all-zero state
  otherwise) and re-seeded fresh at every 0->1 open transition, same
  determinism boundary as everything else `reset_unit()` covers.
- **Percent thresholds** (`droppct`/`duppct`, 0-100): `pct*655` compared
  against the upper 16 bits of a draw, approximating `pct`/100 without a
  32-bit division this `-nostdlib` build has no libgcc for. 0 and 100 are
  exact boundaries, not leaning on the approximation at the edges --
  100*655=65500, not the full 0-65535 range, so without special-casing it
  a configured `droppct=100` would still let ~0.05% of writes through.
- **`reorder=N`** (0-`SANA2LOOP_MAX_REORDER`, 8): a fixed-size, per-unit
  buffer of packet pointers. New packets are appended until the buffer
  reaches its configured window; once full, a PRNG-chosen SLOT (not
  necessarily the newest arrival) is evicted for immediate delivery, and
  the remaining slots are shifted down to close the gap -- preserving
  their *relative* arrival order, which is what lets a final flush (see
  below) just walk the buffer front-to-back with no separate bookkeeping.
  This bounds how far out of order a packet can arrive (at most
  `window - 1` later packets can overtake it before it's forced out), and
  it means the PRNG only ever needs to draw an index in a small range --
  done via masking to 3 bits and rejecting an out-of-window draw, NOT
  `% window` (a runtime-variable modulo compiles to a `__umodsi3` call on
  this `-m68000` target regardless of how small the operands turn out to
  be, since the compiler can't see that from a variable divisor -- the
  exact same class of libgcc trap M3's `memmove`-idiom discovery already
  taught this project to watch for, just for division instead of copies).
  Anything still buffered gets flushed, in genuine arrival order (no
  further permutation), on `S2_OFFLINE` (manual or `offlineafter`'s
  auto-trigger), `CMD_FLUSH`, or last-close -- so nothing is ever silently
  stuck there forever once nothing's left to push it back out.
- **`oversilent=1`**: only meaningful for crossover-mode writes (echo's
  delivery target IS the writer, so its own MTU check already covers the
  case). If a packet fits the writer's own MTU but exceeds the crossover
  PEER's, this makes the write succeed (`io_Error` stays 0 -- the caller
  is never told) while the packet silently vanishes instead of ever
  reaching `deliver_packet()`. This simulates a real PMTUD (Path MTU
  Discovery) black hole: an oversized frame that should trigger "packet
  too big" feedback (or, at the IP layer, an ICMP fragmentation-needed
  message) instead just disappears, exactly the pathology PMTUD itself
  exists to detect and route around.
- **`offlineafter`/`onlineafter`**: packet-count-driven auto-flapping,
  simulating a flaky link. `su_WriteCounter` counts successful writes
  since the last online transition; once it reaches `offlineafter`, the
  unit auto-forces itself offline (same transition as a manual
  `S2_OFFLINE`, including the reorder-buffer flush) for the *next* write
  attempt -- the write that crosses the threshold still completes
  normally, this isn't retroactive. Symmetrically, `su_FailCounter` counts
  failed (`S2ERR_OUTOFSERVICE`) write ATTEMPTS while offline; once it
  reaches `onlineafter`, the unit auto-recovers online for the next
  attempt (that attempt itself still fails).
- A latent M4 bug surfaced and fixed while building this: `do_write()`'s
  addressing branch for "real, unswapped" delivery (crossover mode, or
  echo's own `S2_BROADCAST`/`S2_MULTICAST`) now fills `ep_SrcAddr` from
  the writer's own actual station address. M4's version had reused the
  swap-mode's `src = ios2_DstAddr` line unconditionally for ALL non-echo
  writes, meaning a broadcast frame's "source" was wrongly the broadcast
  address itself -- never caught because M4's own broadcast/multicast
  tests only ever asserted the delivered *destination*, not source.
- **A real on-target bug this milestone's own harness found**: the
  `faultreorder` sub-test's final-flush helper allocated a third
  `IOSana2Req`/`MsgPort` pair but never called `OpenDevice()` on it before
  using it for `S2_READORPHAN` -- `SendIO()` on a request with a NULL
  `io_Device` doesn't fail cleanly, it hangs indefinitely (confirmed
  identical on bundled AROS and real Kickstart 1.3, so a genuine harness
  bug, not a target-specific quirk). Found by bisecting with temporary
  inline-`RawPutChar` tracing on both sides (device and harness) until the
  exact missing `OpenDevice()` call was obvious from where the trace
  simply stopped -- the same "leak a value/checkpoint through direct
  serial output" technique this project has reached for before, since the
  hang happens before any `IORequest` involved ever completes (so there's
  no `ios2_WireError`/`ios2_Req.io_Error` to inspect after the fact).

## pcap core + consume-mode replay (M8)

`src/pcap.h`: hand-rolled classic pcap ("libpcap savefile") format, no I/O
in the header at all -- callers own Open/Read/Write, this only knows how
to make sense of bytes already in memory. Both endians deliberately:
`PCAP_MAGIC_NATIVE`/`PCAP_MAGIC_SWAPPED` let a reader detect, from the
file's own magic number, which byte order every OTHER numeric field in
that file uses -- decided once per file, not by whichever CPU happens to
be reading it. A big-endian 68000 reading a little-endian-written host
capture (scapy/tcpdump -- the common case) needs `swapped=TRUE`, despite
nothing about the 68000 itself changing. `pcap_read_u16()`/`pcap_read_u32()`
read a raw byte buffer honoring that; `pcap_parse_file_header()`/
`pcap_parse_pkthdr()` build an already-parsed, native-typed, in-memory
struct from it. Deliberately never cast a raw file buffer directly to
`struct pcap_file_header`/`pcap_pkthdr` -- that would depend on this
compiler's struct padding matching the on-disk layout, which isn't
guaranteed (and the explicit parse functions cost nothing extra).

A `mode=replay` unit plays back a pre-loaded pcap file's records in order,
one per `CMD_READ`/`S2_READORPHAN` -- "consume mode" (docs/PLAN.md decision
#7): no timestamp/VBlank scheduling at all, just a scripted, in-order
drain. M9 owns the real tick-scheduled (VBlank-driven, timestamp-honoring)
replay variant this deliberately doesn't attempt yet.

- **Config**: `mode=replay` plus `replay=<path>` (a dos.library-openable
  file, read the SAME way config files are -- `pr_WindowPtr` suppression,
  process-context gating). Unlike a missing/malformed CONFIG file (which
  just means "use defaults"), a missing/malformed REPLAY file is a hard
  open-time failure (`S2ERR_BAD_ARGUMENT`) -- there's no sensible default
  capture to fall back to for a `mode=replay` that named one specifically.
- **Loading** (`load_replay_file()`, called once at open time, same 0->1
  determinism boundary as everything else `reset_unit()`/`open()` cover):
  parses each captured Ethernet frame's first 14 bytes (dst/src/type) into
  the exact same `struct EchoPacket` shape `CMD_WRITE` already builds, so
  `CMD_READ`/`S2_READORPHAN`'s existing `fill_read_from_packet()` serves a
  replayed record with no separate delivery path. Fail-fast on: wrong
  `linktype` (only `LINKTYPE_ETHERNET` supported), a record shorter than a
  full Ethernet header or bigger than this device ever builds, a
  snaplen-truncated record (`incl_len != orig_len` -- replaying a
  shortened frame would be replaying something that never really existed
  on the wire), more records than `SANA2LOOP_MAX_REPLAY_PACKETS` (16), or
  an empty capture.
- **Typing is ignored**: `CMD_READ` on a replay-mode unit behaves exactly
  like `S2_READORPHAN` -- both just mean "give me the next captured frame,
  whatever type it turns out to be." Once the loaded sequence is
  exhausted, a further read just queues like any unit with no traffic
  waiting -- SANA-II has no "end of stream" concept, so this device
  doesn't invent one.
- **`CMD_WRITE`/`S2_BROADCAST`/`S2_MULTICAST` on a replay-mode unit fail
  `S2ERR_NOT_SUPPORTED`**: a canned trace has nowhere meaningful to send
  anything. Checked ahead of the `su_Online` check in `do_write()` --
  this is a fundamental "not supported for this mode," not a temporary
  unavailability.
- **`su_ReplayNext` is a running POINTER, not an array index.**
  `struct EchoPacket` isn't power-of-2 sized (its
  `ep_Data[SANA2LOOP_MAX_PACKET]` dominates), so indexing it by a runtime
  variable (`su_ReplayPackets[su_ReplayPos]`, or the equally-tempting
  `packets[count]` while loading) needs an actual
  `count*sizeof(struct EchoPacket)` multiply -- which compiles to an
  unavailable `__mulsi3` call on this `-nostdlib -m68000` build. A plain
  pointer increment (`su_ReplayNext++`) is just `address += a
  compile-time-constant step`, never a multiply, so replay consumption and
  loading both walk forward with a cursor instead of indexing.
- **`__attribute__((noinline))` on `load_replay_file()` is load-bearing**,
  not style: inlined into `open()` (its only call site, the classic shape
  GCC auto-inlines), the compiler's register allocator rematerialized the
  `unit` pointer via exactly the runtime-multiply-by-`sizeof(Sana2LoopUnit)`
  this milestone otherwise avoided, rather than keeping it live in a
  register across the larger merged function -- broke the build with the
  same undefined `__mulsi3` reference. Every OTHER `sld_Units[]` index site
  in this file (`delivery_target()`, `init_device()`'s setup loop) needs
  the identical multiply and has so far stayed under whatever shift/add
  budget this backend allows before falling back to a library call; this
  one didn't, once inlined. Any future milestone growing `struct
  Sana2LoopUnit` further should re-check for the same failure by rebuilding
  and grepping for `__mulsi3` -- the exact byte count that tips this over
  isn't something to reason about in advance.
- **`SANA2LOOP_MAX_REPLAY_PACKETS=16` was picked empirically.** The
  original guess (32, ~49 KB) reliably failed its own `AllocMem()` under
  bundled AROS specifically (confirmed via a temporary `AvailMem(MEMF_LARGEST)`
  probe) -- AROS, unlike a real ROM in true ROM space, has to load its own
  Kickstart replacement INTO the test profile's 512K+512K chip+fast budget,
  leaving measurably less free than real Kickstart 1.3 does on the
  identical profile. 16 (~25 KB) passed with headroom on both targets.
- Always the FULL `SANA2LOOP_MAX_REPLAY_PACKETS`-slot allocation is made
  at load time, not one sized to the capture's actual record count -- that
  count isn't known until the whole file has been read, and a two-pass
  read (count, then allocate exactly, then re-read) buys nothing a
  fixed-size allocation doesn't already give this device elsewhere
  (`rxqueue`, `S2_TRACKTYPE`, `S2_ADD/DELMULTICASTADDRESS`).
  `reset_unit()`'s matching `FreeMem()` (freeing a previous lifetime's
  buffer at the next 0->1 transition) must use this same fixed size, not
  `su_ReplayCount`.

## VBlank tick server, tick-scheduled replay, RAM record (M9)

`config-file infrastructure` gains `replaymode` (`consume` -- M8's
existing on-demand behavior, still the default -- or `scheduled`),
`record=<path>`, and `recordmax` (default 64 KB). One VBlank interrupt
server (`vblank_server()`, `AddIntServer(INTB_VERTB, ...)`, added once in
`init_device()`) drives BOTH new features: it ticks `su_Tick` for any unit
that's online AND (in scheduled-replay mode OR recording), and for
scheduled units, delivers each record whose due tick has arrived through
the SAME `deliver_packet()` precedence echo/crossover writes use (per
docs/PLAN.md decision #3 -- one delivery path, not a parallel mechanism).
A fresh heap copy is made (`AllocMem` + `CopyMem` from the read-only loaded
array) before handing it to `deliver_packet()`, since that function's
existing contract always consumes/frees/queues its `pkt` argument, and the
loaded replay array is NOT individually-owned per record.

- **Due-tick scheduling is whole-second, not microsecond-exact.** Each
  loaded record's due tick = `(ts_sec - first_record's ts_sec) * 50`
  (`SANA2LOOP_VBLANK_HZ`, a fixed PAL assumption -- this device has no
  timer.device dependency and no interest in real-world timing fidelity,
  only in determinism under a fixed emulation). `ts_usec` is never
  consulted, deliberately: converting it to ticks would need a genuine
  runtime division/multiply against a non-power-of-2 constant -- exactly
  the class of expression this project has twice already found compiles
  to an unavailable `__udivsi3`/`__mulsi3` on this `-nostdlib` build.
  Records captured in the same real second share a due tick and are
  delivered back-to-back, in file order (already guaranteed by sequential
  consumption) -- a deliberate, documented simplification.
- **`su_ReplayDueTicks` is a `ULONG` array, safely indexable by a runtime
  variable** (power-of-2 element size) -- unlike `su_ReplayPackets`
  (`struct EchoPacket`, NOT power-of-2), which still only ever gets walked
  via the `su_ReplayNext` running-pointer idiom M8 established.
- **`load_replay_file()` validates monotonic timestamps** (`ts_sec` never
  decreasing between consecutive records) -- needed so the due-tick
  subtraction can never underflow on a corrupt or out-of-order capture;
  fails the whole open, same as every other malformed-capture case.
- **CMD_READ/S2_READORPHAN's M8 "serve directly from `su_ReplayNext`"
  shortcut is now gated on `su_ReplayMode == CONSUME`.** A SCHEDULED unit's
  reads fall through to completely normal queuing (typed reads still
  respect `ios2_PacketType`; only `S2_READORPHAN` gives "whatever's next"
  semantics) and wait for `vblank_server()` to deliver -- the whole point
  of this milestone versus M8's on-demand behavior.
- **`record_append()` taps `deliver_packet()` unconditionally** (delivered,
  queued, or dropped -- matching how `PacketsReceived` already counts
  regardless of outcome), appending a native-endian (big-endian --
  `pcap_write_u32`/`u16` in `pcap.h`, no need to ever write little-endian
  from this device's own recorder) pcap record with `su_Tick` as `ts_sec`
  (a virtual tick count, NOT real wall-clock time) and `ts_usec` always 0.
  Overflow (`su_RecordMax` reached) stops appending further packets
  (`su_RecordDropped` counts how many) but never truncates/corrupts what's
  already captured; surfaced via `S2_GETSPECIALSTATS` per docs/PLAN.md
  decision #7 ("overflow = stop + count") -- the ONE record type this
  device defines, `SANA2LOOP_SPECIALSTAT_RECORD_OVERFLOW`, written
  immediately after the caller-supplied header per the standard (if
  underdocumented in the bare struct) SANA-II convention of records living
  contiguous with the header in the same `ios2_StatData` buffer.
- **Two real bugs found and fixed while building this, both worth
  remembering:**
  1. `build_config_path()`'s original one-line `'0' + unit_index` digit
     substitution (from Phase 1) silently produced `:`/`;` instead of
     `10`/`11` for double-digit unit numbers -- a LATENT bug that survived
     M7 (bumped to 9 units) and M8 (bumped to 10) purely because every
     unit number through then was still single-digit; M9 was the first
     milestone to actually reach unit 10. Not a parse error (a config file
     simply wasn't found under its real name, which read_unit_config()'s
     own contract treats as "no config, defaults apply") -- it looked
     exactly like "the config isn't being applied" until traced back to
     the path string itself. Fixed with a division-free (repeated
     subtraction) tens/ones digit split, handling 0-99.
  2. `SANA2LOOP_MAX_REPLAY_PACKETS` (M8's empirically-chosen 16, picked
     against a SINGLE replay unit's memory footprint) needed shrinking
     again, to 4 -- not just from M9's own per-unit field growth eating
     into bundled AROS's already-tight headroom, but because this
     device's own "free memory only at the NEXT open, never at close"
     design (`reset_unit()`'s documented contract) means a SECOND
     replay-mode unit opened later in the same run has to coexist with
     the FIRST one's still-held buffer -- the real constraint turned out
     to be two simultaneous `SANA2LOOP_MAX_REPLAY_PACKETS`-sized
     allocations, not one, confirmed via the same `AvailMem(MEMF_LARGEST)`
     probing technique M8 used.
- **`__attribute__((noinline))` verification extended to `vblank_server()`
  in a different way than `load_replay_file()`'s: register convention AND
  the VERTB "always return with Z set" calling-convention requirement
  were both confirmed by objdump before trusting them** (same discipline
  M1 established for buffer-management hook calls) -- `A1` correctly holds
  `is_Data` at entry, and the function's single `return 0;` compiles to a
  `moveq #0,d0` immediately before the epilogue's `movem.l`/`rts` (MOVEM
  doesn't touch condition codes on 68k, so Z survives from the `moveq`
  through to the `rts`).
- **Copperline persists floppy writes back to the host ADF file** --
  confirmed empirically (write a known file to `S:`, extract it post-run
  via `xdftool type`, byte-identical) before building the record-flush
  feature on this assumption -- but ONLY once `write_protected = false` is
  set in `[floppy.dfN]` (`machine.toml`); it defaults to `true` (safe by
  default), which otherwise fails the guest's `Open(..., MODE_NEWFILE)`
  with `ERROR_DISK_WRITE_PROTECTED` (214) -- found the hard way via a
  standalone write-test program before ever touching the real feature.
- **Resolved: bundled AROS under Copperline initially hung once a unit
  actually used `replaymode=scheduled` or `record=`** -- confirmed NOT a
  simple "AROS doesn't dispatch custom VBlank servers" via an isolated
  standalone test (it does, verified: 723 hits over 30 emulated seconds).
  The first theory tried -- `AllocMem()` from real interrupt context
  (`vblank_server()`'s per-due-record fresh-copy allocation) hitting an
  AROS-specific allocator limitation -- was tested directly (moved the
  allocation to `load_replay_file()`'s ordinary task context instead,
  `su_ReplayDeliveryCopies[]`) and **disproven**: the hang persisted
  unchanged with zero allocator calls left in the interrupt path. Root
  cause, found by stashing per-stage progress markers into otherwise-
  unused `Sana2DeviceStats` fields inside `deliver_packet()`/
  `vblank_server()` and reading them back through a SECOND, independent
  `IOSana2Req` (so they stayed visible even if the hung request's own
  task never resumed): **bundled AROS's `CheckIO()`/`WaitIO()` can both
  falsely report an `IOSana2Req` as already complete immediately after
  `SendIO()`, before the device has actually replied it** -- specifically
  for a request destined to complete via a genuine hardware-interrupt-
  driven `ReplyMsg()` (M9's `vblank_server()`), unlike every earlier
  sub-test's task-context completion. The stashed markers proved
  `deliver_packet()` had not even been entered at the moment a plain
  `WaitIO()` returned "success" with stale data. The test harness then
  reusing that (AROS's-eyes-complete, actually-still-enqueued)
  `IOSana2Req` for the next record double-inserted it into the device's
  own `su_OrphanReads` list, corrupting it -- THAT corruption (an
  infinite loop the next time `deliver_packet()` walks the broken list
  from inside a real interrupt) is what produces the observed full,
  permanent guest freeze, not the device's own interrupt handling as
  such. Real Kickstart 1.3 never emits the false-completion signal, which
  is why this was invisible there. Fixed entirely in
  `tests/copperline/sana2test.c` (`reap_interrupt_io()`): wait on
  `g_HookToCount` reaching an expected value -- a side effect only the
  real `S2_CopyToBuff` hook invocation can produce, immune to the false
  CheckIO/WaitIO signal -- then unconditionally `AbortIO()` (a documented
  no-op if genuinely already complete) followed by `WaitIO()`,
  guaranteeing the request is cleanly reaped either way before any reuse.
  No device-code change was needed for the actual fix, though
  `su_ReplayDeliveryCopies[]` was kept as a real (if not root-cause)
  hygiene improvement. Verified 5/5 clean runs on bundled AROS, 3/3 on
  real Kickstart 1.3, after the harness change. **Lesson for future
  interrupt-driven-completion tests in this file**: never trust a bare
  `CheckIO()`/`WaitIO()` immediately after `SendIO()` for a request whose
  completion depends on a real hardware interrupt on bundled AROS --
  either wait on independent evidence of the real side effect, or always
  reap via `AbortIO()`+`WaitIO()` regardless of what `CheckIO()` claims.

## SANA-II Rev 2/3/4/7 additions (M13)

These revisions were never part of any vendor NDK release -- SANA-II's own
original spec is Commodore's, and its post-Commodore extensions live only
as informal documentation on wiki.amigaos.net (authored largely by Olaf
Barthel). Confirmed empirically: the bundled AROS-derived
`devices/sana2.h` this project's toolchain ships already has Rev 2/3's
`S2_CopyToBuff16/32`/`S2_CopyFromBuff16/32`/`S2_DMACopyToBuff32/FromBuff32`
tag values (`S2_Dummy+4` through `+9`) and the `0xc000`/`0xc001` NSD-style
command numbers for `S2_ADD/DELMULTICASTADDRESSES` (plural, the batch-list
variants) -- but nothing from Rev 4 or the rest of Rev 7. Where the header
was silent, `loopback_device.c` and every tool that needs the same values
(`sana2test.c`, `sanaconform.c`) each carry their own local `#define`s,
cross-checked against wiki.amigaos.net directly rather than trusted from
memory.

**Rev 2/3 -- buffer-hook width negotiation, genuinely invoked, not just
accepted.** `S2_CopyToBuff16/32`/`S2_CopyFromBuff16/32` use the *exact
same* calling convention as the base `S2_CopyToBuff`/`S2_CopyFromBuff`
hooks (confirmed by wiki.amigaos.net's "Revision 2+3" page: "the same
calling interface as ... respectively") -- the only difference is an
alignment hint a driver is free to ignore ("advisory only and may be
ignored by the driver for any data buffer at any time"). `apply_buffer_
mgmt_tags()` records whichever of the four an opener supplies (`NULL` if
not offered) alongside the existing base-hook slots. A new per-unit
config knob, `buffhooks=8|16|32` (default `8` -- meaning "always the base
hook", so every milestone before M13 is byte-for-byte unaffected), then
picks which width `pick_copy_to_buff()`/`pick_copy_from_buff()` actually
invoke for that unit's traffic -- but only if the specific opener
involved actually negotiated that width; one that didn't falls back to
its own base hook transparently, the same graceful-decline path a real
driver takes. This is the "stack actually invoking it" half of Rev 2/3
conformance the proposal calls for -- accepting the tag at `OpenDevice()`
time is necessary but not sufficient to prove a driver really uses it.

**Rev 4 -- `S2_GETPEERADDRESS`/`S2_GETDNSADDRESS`, `RawMTU`.** Command
values `0xC002`/`0xC003` (quoted from wiki.amigaos.net's "Revision 4"
page, and independently corroborated: they continue the exact `0xc000`/
`0xc001` sequence already in the bundled header). `ios2_SrcAddr`/
`ios2_DstAddr` reuse convention: local/peer for `S2_GETPEERADDRESS`
(same "one address pair, two different meanings depending on command"
pattern `S2_GETSTATIONADDRESS` already established for current/factory),
primary/secondary DNS server for `S2_GETDNSADDRESS`. Only crossover mode
has a real peer (echo's "peer" is itself; replay has no live link at
all) -- both other modes fail `S2ERR_NOT_SUPPORTED`, matching `do_write()`'s
own established precedent for "not a supported operation on this unit's
current mode". `S2_GETDNSADDRESS` has no real concept on any mode this
device supports at all, so it deliberately takes the *other* shape of
"not applicable" instead: zero-filled addresses with `io_Error` staying
0 (success) -- a real stack has to gracefully tolerate both an outright
`S2ERR_NOT_SUPPORTED` and a soft "supported, nothing to report", so this
device now demonstrates both on purpose. `RawMTU` extends
`struct Sana2DeviceQuery` past `HardwareType` -- since the bundled header
doesn't have the field at all, `loopback_device.c` (and every tool that
queries it) each carry a local `struct Sana2LoopDeviceQuery` with the
identical byte layout plus `RawMTU` appended, written only when the
caller's own `SizeAvailable` covers the extended struct's full size --
the exact growth-safe negotiation `SizeAvailable`/`SizeSupplied` exists
for in the first place (see `S2_DEVICEQUERY` convention below). Value:
`MTU + 14` for echo/crossover (the identical number M4's own
`SANA2IOF_RAW` allowance already uses -- `RawMTU` is that same number,
just given a name), `0` for replay-mode units (no raw-write support
there at all, "0" being the spec's own documented value for "not
supported").

**Rev 7 -- DMA hooks, accepted and counted, never invoked.** No published
numeric constant exists anywhere for `S2_DMACopyToBuff64`/
`S2_DMACopyFromBuff64` (Rev 7's own doc names the tags but never gives
hex values) -- `S2_Dummy+10`/`+11` here is this project's own inferred
continuation of the bundled header's existing `+4`..`+9` sequence, not a
confirmed spec value. Harmless if wrong: this device only ever *records*
a DMA hook pointer (as an opaque `APTR`, never a typed function pointer
-- the real DMA calling convention doesn't need to be nailed down when
nothing ever calls through it), never invokes one. A hardware-less RAM
loopback has nothing to hand a DMA-capable buffer address to in the
first place, and declining is fully spec-legal ("up to the driver to
decide if it can use DMA for this buffer and it shall fall back to the
standard CPU callbacks if necessary"). To make that a genuinely testable
claim rather than a silent no-op, every open that supplies at least one
DMA tag bumps a new per-unit counter (`su_DMAHooksOffered`), surfaced via
`S2_GETSPECIALSTATS` as a new record type
(`SANA2LOOP_SPECIALSTAT_DMA_HOOKS_OFFERED = 2`, alongside M9's existing
`SANA2LOOP_SPECIALSTAT_RECORD_OVERFLOW = 1`) -- a caller can confirm the
driver correctly *recognized* the negotiation even though it always
gracefully falls back for the actual copy.

**New probe tool: `SanaConform`.** A model-citizen conformance/quirk tool
(`src/tools/sanaconform.c`, V36+/2.0+ like the other companion tools)
exercising both directions PLAN.md's M13 section asks for against a real
running unit: opens with every Rev 2/3/7 hook tag offered (does
`OpenDevice()` accept them all without erroring?), queries `S2_DEVICEQUERY`
for `RawMTU` and `S2_GETPEERADDRESS`/`S2_GETDNSADDRESS` (graceful
fallback either way is a PASS, not just success), and runs a real
self-echo round trip through instrumented counting hooks to report
*which* negotiated width the driver actually chose. Deliberately does its
own manual `OpenDevice()` with a 9-tag list rather than reusing
`sana2open.c`'s shared 3-tag helper (`S2_CopyToBuff`/`CopyFromBuff`/
`PacketFilter` only) -- extending that fixed-size array for one tool's
specialized probing wasn't worth complicating SanaDump/SanaSend's own
simpler, stable contract. The self-echo check uses a bounded
`Delay()`+`CheckIO()` poll (~1 second), not a blocking `WaitIO()` --
a crossover-mode (or otherwise non-self-delivering) unit will never
satisfy it, and that's itself a meaningful, gracefully-reported result,
not a probe hang. Verified on-target via `tests/copperline/run-tools.sh`
(bundled AROS; no real-Kickstart-1.3 equivalent, same as SanaDump/
SanaSend -- this is a V36+ tool, a real 1.3 ROM cannot run it at all):
its own stdout is redirected to a file (Copperline's serial capture never
sees a normal AmigaDOS CLI program's stdout, only direct hardware serial
writes) and grep-checked post-run for expected marker lines.

**Real, non-obvious finding from building the on-target test coverage for
this milestone**: adding a 5th on-target sub-test cluster that performs a
*successful* `dos.library` file `Read()` (the new `buffhooks` sub-test's
`unit13.config` fixture) **after** M9's `record` sub-test's own flushed-
and-closed `recorded.pcap` file, in the same Copperline boot session,
reliably corrupted `recorded.pcap`'s data blocks on the host-side ADF --
`xdftool ... type` on it post-run failed with `FSError: Invalid FileData
Block`, even though the guest's own serial output showed every `SUB=`
line and `RESULT=PASS` cleanly, and the disk was nowhere near full (~7%
used). Root-caused by elimination, not by inspecting Copperline's own
source: not a device bug (the exact same buffhooks logic, exercised in
isolation via a temporary debug build, produced entirely correct
in-guest results); not a raw file-count effect (adding a 14th fixture
file that the guest never actually opens -- confirmed via a throwaway
`unit99.config` nothing ever references -- left `recorded.pcap` intact);
not a timeout-truncation effect (raising `BENCH` well past what the
guest actually needs made no difference). The one variable that DID
matter: whether `read_unit_config()` performed a real, successful
`Open()`+`Read()` of a NEW file late in the session, after
`recorded.pcap` was already flushed and closed. This points at a
Copperline/AROS floppy write-back ordering quirk -- likely a cached
track/block buffer for the earlier file getting disturbed by a later,
unrelated file read before the emulator's `--benchmark-until` cutoff
ends the session -- not something this project's own device or test
code controls. **Fixed by reordering, not by touching device code**:
`sana2test.c`'s whole M13 sub-test block now runs *before* M9's `record`
sub-test, restoring the same "the file-write-then-verify test is the
last DOS I/O in the session" property every milestone before M13 already
had by accident. Verified: 3/3 clean `RESULT=PASS` + `RECORD_VERIFY=PASS`
runs on bundled AROS, 2/2 on real Kickstart 1.3, after the reorder.

## `replaycap=` -- configurable replay-record ceiling (M14)

M8 picked `SANA2LOOP_MAX_REPLAY_PACKETS` empirically (32 -> 16 -> 4,
see that section above and M9's own two-simultaneous-buffers finding) as
a single compile-time constant every `mode=replay` unit was stuck with.
M14 makes that a per-unit override: `replaycap=N`, `1..
SANA2LOOP_CONFIG_MAX_REPLAYCAP` (256), validated in `parse_config_line()`
exactly like every other numeric key -- an out-of-range or non-numeric
value fails the whole `OpenDevice()` at pure config validation, before
`load_replay_file()` ever runs. `SANA2LOOP_MAX_REPLAY_PACKETS` (4) now
means only the DEFAULT applied when no `replaycap=` is given; it is no
longer read anywhere inside `load_replay_file()` itself.

**Threading `su_ReplayCap` follows the exact pattern `su_RecordMax`
already established**: `open()` seeds `cfg.replaycap` from the unit's
current (pre-reset) value before `read_unit_config()` runs, then writes
the parsed result back to `unit->su_ReplayCap` before `load_replay_file()`
is called. `reset_unit()` frees the previous lifetime's buffers using
`unit->su_ReplayCap` (the value that was actually active when they were
allocated) BEFORE resetting the field to `SANA2LOOP_MAX_REPLAY_PACKETS`
for the next lifetime -- freeing with the wrong (already-reset) size
would either leak the tail of a larger previous allocation or corrupt
the heap by claiming a smaller one is bigger than it is.

**Runtime multiply verified safe, again.** `cap * sizeof(struct
EchoPacket)` inside `load_replay_file()` is now a genuine runtime
multiplication (`cap` is a per-unit variable, not a compile-time
constant) -- exactly the class of expression M8 already found compiles
to an unavailable `__mulsi3` on this `-nostdlib -m68000` build when the
backend's shift/add synthesis budget is exceeded. Verified safe by
building and checking the output with `nm`/`objdump`: no `__mulsi3` (or
any other undefined multiply-helper symbol) appears anywhere in the
linked binary, and the link itself succeeds -- a real requirement for
`__mulsi3` in this libgcc-less build would be a hard link failure, not a
silent miscompile, so a clean link is conclusive here.

**The positive case (a raised cap actually loading more records) is not
covered by the automated on-target suite** -- only the negative,
allocation-free validation case is (unit 14, `replaycap=0`). Raising the
cap needs a real replay-buffer allocation, and a THIRD simultaneously-
held replay buffer (alongside the pre-existing M8/M9 units 9 and 10,
never freed until next open per this device's own documented design --
see "Memory lifetime" below) reliably exceeds bundled AROS's tight
512K+512K test profile, reproducing even at the historical default cap
of 4 -- proving the constraint is buffer COUNT under bundled AROS, not
the cap value. Confirmed functionally correct instead via real Kickstart
1.3 (debug-instrumented run: a `replaycap=6` capture loads and reads back
all 6 records correctly) and real Kickstart 3.1 (`SanaConform`). See
`docs/PLAN.md`'s M14 risk note for the full investigation, including the
debug-marker methodology (same technique M9's own investigation
established) and the second, independently-failing attempt to relocate
the positive case into `run-tools.sh`'s more generous session.

## `wire=`/`bps=`/`mintu=` -- non-Ethernet wire types and the write-size floor (M15)

All three keys came from reviewing **nullsana** (Aminet
`comm/tcp/nullsana`, 2000 -- the prior-art virtual SANA-II device whose
existence also retired this project's "no null device has ever existed"
claim); its config template (`WIRE/K,MTU/N/K,MINTU/N/K,BPS/N/K,...`) is
the direct ancestor of these knobs' names.

- **`bps=`** is *reported only* (`S2_DEVICEQUERY`'s `BPS` field, was a
  hardcoded 10000000). Deliberately not a throughput shaper -- latency
  belongs to the separate `delay=`/`deviation=` proposal (GitHub #24); a
  reporting knob quietly becoming a shaper would be a trap.
- **`mintu=`** rejects short writes with `S2ERR_BAD_ARGUMENT` --
  deliberately NOT `S2ERR_MTU_EXCEEDED` (that error names the too-LARGE
  failure specifically) and deliberately NOT silent padding (padding
  would mutate the bytes a reader sees, against the byte-exact delivery
  contract). The cross-field `mintu <= mtu` check runs in
  `read_unit_config()` *after* `wire=`'s own mtu-default resolution,
  since the keys may arrive in either order.
- **`wire=slip|cslip|ppp`** narrows the station address to 4 bytes (an
  IP standing in for a hardware address -- nullsana's own convention,
  and the established one for serial-wire SANA-II drivers; SANA-II
  itself gives these wires no addressing at all). Everything
  Ethernet-conceptual declines loudly: `S2_BROADCAST`/`S2_MULTICAST`/
  `S2_ADD/DELMULTICASTADDRESS` and `SANA2IOF_RAW` writes all fail
  `S2ERR_NOT_SUPPORTED` (same shape as replay mode's own write
  rejection), `RawMTU` reports 0, and `fill_read_from_packet()` never
  sets `SANA2IOF_BCAST/MCAST` (both are 802.3 addressing conventions).
  `dest_matches_unit()` compares exactly `su_AddrSize` bytes and skips
  the broadcast/group checks entirely off-Ethernet.
- **Wire defaults vs explicit keys**: a non-Ethernet `wire=` brings its
  own mtu/bps defaults (1006/19200; ppp 115200), but an explicit
  `mtu=`/`bps=` line wins regardless of key order -- implemented as
  `mtu_explicit`/`bps_explicit` parse-state flags resolved in
  `read_unit_config()`, not parse-order overwriting.
- **Crossover across mismatched wire types fails `S2ERR_BAD_STATE` at
  write time** -- the alternative was `dest_matches_unit()` silently
  never matching a differently-sized peer address, a blackhole nobody
  configured on purpose (unlike `oversilent`'s). Write-time, not
  open-time, because the peer's wire type isn't knowable at this unit's
  open: config is only read at each unit's OWN 0->1 open, so the peer
  may still carry the Ethernet default until its own first open.
- **Replay/record are Ethernet-only** (rejected at config validation):
  `load_replay_file()` checks `LINKTYPE_ETHERNET` and parses 14-byte
  headers; `record_append()` synthesizes dst[6]+src[6]+type[2] frames.
  A slip-wire capture would lie about its own linktype.
- The 4-byte factory address is `0A:53:32:<unit>` (10.83.50.<unit> --
  same recognisable "S2" middle bytes as the Ethernet factory address,
  led by a private-range IP octet instead of the locally-administered
  MAC bit). Rebuilt in `open()` at config-apply time, NOT in
  `reset_unit()` -- the wire type isn't known until the config is read,
  which happens after reset.

## `addr=`/`errors=`/`loss=`, and our own `__mulsi3` (M16)

The second nullsana-inspired batch (GitHub #27/#29):

- **`addr=`** pins the **factory** address (current starts equal to it;
  `S2_CONFIGINTERFACE`'s M3 one-shot override works on top unchanged) --
  the less surprising of the two semantics the issue weighed. Parsed as
  colon-separated hex bytes (1-2 digits per component, nullsana's own
  single-digit style accepted); byte count must equal the wire's address
  width, validated in `read_unit_config()` AFTER `wire=` resolution
  (either key may come first); an Ethernet address with the group bit is
  rejected (a station address with the multicast bit set isn't one).
  Applied in `open()` after the wire block (which would otherwise
  clobber it for point-to-point units), via CopyMem (the
  variable-length-copy-becomes-memcpy hazard).
- **`errors=N`** is the only *visible* fault knob: 1-in-N writes fail
  `S2ERR_TX_FAILURE`/`S2WERR_TOO_MANY_RETRIES` (the classic
  "transmitter gave up" wire error) and fire `S2EVENT_TX|S2EVENT_ERROR`
  through the existing `fire_event()` -- so queued `S2_ONEVENT`
  consumers finally have something that exercises the momentary event
  bits, not just ONLINE/OFFLINE. Checked before the packet buffer is
  allocated (a failed transmit has nothing to deliver or free);
  deliberately does NOT count PacketsSent or touch the
  offlineafter/onlineafter counters (different failure class).
- **`loss=N`** sits immediately after `droppct` with identical
  observable semantics (write succeeds, packet vanishes,
  `note_write_success()`), just 1-in-N instead of percent. Both compose.
- **1-in-N draw without division**: hit iff `roll * n < 65536` where
  roll is a 16-bit draw -- `prng_hits_one_in()`, mirroring
  `prng_hits_pct()`'s exact-boundary discipline (0 = disabled with NO
  draw, so enabling one knob never perturbs another's seeded sequence;
  1 = always, also draw-free). `n` capped at 65535.
- **`__mulsi3` is now provided by the device itself** -- the M16 struct
  growth (two ULONGs per unit) pushed `sizeof(struct Sana2LoopUnit)`
  past GCC's shift/add synthesis budget at PRE-EXISTING `sld_Units[]`
  index sites, i.e. the long-standing avoidance strategy (M8's
  noinline, M14's nm-verification) stopped scaling: careful new code
  could no longer guarantee a linkable build. The definition (three
  native mulu.w partial products via inline asm -- C `*` inside the
  function that implements `*` risks a recursive libcall, and GCC 6.5
  was observed NOT folding `(UWORD)a*(UWORD)b` to mulu.w anyway) lives
  textually after `_start()`/romtag ON PURPOSE: as the file's only other
  extern function it otherwise gets emitted at text offset 0
  (observed), displacing the moveq #-1 safety return -- the same
  front-of-hunk hazard `sana2test.c` documents for pooled string
  literals. The whole future-milestone hazard class ("did this struct
  edit break the link?") is retired.

## `delay=`/`deviation=` -- simulated transit latency and jitter (M17)

The third and final nullsana-inspired batch item (GitHub #24), completing
the config knobs first sketched when nullsana (comm/tcp/nullsana, 2000)
was reviewed as prior art -- its own config template carried
`DELAY/N/K,DEV=DEVIATION/N/K` under the identical names.

- **Queue shape, deliberately NOT `su_ReorderBuf`'s fixed-array
  pattern**: `su_DelayQueue` is a plain `struct List`, packets linked via
  their own existing `ep_Node` (no wrapper allocation, unlike a naive
  "queue node wraps a packet pointer" design). Reorder's fixed 8-slot
  array works because a bounded WINDOW is the whole point of reordering;
  delay has no such natural bound -- an arbitrarily large number of
  packets can be genuinely in flight at once under a long `delay=` and a
  fast write rate. Left unbounded on purpose, backed by the same
  `AllocMem()` failure path (`S2ERR_NO_RESOURCES`) every other packet
  allocation in this file already has -- no NEW resource-exhaustion
  surface, just the existing one applying here too.
- **Not due-tick sorted**: jitter means a packet enqueued LATER can have
  an EARLIER due tick than one already waiting (that's the mechanism by
  which `deviation` reorders delivery on its own, independent of the
  `reorder=` knob) -- so `delay_queue_tick()` scans the whole list every
  VBlank rather than assuming a sorted prefix, the same shape
  `vblank_server()`'s existing scheduled-replay branch does NOT need
  (that one IS strictly ordered, by construction of the loaded pcap).
- **Pipeline position: LAST, after reorder.** `deliver_or_reorder()`'s
  non-reorder branch and `reorder_push()`'s own natural-eviction branch
  both now call `deliver_with_delay()` instead of `deliver_packet()`
  directly -- a packet's simulated wire latency is whatever's left once
  every other fault decision (errors/droppct/loss/truncate/duppct/
  reorder) already ran. `reorder_flush()` and `delay_queue_flush()`
  themselves deliberately do NOT route through `deliver_with_delay()` --
  see the next point.
- **Flush bypasses delay entirely, on purpose.** `CMD_FLUSH`,
  `S2_OFFLINE` (manual or auto via `offlineafter`), and last-close all
  call `delay_queue_flush()` (mirroring `reorder_flush()`'s own
  placement in all three call sites exactly), which drains the WHOLE
  queue immediately via a direct `deliver_packet()` call -- never
  `deliver_with_delay()`. Re-queuing an already-flushed packet into a
  fresh wait would defeat the entire point of "flush" (push everything
  out NOW). This also matters for correctness, not just intent:
  `vblank_server()` only advances `su_Tick` while a unit is
  `su_Online` (see that field's own comment), so a unit going offline
  with anything still parked in `su_DelayQueue` would otherwise strand
  it forever -- no future tick would ever come to release it. Same
  underlying reasoning `reorder_flush()`'s own doc comment already gives
  for why IT exists; delay's version of the hazard is just more acute
  (reorder's buffer is bounded to 8 slots and stops accepting once full;
  delay's queue has no such natural ceiling to eventually force a
  flush by itself).
- **Jitter draw, and why it's NOT `%`:** `compute_delay_ticks()` draws
  uniformly in `[0, window)` via `(roll16 * window) >> 16` -- the exact
  top-bits-of-a-16x16-multiply technique `prng_hits_pct()`/
  `prng_hits_one_in()` already established, not a general modulo. M16
  added this build's own `__mulsi3` (32x32->32 multiply), but NOT
  `__umodsi3` (division/modulo by a runtime divisor) -- that gap is
  still open, so a `% window` here would still fail to link. `window =
  2*deviation+1`, capped by `SANA2LOOP_CONFIG_MAX_DELAY` (3000) well
  under the 65535 a UWORD operand needs.
- **Proving "genuinely deferred" on-target needed a technique the
  existing `scheduled` sub-test never had to use.** `scheduled` only
  ever proves EVENTUAL correct delivery via `reap_interrupt_io()` (a
  side-effect wait, sidestepping the documented bundled-AROS
  `CheckIO()`/`WaitIO()` false-completion bug from the M9 investigation
  -- see that risk note). That alone can't distinguish "delivered later,
  via the delay queue" from "delivered synchronously, and
  `reap_interrupt_io()`'s spin just happened to already see the
  evidence" -- a bug where `delay=` silently did nothing would pass a
  `scheduled`-shaped test undetected. The `delay` sub-test adds a SECOND
  check the `scheduled` test doesn't need: immediately after the
  write's own `do_io()` returns (which is unconditionally synchronous --
  only the READ's delivery is ever deferred, never the WRITE's own
  completion), a plain read of `g_HookToCount` (an ordinary side-effect
  global, not an IORequest completion check, so none of the
  CheckIO()/WaitIO() bug applies) must show it hasn't incremented yet.
  Only then does `reap_interrupt_io()` prove the eventual, correct
  arrival -- exactly like `scheduled` does.

## Multi-open fan-out, real `S2_PacketFilter` dispatch, `SANA2OPF_MINE` (M18)

The last item in the nullsana-inspired queue (GitHub #25), and the only
one that changed core delivery semantics rather than adding an
orthogonal config knob -- scoped in depth via two investigation comments
posted to the issue *before* any code moved (a design fork had to be
settled first), then implemented per that plan.

- **The design fork, resolved**: does typed-vs-orphan precedence stay
  GLOBAL (across the whole unit) or become fully PER-OPEN once fan-out
  exists? Resolved by going back to what `S2_READORPHAN` actually IS per
  the SANA-II spec -- the unclaimed-TYPE catch-all, not a general
  sniffing tool. So: typed reads fan out per-open (every distinct open
  with a matching queued read gets a fill), but orphan reads stay a
  GLOBAL, single-delivery fallback consulted only when NO typed reader
  anywhere claimed the packet. This is why the pre-existing `delivery`
  sub-test's own assertion (a typed read on one open beats an orphan
  read on a SEPARATE open, and the orphan stays genuinely untouched)
  passed **unchanged** -- confirmation the fork was resolved correctly,
  not a lucky coincidence. Fanning orphan out too would have broken that
  test's own definition of correct behavior, which is exactly the
  collision the first investigation comment on #25 flagged before
  implementation started.
- **One candidate per open, not per queued read**: an open may have
  several typed reads of the same type pipelined (the standard
  "queue N reads to catch N *consecutive* packets" pattern this test
  suite itself already teaches) -- one packet must satisfy at most ONE
  of them, not drain the whole pipeline. `open_already_collected()`
  dedups by comparing each candidate's `ios2_BufferManagement` (the
  `Sana2LoopOpen*` cookie) against everything already collected for this
  packet, during the single forward walk over `su_PendingReads`.
- **`deliver_packet()`'s rewrite reuses an EXISTING pattern, not a new
  one**: collect every matching candidate onto a PRIVATE local `List`
  under ONE `Disable()` (exactly `abort_requests_for_open()`/
  `fire_event()`'s own established "pop under Disable, act outside"
  discipline -- both of those functions' own comments explain WHY
  dropping Disable() mid-walk and resuming from a stale `next` is
  dangerous under concurrent list mutation, and that reasoning applies
  identically here), THEN filter+deliver each one outside `Disable()`
  (consumer hooks and `ReplyMsg()` are never legal disabled). A
  filter-rejected candidate requeues via `AddHead()` rather than being
  treated as a failure -- it's still validly waiting for a FUTURE
  packet, just not this one.
- **The orphan fallback ALSO needs a proper multi-candidate walk, not
  just single-delivery**: if the first collected orphan candidate's
  filter rejects, the search must try the NEXT orphan candidate (a
  rejection means "not for me", not "no eligible orphan exists") --
  still stopping at the first ACCEPT, preserving single-delivery
  overall. Untried remainder candidates (the search stopped early once
  one accepted) are put back exactly as untouched as a
  never-considered candidate should be.
- **Stats are per wire-packet, not per-copy**: `PacketsReceived`/
  `S2_TRACKTYPE` counts increment ONCE per packet arrival regardless of
  how many opens received a fill -- matching what these numbers mean on
  a real driver (packets that reached the interface), not a
  per-listener multiplier. `UnknownTypesReceived` still means "nobody --
  typed, orphan, or rxqueue -- wanted it".
- **`S2_PacketFilter`'s calling convention, objdump-verified**: sourced
  from wiki.amigaos.net's Autodocs (this toolchain's own
  `<devices/sana2.h>` has no signature for it, the same class of gap
  M13 already found for the Rev 2/3/7 tag VALUES) --
  `keep = PacketFilter(hook, ios2, data)` / `d0 = a0(hook) a2(ios2)
  a1(data)`. This is the file's THIRD distinct hook register convention
  (after `Sana2CopyFunc`'s sequential `a0/a1/d0` and the VBlank
  interrupt server's `a1`-plus-Z-flag convention), and the register
  ORDER doesn't match C parameter order -- a documented quirk of the
  standard Amiga Hook calling convention, not a typo, and exactly the
  kind of subtlety this project doesn't take on faith: disassembling
  the compiled `try_deliver_candidate()` confirmed the compiler loads
  `a0`=hook, `a2`=ios2 (via `a3`, the stashed original `io` argument),
  `a1`=data (`pkt->ep_Data`, computed once as a small-constant offset
  add) immediately before the `jsr`, with the jump target itself staged
  through `a6` (a GCC codegen convenience register, not part of the
  hook's own ABI -- same pattern the pre-existing copy-hook calls
  already used). Same M1 discipline, extended to a third convention.
- **Filter dispatched from real VBlank interrupt context inherits the
  same M9-established legality** as `S2_CopyToBuff` already has there
  (scheduled replay, and M17's own delay-queue release both call
  `deliver_packet()` from `vblank_server()`) -- not a new question, the
  identical contract just reached by one more caller-supplied hook type.
- **The AbortIO-during-filter-transit race, investigated and
  deliberately left to the EXISTING contract**: a candidate is briefly
  off every unit-shared list while its filter hook runs (which can take
  arbitrarily long -- an external caller's code, unlike the earlier
  Remove()-then-immediately-ReplyMsg() window this file already
  accepted). A concurrent `AbortIO()` landing exactly there finds
  nothing on any list and returns -1 ("already in-flight" --
  `abort_io()`'s own PRE-EXISTING, unmodified contract, quoting Exec's
  own "an operation already in progress may complete before it can be
  aborted"). Two candidate fixes were considered and rejected: an
  `io_Flags` bit (unsafe -- `SANA2IOF_*` is spec-owned, and
  `SendIO()`/`DoIO()` already clear `io_Flags` wholesale before every
  dispatch per M4's own finding, making it an unreliable place to stash
  device-private transient state) and a fourth `abort_requests_for_open()`-
  searched transit list (real complexity for a window this device's own
  delivery model -- synchronous, task-context, never deferred past the
  write's own `do_io()` -- makes unreachable from any test following
  this file's existing write-then-check sequencing). Net: no new
  tracking machinery; the pre-existing "-1, let WaitIO see it
  naturally" contract already covers the widened window correctly.
- **`SANA2OPF_MINE`**: new `su_Exclusive` BOOL, checked in `open()`
  against the unit's CURRENT state (`unit_OpenCnt`/`su_Exclusive`)
  BEFORE `dev->lib_OpenCnt++` -- a busy-rejected open never became a
  real open, so (unlike every later failure path in `open()`) there's
  no matching decrement needed. `IOERR_UNITBUSY` (Exec's own "device
  opens ok, but requested unit is busy"), not an `S2ERR_*` -- this
  rejection happens before SANA-II's own error space is reachable at
  all.
- **`run.sh`'s real-1.3 `BENCH` default raised 30 -> 45**: the new
  sub-tests' extra `OpenDevice()`/`CloseDevice()` cycles (two of which,
  in `mine`, trigger a real `ENV:`/`S:` config-file read via unit 13's
  existing `buffhooks=32` fixture) pushed real Kickstart 1.3's own total
  session time past its previous budget -- found as a guest-side clean
  `END`/`RESULT=PASS` but a 0-byte `recorded.pcap` on host-side
  extraction (the LAST DOS I/O's own write-back not yet flushed when
  `--benchmark-until` tore the session down), confirmed via BENCH=45
  passing twice where BENCH=30 failed twice. Same class of fix M9's own
  KICK= default increase (10 -> 30) already needed, for the identical
  underlying reason (real 1.3's dos.library is measurably slower than
  AROS's).

## Companion tools become general-purpose SANA-II utilities (M19)

Prompted by reviewing sanamon (Aminet `comm/net/sanamon`, Michael van
Elst, 1995) -- a general SANA-II packet monitor whose whole design
assumes it can target *any* driver. Reviewing it against sana2loop's own
four companion tools (which, until this milestone, ALL hardcoded
`"loopback.device"` as a string literal) surfaced the gap directly.

- **`DEVICE=`** everywhere, default unchanged. `sana2_open()` (the
  shared helper `SanaDump`/`SanaSend` already use) already took a device
  name parameter -- only the *callers* were hardcoding it.
- **`CONFIG` matches sanamon's own documented behavior exactly**: "Try
  to configure interface with its default address." Implemented as
  `S2_GETSTATIONADDRESS` (reads the driver's own factory default from
  `ios2_DstAddr`) then `S2_CONFIGINTERFACE` with that same address --
  not an arbitrary synthesized one. `S2WERR_IS_CONFIGURED` (a stack beat
  us to it) is reported, not fatal, matching `S2_CONFIGINTERFACE`'s own
  one-shot-per-lifetime contract.
- **`ONLINE`'s default flip is the one genuine behavior CHANGE, not just
  an addition.** Pre-M19, `SanaDump`/`SanaSend`/`SanaConform` all forced
  `S2_ONLINE` unconditionally at startup, never `S2_OFFLINE` at exit.
  Harmless against sana2loop's own device (a test tool, no real-world
  consequence). A genuine risk once `DEVICE=` means these can target a
  driver a stack is actively managing -- forcing a unit online (or,
  worse, taking it back offline on exit) out from under a stack that
  deliberately put it in a specific state is real interference a
  monitoring/testing tool shouldn't cause by default. sanamon's own
  `ONLINE` flag is opt-in for exactly this reason ("tell driver to go
  online first and to go offline when sanamon is exiting" -- i.e. own
  the WHOLE lifecycle, not just flip a bit). Matched that: omitted
  (default) means "assume a stack already manages this, don't touch
  it"; `ONLINE` given means own start-to-finish.
- **A real, unplanned multi-consumer correctness bug found wiring the
  new default into `tests/copperline/tools-startup-sequence.txt`**:
  `SanaDump` and `SanaSend` share unit 12 in that test (`SanaDump` opens
  first and stays open across `SanaSend`'s later write -- the M11-era
  design). Giving BOTH tools their own `ONLINE` flag would have been
  wrong: `SanaSend`'s own `ONLINE` would take unit 12 back OFFLINE at
  ITS OWN exit, potentially before `SanaDump` (still running, still
  expecting the unit to stay online) is done with it. Resolved by giving
  `ONLINE` only to `SanaDump` (genuinely first to use the unit, and the
  one that keeps it open longest) -- `SanaSend` needs no flag at all,
  since the unit is already online by the time it runs. Documented in
  `tools-startup-sequence.txt`'s own comment as general guidance for
  anyone driving two of these tools against the same unit.
- **`SanaConform`'s new `S2_PacketFilter` probe** reuses its existing
  instrumented-counting-hook pattern (same shape as the Rev 2/3 buffer-
  hook probes already there) -- `hook_filter()` uses the identical
  `a0`(hook)/`a2`(ios2)/`a1`(data) register convention M18's own
  `Sana2FilterFunc` established and objdump-verified; adding it to
  SanaConform is the independent, harness-side confirmation that a
  genuine caller-supplied hook (not just the device's own internal one)
  receives exactly what M18 claimed. Placed in the SAME `tags[]` array
  the self-echo round trip already uses, so one round trip proves buffer
  hook AND filter invocation together -- no separate probe needed.

### The DUMP/LEN on-target verification investigation (two real findings, one red herring)

Trying to get real on-target coverage for `SanaDump`'s new `DUMP`/`LEN`
console output surfaced a genuine environment limitation, reached via a
false lead worth recording so it isn't re-chased:

1. **First attempt**: redirect the backgrounded `Run`'s own stdout to a
   real file (`Run >S:sana2loop/dump.out SanaDump ... DUMP LEN=8`) and
   grep the extracted file for `dump_packet()`'s own expected text,
   exactly the pattern already established for `SanaConform`/`SanaInfo`'s
   own redirected-output checks. This broke the WHOLE session on bundled
   AROS specifically: `SanaSend`'s write never got through, and
   `SanaConform`/`SanaInfo`'s own later steps never even ran (their
   output files never got created at all) -- not a clean failure, a
   stall.
2. **Red herring chased first**: bisecting by removing pieces
   (`LEN=8` -> `LEN=16` -> renaming the keyword to `DUMPLEN` entirely)
   showed the hang tracked with having a THIRD `/K/N` (numeric keyword)
   field in `SanaDump`'s `ReadArgs()` template (`UNIT/A/N` + `COUNT/K/N`
   + a new one), regardless of that field's name or value -- strongly
   suggesting a bundled-AROS-specific `ReadArgs()`/dos.library limit on
   simultaneous numeric-keyword fields. Tested against real Kickstart
   3.1 to check: the SAME 3-numeric-field combination worked FINE there
   (main pipeline succeeded) -- confirming this half was a genuine
   bundled-AROS-only quirk, in the same class as M9's `CheckIO()`/
   `WaitIO()` bug and M13's floppy write-back ordering issue (AROS
   diverging from real hardware, not a device/tool bug).
3. **But it stopped mattering**: inspecting `dump.out`'s own content on
   the run where the pipeline DID succeed (real Kickstart 3.1, 3
   numeric fields, no hang) showed it contained only 8 bytes:
   `"[CLI 2]"` -- AmigaDOS's own new-process banner text, NOT any of
   `dump_packet()`'s own `printf()` output. Confirmed the SAME result on
   bundled AROS once the 3-numeric-field hang was avoided (2 fields,
   `DUMP` alone, no `LEN=`). **A backgrounded `Run`'s stdout redirected
   to a real file, in this Copperline+dos.library combination, simply
   does not deliver a child clib2 program's own stdout content to that
   file at all** -- only the CLI-creation banner reaches it, on BOTH
   bundled AROS and real Kickstart 3.1. A genuine environment/toolchain
   limitation of this specific mechanism (backgrounded process + file
   redirect + clib2 stdio), not a device or tool bug, and orthogonal to
   the numeric-field finding above (which only ever explained *whether
   the session hangs*, never *whether the redirect captures anything*).
4. **Resolution**: `DUMP LEN=16` stays exercised in the shared
   round-trip session (`tools-startup-sequence.txt`), proving the new
   code path runs without error or hang, via the ORIGINAL working
   `Run >NIL:` redirect (not a real file) -- consistent with this
   project's own established stance (M14's own positive-case resolution
   is the closest precedent): verify what's mechanically checkable
   on-target, and be explicit in the docs/comments about what isn't,
   rather than either skipping coverage silently or forcing a workaround
   that doesn't actually prove anything. `dump_packet()`'s own printed
   TEXT isn't independently checked on-target; its correctness rests on
   code review plus the fact that it formats the SAME `io` fields
   `verify_sanadump.py` already proves correct via `captured.pcap` (a
   narrower, lower-risk unverified surface: formatting only, not data
   correctness).

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
   Hook *`; recorded at M1, genuinely dispatched per candidate delivery
   since M18 -- see this file's own M18 section for the calling
   convention and fan-out semantics). `NULL` means "no tags" -- install
   defaults.
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

**M13 update**: the `S2_CopyTo/FromBuff16/32` (Rev 2/3) and
`S2_DMACopyTo/FromBuff32/64` (Rev 7) variants are no longer unimplemented
-- see the dedicated "SANA-II Rev 2/3/4/7 additions (M13)" section below
for the full story (negotiation, actual invocation, and why the DMA hooks
are recorded but deliberately never called).

Genuinely later-phase, still not implemented: **invoking the base hooks
from interrupt context** as an opt-in fault knob (SANA-II requires hooks
to be interrupt-safe; testing that a consumer's hooks actually are is
real value, distinct from M9's `vblank_server()` already doing exactly
this for scheduled-replay/record delivery -- this would be a *configurable*
knob making EVERY delivery interrupt-driven, not just those two modes).

## Memory lifetime: no low-memory participation, fail-fast allocation, and the expunge()-leak fix

Every `AllocMem()` in this device (`su_ReplayPackets`/`su_ReplayDueTicks`/
`su_ReplayDeliveryCopies`, `su_RecordBuf`, the per-open `Sana2LoopOpen`
cookie, echo/duplicate `EchoPacket` nodes) passes only `MEMF_CLEAR` -- no
`MEMF_CHIP`/`MEMF_FAST`. Per the RKRM (Exec Memory Allocation): an
allocation with neither flag set is satisfied from the first memory
available on the system's lists, which is **fast RAM if any is fitted,
chip only as the fallback**. This device has no chip-memory-specific
requirement of its own; test runs that reported chip-RAM exhaustion
(M11's on-target findings, `docs/PLAN.md`) were a property of the
deliberately minimal `machine.toml` profile in effect at the time (no
`[memory] fast` at all, matching the device's then-1MB target floor --
raised to 2MB by the pre-launch-review pass, see `machine.toml`'s own
comment), not a device preference for chip memory.

The device also never calls `AddMemHandler()` -- it does not register
itself with Exec's low-memory recovery protocol (the mechanism that lets
a task be asked to free something when some *other* allocation is about
to fail). It is invisible to system memory pressure right up until one of
its own `AllocMem()` calls fails, at which point failure is handled
locally and immediately (`S2ERR_NO_RESOURCES`, or failing the whole
`OpenDevice()`) -- no retry, no attempt to reclaim anything of its own to
help the request along.

Replay/record buffers are freed only at a unit's own next 0->1 open
transition (`reset_unit()`), never when the last opener closes it --
established at M8/M9, real cost confirmed at M11 (see `docs/PLAN.md`).
**A real gap this left, found by code review while documenting the above
(not by on-target testing): `expunge()` -- called by Exec to unload the
whole device once its device-wide `lib_OpenCnt` reaches 0, e.g. via the
Shell's `Avail FLUSH` -- froze only the device base struct itself and
never looped over `sld_Units[]` first.** Any unit that had used
`mode=replay`/`record=` and was closed but never reopened before the
whole device got expunged leaked those buffers permanently (once the
base is freed, nothing points at them again) -- strictly worse than the
documented "held until next open" cost. Fixed by looping over every unit
and calling `reset_unit()` on each before the base's own `FreeMem()`,
reusing the existing free logic instead of duplicating it (harmless that
it also resets other unit fields on a struct about to be freed anyway).
See [Replay and Record](../userdocs/Replay-and-Record.md#memory-usage-and-freeing-it-manually)
for the user-facing manual-free recipe this all motivates.

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
