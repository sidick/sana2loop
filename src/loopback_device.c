/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Simon Dick
 */

/* loopback_device.c -- sana2loop's loopback.device.
 *
 * A hardware-free SANA-II device. Every unit is, at any time, in exactly
 * one of three modes (config-only concept, `mode=` -- see
 * read_unit_config()): standalone **echo** (a write comes straight back to
 * a reader on the same unit, addresses swapped -- unit 0's default and
 * every otherwise-unconfigured unit's), **crossover** (paired with exactly
 * one other unit via `peer=`, real unswapped addressing and standard
 * 802.3-style receive filtering -- units 2 and 3 by default), or **replay**
 * (plays back a loaded `.pcap` capture -- see pcap.h -- either on-demand,
 * `replaymode=consume`, or on its own captured real-time-relative
 * schedule via a VBlank interrupt server, `replaymode=scheduled`; writes to
 * a replay unit fail S2ERR_NOT_SUPPORTED). Independent of mode, `record=`
 * taps the same delivery path to capture whatever a unit actually receives
 * back out to its own `.pcap` file. A configurable fault-injection
 * pipeline (do_write(): MTU/PMTUD-black-hole, drop, truncate, duplicate,
 * reorder, plus config-driven forced online/offline flapping) sits between
 * every write and delivery, off by default. See userdocs/ (built as a
 * MkDocs site, `make guide` for an on-Amiga AmigaGuide version) for the
 * full user-facing reference to all of this; docs/PLAN.md for the
 * milestone-by-milestone design history and reasoning behind each piece;
 * docs/sana2-notes.md for the SANA-II protocol details this header
 * doesn't spell out.
 *
 * One delivery path (deliver_packet()) is shared by echo, crossover, AND
 * replay -- real SANA-II precedence, strictly: a queued **typed**
 * CMD_READ first, else a queued S2_READORPHAN, else (config `rxqueue`,
 * defaults to 0 -- strict drop, no buffering, matching "real drivers hold
 * no internal buffers") a bounded queue, else dropped with
 * Sana2DeviceStats.UnknownTypesReceived counted.
 *
 * No separate unit task anywhere in this file: echo/crossover/reorder-
 * flush all complete synchronously inside BeginIO on the calling task
 * (ReplyMsg from there is perfectly legal -- it just posts to whatever
 * port the reader's IORequest names); scheduled-replay/record instead
 * complete from a real VBlank hardware interrupt (vblank_server()) via the
 * SAME deliver_packet() precedence, deliberately not a parallel mechanism.
 * Both the pending-reads/orphan-reads lists and the pending-echoed-data
 * list are wrapped in Disable()/Enable() critical sections throughout,
 * since multiple tasks (and, for the VBlank case, real interrupt context)
 * can touch the same unit's lists concurrently.
 *
 * Freestanding: hand-rolled Resident/auto-init, NOT libnix's devinit.o.
 * Reason: echo mode needs state SHARED across every independent OpenDevice()
 * of a unit (two processes opening unit 0 must see each other's packets),
 * but libnix's devinit.o hands back a fixed-size opaque device base with no
 * room for that -- and its -fbaserel/A4 instance-data model is a known
 * hazard for stray globals (see the sibling narrator.wyoming project, which
 * hit a real on-target Guru #8100 0005 from a device global under that
 * framework). Hand-rolling the romtag/auto-init table (as bebbo's own
 * toolchain examples do -- see docs/sana2-notes.md) lets us size the device
 * base ourselves, so the per-unit state lives in *that* struct rather than a
 * file-scope global, and is reached explicitly via the `dev` (a6) parameter
 * every hook already receives. No -fbaserel, so ordinary globals (SysBase)
 * are fine here -- this build has no A4 small-data model to violate.
 *
 * 68000 / Kickstart 1.3 baseline throughout: no utility.library, no
 * ReadArgs, no dos.library from the unit's perspective. The one exception
 * is the config-file read (read_unit_config()) and, downstream of it,
 * load_replay_file()/the record-flush at last close -- all three touch
 * dos.library, and all three do so only from a real Process opener,
 * never from a bare Task, preserving the guarantee for everyone else.
 */

#include <exec/types.h>
#include <exec/nodes.h>
#include <exec/lists.h>
#include <exec/io.h>
#include <exec/errors.h>
#include <exec/resident.h>
#include <exec/devices.h>
#include <exec/memory.h>
#include <exec/tasks.h>
#include <devices/sana2.h>
#include <utility/tagitem.h>
#include <utility/hooks.h> /* M18: struct Hook (h_Entry) for S2_PacketFilter
                               dispatch -- header-only, no utility.library
                               call anywhere in this file; the KS1.3
                               constraint is unaffected. */
#include <exec/interrupts.h>
#include <hardware/intbits.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <dos/dosextens.h>
#include "pcap.h"

/* Old-style dos.library file access mode (Open()'s accessMode) --
 * MODE_OLDFILE=1005 is a stable, long-documented Amiga constant; hand
 * defined rather than pulling in the whole of <dos/dos.h> for one value,
 * matching this file's existing minimal-include-set philosophy. */
#define MODE_OLDFILE 1005

/* M13: SANA-II Rev 4/7 additions this toolchain's <devices/sana2.h> doesn't
 * have at all -- unlike MODE_OLDFILE above (a real header just not
 * included), Hyperion never shipped an updated devices/sana2.h past the
 * original Commodore Rev 2/3 spec (confirmed: the bundled header already
 * has S2_CopyToBuff16/32 and S2_DMACopyToBuff32/FromBuff32 at S2_Dummy+4..
 * +9, and S2_ADDMULTICASTADDRESSES/S2_DELMULTICASTADDRESSES at 0xc000/
 * 0xc001 -- but nothing past that). SANA-II Rev 4-7 live only as informal
 * documentation on wiki.amigaos.net (see docs/PLAN.md's M13 section) --
 * these commands were never in any vendor NDK release.
 *
 * S2_GETPEERADDRESS/S2_GETDNSADDRESS values (0xC002/0xC003) are quoted
 * directly from wiki.amigaos.net's "Revision 4" page and independently
 * corroborated by continuing the exact 0xc000/0xc001 NSD-style sequence
 * the bundled header already has for S2_ADD/DELMULTICASTADDRESSES -- high
 * confidence. S2_DMACopyToBuff64/S2_DMACopyFromBuff64 have NO published
 * numeric constant anywhere found (Rev 7's own doc names the tags but
 * never gives hex values) -- S2_Dummy+10/+11 here is this project's own
 * inferred continuation of the existing +4..+9 sequence, not a confirmed
 * spec value. Harmless if wrong: this device only ever RECORDS a DMA hook
 * pointer, never invokes one (see apply_buffer_mgmt_tags()) -- a real
 * caller using the true value we guessed wrong just means we silently
 * don't recognize that particular tag, functionally identical to
 * correctly declining it, since every DMA hook is declined regardless. */
#define S2_GETPEERADDRESS    0xC002
#define S2_GETDNSADDRESS     0xC003
#define S2_DMACopyToBuff64   (S2_Dummy + 10)
#define S2_DMACopyFromBuff64 (S2_Dummy + 11)

/* Rev 4's RawMTU field extends struct Sana2DeviceQuery past what this
 * toolchain's header defines (see above) -- same layout as the real
 * struct (confirmed identical field order/types against both the bundled
 * AROS-derived header and a real 1994 Commodore devices/sana2.h) with
 * RawMTU appended, per wiki.amigaos.net's "Revision 4" page. Binary
 * layout is what matters for real interop, not field spelling, so this
 * reuses the bundled header's own field names/types rather than the
 * wiki's slightly different ones (LONG vs ULONG is immaterial on m68k). */
struct Sana2LoopDeviceQuery {
    ULONG SizeAvailable;
    ULONG SizeSupplied;
    ULONG DevQueryFormat;
    ULONG DeviceLevel;
    UWORD AddrFieldSize;
    ULONG MTU;
    ULONG BPS;
    ULONG HardwareType;
    ULONG RawMTU;   /* Rev 4 -- new field */
};

#define STR(s)  #s
#define XSTR(s) STR(s)

#define DEVICE_NAME       "loopback.device"
/* dd.mm.yyyy, not a month name -- the AmigaDOS Version command's "$VER:"
 * cookie parses this field as three decimal numbers (day/month/year), so
 * a textual month wouldn't parse as a date at all (see DEVICE_ID_STRING's
 * own comment). */
#define DEVICE_DATE       "(30.07.2026)"
#ifndef DEVICE_VERSION
#define DEVICE_VERSION 1
#endif
#ifndef DEVICE_REVISION
#define DEVICE_REVISION 0
#endif
/* Doubles as both lib_IdString (Exec's own "what is this module"
 * convention -- see init_device()) AND the standard AmigaDOS "$VER:"
 * version cookie the Shell's Version command scans a loaded module's
 * segments for (RKRM: DOS, "The Version Cookie") -- one string serving
 * both, the idiomatic pattern most shipped libraries/devices use, rather
 * than a second copy of the same information. */
#define DEVICE_ID_STRING  "$VER: sana2loop " XSTR(DEVICE_VERSION) "." XSTR(DEVICE_REVISION) " " DEVICE_DATE
#define DEVICE_PRIORITY   0

/* Unit 0 is the Phase-1 echo default (per the proposal's mode-selection
 * plan). Units 2-3 default to a crossover pair (M6); unit 1 stays echo by
 * default too, both so a plain "no config at all" boot still has more
 * than one echo-mode unit available and to leave the M6 crossover default
 * (2<->3) as the sole example of it, rather than making every remaining
 * unit crossover-paired. Units 4 and up are otherwise-uncommitted,
 * echo-default units that on-target tests claim one at a time via their
 * own config fixture, specifically so no two tests' expectations ever
 * collide on the same unit (see tests/copperline/run.sh's per-unit
 * config fixture layout for the current allocation: 1=M5 mtu override
 * (also, as of the pre-launch review pass, truncate=/rxqueue= -- inert
 * for M5's own DEVICEQUERY-only check, picked up fresh by later,
 * separate reopens instead; see unit1.config's own comment for why this
 * reuses unit 1 rather than adding new units at all),
 * 2/3=M6 crossover pair (also M7's oversilent/PMTUD pair, since neither
 * milestone's own assertions care about the other's config additions
 * there; M13 also reuses this pair, read-only, for its S2_GETPEERADDRESS
 * success-path check), 4=M5 malformed-config-fails, 5=M7 droppct,
 * 6=M7 duppct, 7=M7 reorder, 8=M7 offlineafter/onlineafter,
 * 9=M8 consume-mode replay (M13 also reuses this, read-only, for its
 * RawMTU=0-on-replay check), 10=M9 scheduled replay, 11=M9 RAM record
 * (also, as of the pre-launch review pass, a smaller recordmax=150
 * exercising S2_GETSPECIALSTATS' overflow-count record too -- see
 * unit11.config's own comment),
 * 12=M11 SanaDump/SanaSend on-target round-trip, 13=M13 buffhooks=32,
 * 14=M14 replaycap=0 (out-of-range config value, open must FAIL at pure
 * validation -- see SANA2LOOP_CONFIG_MAX_REPLAYCAP's own comment for why
 * `replaycap=`'s POSITIVE case -- a raised cap actually loading more
 * than the default -- is deliberately NOT covered by this on-target
 * suite at all: it needs a third simultaneously-held replay buffer,
 * which empirically exceeds bundled AROS's tight 512K+512K profile at
 * this point in the sequence even at the historical default cap size;
 * verified instead via real Kickstart 1.3/3.1 locally, see
 * docs/PLAN.md's M14 risk note). Pre-launch-review `truncate=`/
 * `rxqueue=`-bound-hit/`recordmax=`-overflow coverage all deliberately
 * reuse EXISTING units (1 and 11) rather than adding new ones -- see
 * unit1.config's and unit11.config's own comments: even UNOPENED extra
 * units measurably shrink bundled AROS's already-tight free-memory
 * headroom for later dynamic allocations elsewhere in the session (this
 * was tried and reliably broke the unrelated "scheduled" sub-test's own
 * replay-buffer allocation on bundled AROS specifically -- the same
 * class of AROS memory-pressure regression M9/M14 already hit, this
 * time avoided by not growing this constant at all).
 *
 * M15 grew this to 16 anyway (unit 15: the wire=slip fixture) -- the
 * calculus changed when the same pre-launch pass moved machine.toml's
 * memory floor from 1 MB to 2 MB (the documented minimum since), which
 * is exactly the headroom the reuse-don't-grow workarounds above were
 * compensating for the absence of. Verified against bundled AROS after
 * the growth, same as every struct-size change here must be.
 * M16: 18 (units 16/17 -- the errors=1 and loss=1 fixtures; each needs
 * its own unit because both knobs change what a plain write observably
 * does, which every other unit's own sub-test depends on).
 * M17: 19 (unit 18 -- the delay=10 fixture; needs its own unit for the
 * same reason as M16's pair, and because it's also the first unit whose
 * su_Tick advances from an ordinary CMD_WRITE, not just scheduled
 * replay/recording -- sharing a unit with either of those would make
 * su_Tick's own advance ambiguous between the two features). */
#define SANA2LOOP_NUM_UNITS   19
#define SANA2LOOP_DEFAULT_MTU 1500
/* Reported by S2_DEVICEQUERY (BPS field) unless `bps=` (M15) overrides it
 * per unit -- was a bare hardcoded 10000000 at the query site before
 * that. Purely a reported value: nothing in this device throttles. */
#define SANA2LOOP_DEFAULT_BPS 10000000
/* M15 `wire=` per-wire-type defaults, applied only when the config names
 * a non-Ethernet wire and doesn't explicitly override mtu=/bps=. SLIP's
 * 1006 is the classic RFC 1055 default (296 is the interactive-use
 * convention; 1006 the historical BSD maximum) and matches nullsana's own
 * example configs; the BPS values are honest serial-line-era defaults,
 * reported-only like SANA2LOOP_DEFAULT_BPS. Address size 4 for the
 * point-to-point wires follows nullsana's "an IP number as hardware
 * address" convention -- SANA-II itself gives SLIP/PPP no hardware
 * addressing, and a 4-byte IP is the established stand-in real stacks
 * expect from such drivers. */
#define SANA2LOOP_SLIP_MTU      1006
#define SANA2LOOP_SLIP_BPS      19200
#define SANA2LOOP_PPP_BPS       115200
#define SANA2LOOP_ADDR_ETHERNET 6
#define SANA2LOOP_ADDR_P2P      4
/* M16 `errors=`/`loss=` ceiling: both are 1-in-N rates and the draw is a
 * 16x16->32 multiply (native mulu.w on plain 68000 -- see
 * prng_hits_one_in()), so N must fit a UWORD. 1-in-65535 is already far
 * rarer than any realistic test needs. */
#define SANA2LOOP_CONFIG_MAX_ONEIN 65535
/* Ethernet dst[6]+src[6]+type[2] -- the allowance SANA2IOF_RAW adds on top
 * of su_MTU (docs/PLAN.md's M4 entry: "SANA2IOF_RAW (buffer -> MTU+14)").
 * Scoped deliberately narrow: this device only relaxes the MTU-exceeded
 * size check by this much when the flag is set, it does NOT reinterpret
 * the buffer as a literal header-plus-payload frame the way a real
 * hardware driver (e.g. plipbox) does -- there is no real wire framing
 * here to parse a header out of, so growing the size budget is the whole
 * of what "raw" usefully means for a loopback device. */
#define SANA2LOOP_RAW_HDR_SIZE 14
/* Fixed queued-packet buffer size: MTU plus the RAW allowance above, since
 * a RAW write is still just one more CMD_WRITE-shaped packet through the
 * exact same EchoPacket buffer -- it needs room for the biggest thing
 * that's allowed to arrive, not just the plain MTU. Per-unit configurable
 * MTU (from ENV:sana2loop/unitN.config) is a later phase; for now this is
 * also the hard cap enforced against ios2_DataLength. */
#define SANA2LOOP_MAX_PACKET  (SANA2LOOP_DEFAULT_MTU + SANA2LOOP_RAW_HDR_SIZE)
/* S2_TRACKTYPE table size: fixed, not dynamically grown -- consistent with
 * this device's "no hidden buffers/allocations behind the caller's back"
 * ethos elsewhere (rxqueue=0 default, fixed-size EchoPacket). A caller that
 * needs more simultaneously-tracked types gets S2ERR_NO_RESOURCES, same as
 * any other fixed-capacity resource here. */
#define SANA2LOOP_MAX_TRACKED_TYPES 8

/* S2_ADD/DELMULTICASTADDRESS table size: same fixed-capacity reasoning as
 * SANA2LOOP_MAX_TRACKED_TYPES -- a caller that needs more simultaneously
 * joined groups gets S2ERR_NO_RESOURCES/S2WERR_MULTICAST_FULL. */
#define SANA2LOOP_MAX_MULTICAST 8

/* M7 reorder buffer size -- docs/PLAN.md's "fixed 8-slot pointer array".
 * Also the config ceiling for `reorder=N` (the window size). */
#define SANA2LOOP_MAX_REORDER 8

/* M8 replay: cap on how many pcap records a `mode=replay` unit will load,
 * same fixed-capacity-over-dynamic-growth reasoning as
 * SANA2LOOP_MAX_TRACKED_TYPES/_MULTICAST above -- still a hard per-unit
 * ceiling, just a per-unit CONFIGURABLE one since M14 (`replaycap=N`,
 * su_ReplayCap), not a single compile-time constant for every unit. A
 * capture with more records than the active cap is REJECTED outright at
 * open time (S2ERR_BAD_ARGUMENT) rather than silently truncated --
 * silently playing back only part of a named capture would be a worse
 * surprise than a loud, obvious failure. SANA2LOOP_MAX_REPLAY_PACKETS is
 * now only the DEFAULT (no `replaycap=` given) -- unchanged from its
 * original M8/M9 value, so every existing config/fixture that never sets
 * `replaycap=` is byte-for-byte unaffected. That default value (4) had to
 * be re-verified empirically twice, at M8 and again at M9 -- see
 * load_replay_file()'s own comment for the full memory-pressure story
 * behind picking it, which is exactly *why* this became configurable
 * rather than just raising the constant: the safe ceiling depends on the
 * target's own real memory budget (bundled AROS under Copperline is
 * measurably tighter than real hardware -- see that same comment), which
 * only a config knob, not a single compile-time guess, can accommodate. */
#define SANA2LOOP_MAX_REPLAY_PACKETS 4
/* Sanity ceiling on `replaycap=N` -- not a spec requirement, same guard
 * purpose as SANA2LOOP_CONFIG_MAX_RECORDMAX (itself ~256KB): each replay
 * slot costs roughly sizeof(struct EchoPacket) (~1.5KB, dominated by
 * SANA2LOOP_MAX_PACKET) plus a few bytes of due-tick/delivery-copy
 * bookkeeping, so this ceiling's ~400KB worst case is the same order of
 * magnitude as recordmax's own ceiling, not a promise it fits any given
 * target -- it's the operator's own job to pick a `replaycap=` their
 * target's actual free memory can satisfy (AllocMem() failure at open
 * time fails loudly, per S2ERR_NO_RESOURCES, not silently). */
#define SANA2LOOP_CONFIG_MAX_REPLAYCAP 256
/* dos.library path/filename length for `replay=<path>` -- generous for
 * "S:sana2loop/whatever.pcap"-shaped values; same bounded-not-dynamic
 * reasoning as the config-path buffers in read_unit_config() below. */
#define SANA2LOOP_MAX_REPLAY_PATH 64

/* M9: RAM record buffer -- `record=<path>` taps deliver_packet() and
 * appends every packet THIS unit actually receives (regardless of mode --
 * echo self-delivery, crossover peer delivery, or scheduled-replay's
 * manufactured deliveries) as pcap-formatted bytes into a fixed-size RAM
 * buffer, flushed to `record_path` at last close (only if the closer is a
 * real Process -- same dos.library-needs-process-context reasoning as
 * config/replay file I/O). `recordmax` default matches docs/PLAN.md
 * decision #7 ("64 KB"); overflow stops recording further packets (not a
 * fatal error -- see su_RecordDropped) rather than truncating what's
 * already captured. NOTE: unlike SANA2LOOP_MAX_REPLAY_PACKETS (which M8
 * had to shrink from a 32-record/49KB guess after it reliably failed
 * AllocMem() under bundled AROS's tighter free-memory headroom), this
 * default is NOT independently re-verified against that same AROS memory
 * pressure -- this project's own on-target test deliberately configures a
 * much smaller `recordmax=` on its one recording fixture rather than
 * exercising the 64KB default, for exactly that reason. A real deployment
 * enabling `record=` under bundled AROS specifically may need to lower
 * `recordmax=` if AllocMem() ever fails for it. */
#define SANA2LOOP_DEFAULT_RECORDMAX (64 * 1024)
/* Sanity ceiling on `recordmax=N` -- not a spec requirement, same guard
 * purpose as SANA2LOOP_CONFIG_MAX_RXQUEUE. */
#define SANA2LOOP_CONFIG_MAX_RECORDMAX (256 * 1024)

/* SysBase -- the one file-scope global this file needs. Safe here: this is
 * a plain -nostdlib freestanding build (no -fbaserel), so globals use
 * normal addressing, not an A4-relative small-data segment that some
 * framework may or may not point at us. Set once in init_device(), which
 * Exec guarantees runs single-threaded before any Open. */
struct ExecBase *SysBase;

/* DOSBase -- like SysBase above, a plain global this toolchain's
 * <proto/dos.h> inline stubs reference by this exact name (dos.library
 * has no fixed low-memory pointer the way Exec does, so it must be
 * OpenLibrary()'d first); <proto/dos.h> itself already declares it
 * `extern struct DosLibrary *DOSBase` -- matching that exact type here is
 * what actually supplies its storage. Unlike SysBase, NOT set once at
 * init: dos.library isn't always available/needed (a device targeting
 * Kickstart 1.3 doesn't open it at load time), so each of the three
 * places this file ever touches dos.library -- read_unit_config(),
 * load_replay_file(), flush_record() -- opens it, uses it, and closes it
 * again within that single function call, every time.
 *
 * KNOWN, ACCEPTED CONCURRENCY GAP: none of those three calls run under
 * any lock against each other or against a concurrent open()/close() on
 * a DIFFERENT unit, so two overlapping DOS-I/O windows on this shared
 * global (and the `static` scratch buffers each of those three functions
 * uses -- see their own comments) can corrupt each other's in-flight
 * work, and DOSBase itself can go NULL out from under a caller still
 * mid-sequence if another caller's matching CloseLibrary() runs first.
 * open() DOES now bump dev->lib_OpenCnt before doing any of this DOS I/O
 * (see open()'s own comment) specifically to close the one sub-case that
 * was a genuine use-after-free rather than "just" data corruption: a
 * concurrent Avail-FLUSH-triggered expunge() freeing the whole device
 * base while an open() call was still blocked inside a DOS Wait(). The
 * remaining gap (concurrent DOS I/O corrupting EACH OTHER's static
 * buffers/DOSBase) is real but was deliberately NOT fixed here -- doing
 * so properly needs a real SignalSemaphore serializing every dos.library
 * touch in this file, a genuinely bigger structural change than fits
 * this pass, and this device's single-file test-tool nature makes the
 * realistic trigger (two DIFFERENT units' first-opens, or a close's
 * record flush, racing from separate tasks at the exact same moment)
 * rare enough that documenting it precisely here (rather than a vaguer
 * "known gap" with no detail) was judged the right tradeoff for now. */
struct DosLibrary *DOSBase;

/* M5 config-file infrastructure: ENV:sana2loop/unitN.config, falling back
 * to S:sana2loop/unitN.config, read once at first-open (unit_OpenCnt
 * 0->1, same trigger as reset_unit()) and only from a real Process opener
 * -- see read_unit_config()'s own comment. `mtu`/`rxqueue` (M5), `mode`/
 * `peer` (M6), `seed`/`droppct`/`duppct`/`truncate`/`oversilent`/
 * `reorder`/`offlineafter`/`onlineafter` (M7), and `mode=replay`/`replay`
 * (M8, load_replay_file()) are recognized here. The proposal's remaining
 * key vocabulary (record/replaymode/recordmax for M9's tick-scheduled
 * replay and RAM record) has nothing to configure yet -- adding parser
 * support for a key with no consuming feature behind it would be dead
 * code, so each lands with its own milestone instead. */
#define SANA2LOOP_CONFIG_BUF_SIZE 2048
/* Must leave SANA2LOOP_RAW_HDR_SIZE of headroom below the fixed
 * EchoPacket buffer (SANA2LOOP_MAX_PACKET) -- a configured MTU above this
 * would let a RAW write's relaxed size check (M4) exceed the buffer's
 * actual capacity. */
#define SANA2LOOP_CONFIG_MAX_MTU (SANA2LOOP_MAX_PACKET - SANA2LOOP_RAW_HDR_SIZE)
/* Sanity ceiling on the rxqueue convenience knob -- not a spec
 * requirement, just a guard against a typo'd huge value exhausting real
 * 1.3's limited chip/fast memory pool via runaway EchoPacket queuing. */
#define SANA2LOOP_CONFIG_MAX_RXQUEUE 64
/* `truncate=N` can't usefully exceed the biggest packet this device ever
 * builds -- same ceiling `mtu` itself uses. */
#define SANA2LOOP_CONFIG_MAX_TRUNCATE (SANA2LOOP_MAX_PACKET - SANA2LOOP_RAW_HDR_SIZE)
/* M17 `delay=`/`deviation=` ceiling, in VBlank ticks (SANA2LOOP_VBLANK_HZ
 * below) -- 3000 ticks is ~60 seconds at 50Hz, generously past any
 * realistic test scenario (a guard against a typo'd huge value hanging a
 * test run, same spirit as SANA2LOOP_CONFIG_MAX_RXQUEUE). Also load-
 * bearing for compute_delay_ticks()'s jitter draw: `deviation*2+1` must
 * fit a UWORD (<=65535) for its inline mulu.w operand -- 3000 leaves
 * enormous headroom under that, so the two constraints never fight. */
#define SANA2LOOP_CONFIG_MAX_DELAY 3000

/* M6: a unit is either a standalone echo loopback (Phase 1 behavior,
 * unchanged) or crossover-paired with exactly one other unit (a real,
 * separately-addressed peer -- see do_write()/deliver_packet()'s own
 * comments for how addressing and receive filtering differ between the
 * two). Plain ints, not something pulled from devices/sana2.h -- SANA-II
 * itself has no concept of "mode", this is entirely this device's own
 * config surface. */
#define SANA2LOOP_MODE_ECHO      0
#define SANA2LOOP_MODE_CROSSOVER 1
/* M8: a unit configured `mode=replay` plays back a pre-loaded pcap file's
 * packets in order, one per CMD_READ/S2_READORPHAN ("consume mode" --
 * docs/PLAN.md decision #7 -- no VBlank/timestamp scheduling at all, that's
 * M9's tick-scheduled replay). CMD_WRITE/S2_BROADCAST/S2_MULTICAST on a
 * replay-mode unit fail S2ERR_NOT_SUPPORTED (see do_write()) -- a canned
 * trace has nowhere meaningful to send anything. */
#define SANA2LOOP_MODE_REPLAY    2

/* M9: `replaymode` -- which of the two replay delivery models a
 * `mode=replay` unit uses. Default (no `replaymode` key at all) stays
 * `_CONSUME`, M8's existing behavior, for backward compatibility with
 * every config file/test written before this milestone existed.
 * `_SCHEDULED` is new: instead of CMD_READ/S2_READORPHAN pulling the next
 * record whenever asked, the VBlank interrupt server (vblank_server())
 * pushes each record through the normal deliver_packet() precedence
 * (typed read / orphan read / rxqueue / drop+stats) at the moment its own
 * captured timestamp comes due, relative to this unit's own su_Tick
 * (ticking only while online -- see reset_unit()/force_online()). A
 * CMD_READ/S2_READORPHAN submitted before a record is due just queues
 * normally and waits, exactly like any unit with no traffic yet -- see
 * the gating check in both command handlers below. */
#define SANA2LOOP_REPLAYMODE_CONSUME   0
#define SANA2LOOP_REPLAYMODE_SCHEDULED 1

/* M9: deliberately PAL/50Hz, not the actual detected refresh rate -- this
 * device has no timer.device dependency and no interest in exact
 * wall-clock fidelity, only in DETERMINISM under a fixed emulation (per
 * docs/PLAN.md decision #7: "Deterministic under Copperline (VBlank =
 * emulated time)"). Scheduling is also deliberately SECOND-granularity,
 * not microsecond-exact: a captured record's due tick is
 * `(ts_sec - first_ts_sec) * SANA2LOOP_VBLANK_HZ`, dropping ts_usec
 * entirely. Two records captured within the same real second get the
 * same due tick and are delivered back-to-back at that tick, in their
 * original file order (already guaranteed by sequential su_ReplayNext
 * consumption) -- a clean, honest simplification for a test tool, not an
 * attempt at real-world timing fidelity. This also sidesteps a genuine
 * hazard this project has hit twice already: `ts_usec`, a RUNTIME value,
 * multiplied/divided against a non-power-of-2 constant to convert between
 * microseconds and ticks would risk exactly the unavailable
 * __udivsi3/__mulsi3 library calls this -nostdlib build has no libgcc
 * for -- staying at whole-second granularity needs only `delta_sec * 50`,
 * a small-constant multiply of the same safely-synthesizable shape as
 * M7's already-proven `pct*655`. */
#define SANA2LOOP_VBLANK_HZ 50

/* M9 S2_GETSPECIALSTATS record type -- this device's own, since SANA-II
 * leaves "special" (per-driver) stat Type values entirely undefined; only
 * one exists so far. */
#define SANA2LOOP_SPECIALSTAT_RECORD_OVERFLOW 1
/* M13: how many opens on this unit supplied at least one Rev 7 DMA
 * buffer-management hook (S2_DMACopyTo/FromBuff32/64) -- surfaces that
 * this driver correctly recognized and recorded the negotiation even
 * though it always gracefully declines to actually use DMA (spec-legal:
 * "up to the driver... shall fall back to standard CPU callbacks if
 * necessary" -- see docs/PLAN.md's M13 section). */
#define SANA2LOOP_SPECIALSTAT_DMA_HOOKS_OFFERED 2

struct UnitConfig {
    ULONG mtu;
    ULONG rxqueue;
    ULONG mode;
    ULONG peer;
    ULONG seed;         /* M7: xorshift32 seed, 0 coerced to 1 at use time */
    ULONG droppct;      /* M7: 0-100 */
    ULONG duppct;       /* M7: 0-100 */
    ULONG truncate;     /* M7: 0 = disabled, else cap packet length to this */
    ULONG oversilent;   /* M7: 0 or 1 */
    ULONG reorder;      /* M7: 0-SANA2LOOP_MAX_REORDER window size */
    ULONG delay;        /* M17: `delay=N` -- fixed VBlank-tick delivery
                            latency, 0 = disabled (synchronous, unchanged
                            pre-M17 behavior) */
    ULONG deviation;    /* M17: `deviation=N` -- symmetric uniform jitter
                            around `delay` (ticks), 0 = no jitter */
    ULONG offlineafter; /* M7: 0 = disabled, else auto-offline after N writes */
    ULONG onlineafter;  /* M7: 0 = disabled, else auto-online after N failed
                            write attempts while offline */
    char  replay_path[SANA2LOOP_MAX_REPLAY_PATH]; /* M8: mode=replay's
                            source file; empty = not given. Transient --
                            only consulted once, right after config parsing,
                            to load_replay_file(); never itself persisted on
                            the unit (see open()'s own comment). */
    ULONG replaymode;   /* M9: SANA2LOOP_REPLAYMODE_CONSUME/_SCHEDULED */
    char  record_path[SANA2LOOP_MAX_REPLAY_PATH]; /* M9: `record=<path>` --
                            where su_RecordBuf gets flushed at last close;
                            empty = recording disabled. Same transient-only
                            contract as replay_path. */
    ULONG recordmax;    /* M9: RAM record buffer cap in bytes, default
                            SANA2LOOP_DEFAULT_RECORDMAX */
    ULONG buffhooks;    /* M13: 8 (default)/16/32 -- which negotiated Rev
                            2/3 buffer-management hook width this unit
                            actually INVOKES (see pick_copy_to_buff()/
                            pick_copy_from_buff()), not just accepts at
                            open() time */
    ULONG bps;          /* M15: `bps=N` -- S2_DEVICEQUERY's reported BPS,
                            default SANA2LOOP_DEFAULT_BPS. Reported only,
                            never a throttle. */
    ULONG mintu;        /* M15: `mintu=N` -- minimum accepted write size;
                            0 (default) = no lower bound, preserving every
                            pre-M15 unit's behavior byte-for-byte. */
    ULONG wire;         /* M15: `wire=` -- an S2WireType_* value, default
                            S2WireType_Ethernet. Selects DEVICEQUERY's
                            HardwareType, the station-address width, and
                            (unless overridden explicitly) mtu/bps
                            defaults -- see read_unit_config()'s
                            resolution step. */
    BOOL  mtu_explicit; /* M15: TRUE only if this parse actually saw an
                            `mtu=` line -- so `wire=`'s own per-wire mtu
                            default only applies in its absence,
                            regardless of key order in the file. */
    BOOL  bps_explicit; /* M15: same contract for `bps=`. */
    UBYTE addr[6];      /* M16: `addr=` -- operator-pinned station address
                            (becomes BOTH factory and current at open).
                            Byte count must match the wire's address width
                            -- validated in read_unit_config() after
                            `wire=` resolution, since either key may come
                            first. */
    ULONG addr_len;     /* M16: how many bytes `addr=` actually supplied;
                            0 = key absent, keep the derived factory
                            address (pre-M16 behavior). */
    ULONG errors;       /* M16: `errors=N` -- 1-in-N writes fail VISIBLY
                            (S2ERR_TX_FAILURE), 0 = disabled. */
    ULONG loss;         /* M16: `loss=N` -- 1-in-N writes silently lost,
                            0 = disabled. The finer-than-percent
                            counterpart to droppct. */
    ULONG replaycap;    /* M14: `replaycap=N` -- how many pcap records a
                            mode=replay unit may load, default
                            SANA2LOOP_MAX_REPLAY_PACKETS (4) */
};

static char CONFIG_ENV_PREFIX[] = "ENV:sana2loop/unit";
static char CONFIG_S_PREFIX[]   = "S:sana2loop/unit";
static char CONFIG_SUFFIX[]     = ".config";

/* Builds "<prefix><unit_index>.config" into buf by hand (no sprintf/
 * strcpy/strcat -- this is a -nostdlib freestanding build with no libc).
 *
 * Handles unit_index 0-99 (two decimal digits, tens digit omitted for
 * single-digit units so existing fixtures like "unit9.config" keep
 * working unchanged). A REAL, latent bug lived here from M7 (when
 * SANA2LOOP_NUM_UNITS first passed single digits conceptually possible,
 * though not yet reached) through M8 (NUM_UNITS=10, units 0-9, still all
 * single-digit) until M9 actually allocated units 10/11: the original
 * one-line `'0' + unit_index` substitution silently produced ':'/';'
 * (ASCII 58/59, one and two past '9') instead of "10"/"11", so
 * ENV:/S:sana2loop/unit10.config was never found under its real name --
 * not a parse error (missing file isn't one), just silently falling back
 * to compiled-in defaults, which looked exactly like "the config wasn't
 * applied" until traced back here. Tens-digit extraction is a division-free
 * repeated-subtraction loop (bounded, at most ~9 iterations for any
 * 2-digit input) -- consistent with this file's established "no runtime
 * div/mod without libgcc" discipline. */
static void build_config_path(char *buf, const char *prefix, UBYTE unit_index)
{
    int i = 0, k;
    UBYTE tens = 0, ones = unit_index;
    for (k = 0; prefix[k]; k++)
        buf[i++] = prefix[k];
    while (ones >= 10) {
        ones -= 10;
        tens++;
    }
    if (tens > 0)
        buf[i++] = (char)('0' + tens);
    buf[i++] = (char)('0' + ones);
    for (k = 0; CONFIG_SUFFIX[k]; k++)
        buf[i++] = CONFIG_SUFFIX[k];
    buf[i] = 0;
}

/* Decimal-digits-only parser (no sign, no leading/trailing whitespace
 * tolerance -- this is a strict format, not a forgiving one, matching the
 * "fail-fast on anything unexpected" design: a config typo should be
 * loud, not silently ignored or silently reinterpreted). Only needs
 * multiply-and-add, never division, so -- unlike the percentage-threshold
 * math a later fault-injection milestone will want -- this doesn't force
 * the -lgcc-vs-local-div-helpers decision docs/PLAN.md flags as an M5
 * risk; that decision stays deferred until something actually needs
 * 32-bit division. */
static BOOL parse_ulong(const char *s, const char *end, ULONG *out)
{
    ULONG v = 0;
    if (s >= end)
        return FALSE;
    for (; s < end; s++) {
        ULONG d;
        if (*s < '0' || *s > '9')
            return FALSE;
        d = (ULONG)(*s - '0');
        /* Overflow guard BEFORE the multiply-and-add, not after: an
         * unchecked `v = v*10 + d` silently wraps ULONG, and every caller
         * of this function then range-checks the WRAPPED value -- so
         * e.g. `rxqueue=4294967296` wraps to 0 and passes as a valid
         * "strict drop" setting, exactly the "config typo silently
         * reinterpreted" outcome this function's own header comment says
         * never happens. 429496729/5 are 0xFFFFFFFF/10 and 0xFFFFFFFF%10
         * as compile-time-constant literals (10's divisor is a literal,
         * so the compiler folds this at compile time -- no runtime
         * division, keeping this libgcc-free like the rest of the parser). */
        if (v > 429496729UL || (v == 429496729UL && d > 5UL))
            return FALSE;
        v = v * 10 + d;
    }
    *out = v;
    return TRUE;
}

/* Compares the byte range [key, key_end) against a NUL-terminated literal
 * -- this file's own hand-rolled strcmp/strncmp-alike, for the same
 * no-libc reason as parse_ulong() above. */
static BOOL key_matches(const char *key, const char *key_end, const char *name)
{
    int i;
    for (i = 0; key + i < key_end; i++) {
        if (name[i] == 0 || key[i] != name[i])
            return FALSE;
    }
    return name[i] == 0;
}

/* Parses one already-isolated "key=value" line (no surrounding whitespace
 * tolerance). Blank lines and '#' comments are filtered out by the caller
 * before a line ever reaches here. Returns FALSE on anything not
 * recognized -- an unknown key is exactly as fatal as a malformed one,
 * per the fail-fast design. */
static BOOL parse_config_line(struct UnitConfig *cfg, const char *line, const char *line_end)
{
    const char *eq = NULL;
    const char *p;
    ULONG value;

    for (p = line; p < line_end; p++) {
        if (*p == '=') {
            eq = p;
            break;
        }
    }
    if (!eq || eq == line || eq + 1 >= line_end)
        return FALSE; /* no '=', empty key, or empty value */

    if (key_matches(line, eq, "mtu")) {
        if (!parse_ulong(eq + 1, line_end, &value))
            return FALSE;
        if (value < 1 || value > SANA2LOOP_CONFIG_MAX_MTU)
            return FALSE;
        cfg->mtu = value;
        cfg->mtu_explicit = TRUE; /* M15: blocks `wire=`'s own mtu default */
        return TRUE;
    }
    if (key_matches(line, eq, "bps")) {
        /* M15: reported link speed (S2_DEVICEQUERY's BPS field), nothing
         * more -- deliberately NOT a throughput shaper; latency belongs
         * to the (separate) delay=/deviation= proposal, not a reporting
         * knob quietly becoming one. Any nonzero 32-bit value goes:
         * there's no wrong answer for a wire that doesn't exist. */
        if (!parse_ulong(eq + 1, line_end, &value))
            return FALSE;
        if (value < 1)
            return FALSE;
        cfg->bps = value;
        cfg->bps_explicit = TRUE;
        return TRUE;
    }
    if (key_matches(line, eq, "mintu")) {
        /* M15: minimum accepted write size. Same ceiling as mtu= (a
         * minimum above the largest legal mtu could never be satisfied);
         * the cross-field mintu<=mtu check itself lives in
         * read_unit_config(), after wire='s own mtu default resolution,
         * since the two keys may arrive in either order. Writes below
         * the minimum are REJECTED (S2ERR_BAD_ARGUMENT), never silently
         * padded -- padding would mutate the payload a reader sees,
         * against this device's byte-exact-delivery contract. */
        if (!parse_ulong(eq + 1, line_end, &value))
            return FALSE;
        if (value > SANA2LOOP_CONFIG_MAX_MTU)
            return FALSE;
        cfg->mintu = value;
        return TRUE;
    }
    if (key_matches(line, eq, "addr")) {
        /* M16: colon-separated hex bytes, 1-2 digits per component
         * (nullsana's own configs use single digits: `0:80:10:0:10:20`),
         * case-insensitive, hand-rolled like every other parse here (no
         * sscanf in a freestanding build). Only the byte COUNT ceiling is
         * enforced per-line; whether the count matches this unit's wire
         * address width is a cross-field question answered in
         * read_unit_config() after `wire=` resolution. */
        const char *p2 = eq + 1;
        ULONG count = 0;

        while (p2 < line_end) {
            ULONG byte = 0, digits = 0;
            while (p2 < line_end && *p2 != ':') {
                ULONG d;
                char c = *p2;
                if (c >= '0' && c <= '9')
                    d = (ULONG)(c - '0');
                else if (c >= 'a' && c <= 'f')
                    d = (ULONG)(c - 'a') + 10;
                else if (c >= 'A' && c <= 'F')
                    d = (ULONG)(c - 'A') + 10;
                else
                    return FALSE;
                byte = (byte << 4) | d;
                digits++;
                if (digits > 2)
                    return FALSE;
                p2++;
            }
            if (digits == 0 || count >= sizeof(cfg->addr))
                return FALSE; /* empty component (`::`, trailing `:`) or
                                 more bytes than any wire's address holds */
            cfg->addr[count++] = (UBYTE)byte;
            if (p2 < line_end)
                p2++; /* step over the ':' */
        }
        if (count == 0)
            return FALSE;
        cfg->addr_len = count;
        return TRUE;
    }
    if (key_matches(line, eq, "errors")) {
        /* M16: 1-in-N VISIBLE write failure (S2ERR_TX_FAILURE) -- the
         * loud counterpart to droppct/loss's silent vanishing, exercising
         * a stack's error-handling/retry path rather than its timeout
         * path. 0 disables; 1 fails every write (a deterministic
         * everything-fails mode, no PRNG involved). */
        if (!parse_ulong(eq + 1, line_end, &value))
            return FALSE;
        if (value > SANA2LOOP_CONFIG_MAX_ONEIN)
            return FALSE;
        cfg->errors = value;
        return TRUE;
    }
    if (key_matches(line, eq, "loss")) {
        /* M16: 1-in-N silent loss -- same observable behavior as droppct
         * (write succeeds, packet never arrives) at rates a whole percent
         * can't express (droppct's rarest is 1-in-100; real-world loss
         * worth simulating is often 1-in-1000 or rarer -- nullsana's own
         * example is exactly `loss 1000`). Composes with droppct: both
         * draw independently, in pipeline order. */
        if (!parse_ulong(eq + 1, line_end, &value))
            return FALSE;
        if (value > SANA2LOOP_CONFIG_MAX_ONEIN)
            return FALSE;
        cfg->loss = value;
        return TRUE;
    }
    if (key_matches(line, eq, "wire")) {
        /* M15: which wire this unit pretends to be. Only the types with a
         * real testing story are accepted: ethernet (the default,
         * byte-for-byte pre-M15 behavior) and the three point-to-point
         * serial wires (4-byte IP-as-address, no broadcast/multicast, no
         * RAW framing). arcnet/localtalk etc. stay unrecognized-key
         * failures until something actually needs them. */
        if (key_matches(eq + 1, line_end, "ethernet")) {
            cfg->wire = S2WireType_Ethernet;
            return TRUE;
        }
        if (key_matches(eq + 1, line_end, "slip")) {
            cfg->wire = S2WireType_SLIP;
            return TRUE;
        }
        if (key_matches(eq + 1, line_end, "cslip")) {
            cfg->wire = S2WireType_CSLIP;
            return TRUE;
        }
        if (key_matches(eq + 1, line_end, "ppp")) {
            cfg->wire = S2WireType_PPP;
            return TRUE;
        }
        return FALSE;
    }
    if (key_matches(line, eq, "rxqueue")) {
        if (!parse_ulong(eq + 1, line_end, &value))
            return FALSE;
        if (value > SANA2LOOP_CONFIG_MAX_RXQUEUE)
            return FALSE;
        cfg->rxqueue = value;
        return TRUE;
    }
    if (key_matches(line, eq, "mode")) {
        /* key_matches() is a generic byte-range-equals-literal compare,
         * not specifically about keys -- reused here for the value too. */
        if (key_matches(eq + 1, line_end, "echo")) {
            cfg->mode = SANA2LOOP_MODE_ECHO;
            return TRUE;
        }
        if (key_matches(eq + 1, line_end, "crossover")) {
            cfg->mode = SANA2LOOP_MODE_CROSSOVER;
            return TRUE;
        }
        if (key_matches(eq + 1, line_end, "replay")) {
            cfg->mode = SANA2LOOP_MODE_REPLAY;
            return TRUE;
        }
        return FALSE;
    }
    if (key_matches(line, eq, "peer")) {
        if (!parse_ulong(eq + 1, line_end, &value))
            return FALSE;
        if (value >= SANA2LOOP_NUM_UNITS)
            return FALSE;
        cfg->peer = value;
        return TRUE;
    }
    if (key_matches(line, eq, "seed")) {
        if (!parse_ulong(eq + 1, line_end, &value))
            return FALSE;
        cfg->seed = value; /* 0 is a legal config value -- coerced to 1
                               only where the PRNG is actually seeded */
        return TRUE;
    }
    if (key_matches(line, eq, "droppct")) {
        if (!parse_ulong(eq + 1, line_end, &value))
            return FALSE;
        if (value > 100)
            return FALSE;
        cfg->droppct = value;
        return TRUE;
    }
    if (key_matches(line, eq, "duppct")) {
        if (!parse_ulong(eq + 1, line_end, &value))
            return FALSE;
        if (value > 100)
            return FALSE;
        cfg->duppct = value;
        return TRUE;
    }
    if (key_matches(line, eq, "truncate")) {
        if (!parse_ulong(eq + 1, line_end, &value))
            return FALSE;
        if (value > SANA2LOOP_CONFIG_MAX_TRUNCATE)
            return FALSE;
        cfg->truncate = value;
        return TRUE;
    }
    if (key_matches(line, eq, "oversilent")) {
        if (!parse_ulong(eq + 1, line_end, &value))
            return FALSE;
        if (value > 1)
            return FALSE;
        cfg->oversilent = value;
        return TRUE;
    }
    if (key_matches(line, eq, "reorder")) {
        if (!parse_ulong(eq + 1, line_end, &value))
            return FALSE;
        if (value > SANA2LOOP_MAX_REORDER)
            return FALSE;
        cfg->reorder = value;
        return TRUE;
    }
    if (key_matches(line, eq, "delay")) {
        /* M17: fixed VBlank-tick delivery latency -- 0 (default) means
         * synchronous delivery, byte-for-byte pre-M17 behavior. */
        if (!parse_ulong(eq + 1, line_end, &value))
            return FALSE;
        if (value > SANA2LOOP_CONFIG_MAX_DELAY)
            return FALSE;
        cfg->delay = value;
        return TRUE;
    }
    if (key_matches(line, eq, "deviation")) {
        /* M17: symmetric jitter around `delay` (ticks), 0 = none. Legal
         * with delay=0 too -- that models pure scatter with no floor
         * (each write's actual latency independently drawn from
         * [0, deviation]), not an error. */
        if (!parse_ulong(eq + 1, line_end, &value))
            return FALSE;
        if (value > SANA2LOOP_CONFIG_MAX_DELAY)
            return FALSE;
        cfg->deviation = value;
        return TRUE;
    }
    if (key_matches(line, eq, "offlineafter")) {
        if (!parse_ulong(eq + 1, line_end, &value))
            return FALSE;
        cfg->offlineafter = value;
        return TRUE;
    }
    if (key_matches(line, eq, "onlineafter")) {
        if (!parse_ulong(eq + 1, line_end, &value))
            return FALSE;
        cfg->onlineafter = value;
        return TRUE;
    }
    if (key_matches(line, eq, "replay")) {
        /* Raw dos.library path/filename, not a numeric value -- copied
         * byte-for-byte (no interpretation) into a bounded buffer via
         * CopyMem (an Exec LVO, never compiler-synthesized), not a
         * hand-rolled dst[k]=src[k] loop -- that exact shape is the
         * "innocent-looking fixed/variable-count copy loop" idiom
         * GCC's -O2 replaces with a `memcpy` call on this -nostdlib
         * build (no libc, so no memcpy either -- see reset_unit()'s own
         * comment for the same lesson learned the hard way in M3, and
         * this milestone's own load_replay_file() for the matching
         * struct-array-indexing/`__mulsi3` variant of it). Rejects
         * (fail-fast, same as an out-of-range numeric value) a value too
         * long to fit rather than silently truncating a path, which could
         * otherwise resolve to a different, wrong file. */
        ULONG vlen = (ULONG)(line_end - (eq + 1));
        if (vlen >= sizeof(cfg->replay_path))
            return FALSE;
        CopyMem((APTR)(eq + 1), cfg->replay_path, vlen);
        cfg->replay_path[vlen] = 0;
        return TRUE;
    }
    if (key_matches(line, eq, "replaymode")) {
        if (key_matches(eq + 1, line_end, "consume")) {
            cfg->replaymode = SANA2LOOP_REPLAYMODE_CONSUME;
            return TRUE;
        }
        if (key_matches(eq + 1, line_end, "scheduled")) {
            cfg->replaymode = SANA2LOOP_REPLAYMODE_SCHEDULED;
            return TRUE;
        }
        return FALSE;
    }
    if (key_matches(line, eq, "record")) {
        /* Same bounded-CopyMem contract as `replay=` above. */
        ULONG vlen = (ULONG)(line_end - (eq + 1));
        if (vlen >= sizeof(cfg->record_path))
            return FALSE;
        CopyMem((APTR)(eq + 1), cfg->record_path, vlen);
        cfg->record_path[vlen] = 0;
        return TRUE;
    }
    if (key_matches(line, eq, "recordmax")) {
        if (!parse_ulong(eq + 1, line_end, &value))
            return FALSE;
        if (value < 1 || value > SANA2LOOP_CONFIG_MAX_RECORDMAX)
            return FALSE;
        cfg->recordmax = value;
        return TRUE;
    }
    if (key_matches(line, eq, "buffhooks")) {
        /* M13: which negotiated Rev 2/3 hook width this unit actually
         * invokes -- see pick_copy_to_buff()/pick_copy_from_buff(). Only
         * takes effect for an opener that actually supplied the matching
         * S2_CopyTo/FromBuff16/32 tag; one that didn't falls back to the
         * base hook transparently (graceful decline, not an error). */
        if (key_matches(eq + 1, line_end, "8")) {
            cfg->buffhooks = 8;
            return TRUE;
        }
        if (key_matches(eq + 1, line_end, "16")) {
            cfg->buffhooks = 16;
            return TRUE;
        }
        if (key_matches(eq + 1, line_end, "32")) {
            cfg->buffhooks = 32;
            return TRUE;
        }
        return FALSE;
    }
    if (key_matches(line, eq, "replaycap")) {
        /* M14: how many pcap records a mode=replay unit may load --
         * replacing a single compile-time ceiling (SANA2LOOP_MAX_REPLAY_
         * PACKETS, still the default) with a per-unit config knob, since
         * the actually-safe ceiling depends on the target's own free
         * memory (see that constant's own comment). */
        if (!parse_ulong(eq + 1, line_end, &value))
            return FALSE;
        if (value < 1 || value > SANA2LOOP_CONFIG_MAX_REPLAYCAP)
            return FALSE;
        cfg->replaycap = value;
        return TRUE;
    }
    return FALSE; /* unrecognized key -- fail-fast, not silently ignored */
}

/* Splits `buf` (length `len`; Read() does NOT NUL-terminate it) into
 * lines on '\n', trims one trailing '\r' per line (so CRLF-authored files
 * read fine too), skips blank lines and '#'-led comments, and feeds
 * everything else to parse_config_line(). Returns FALSE (fail-fast) on
 * the first line it can't make sense of. */
static BOOL parse_config_buffer(struct UnitConfig *cfg, const char *buf, LONG len)
{
    const char *p = buf;
    const char *end = buf + len;

    while (p < end) {
        const char *line = p;
        const char *line_end;
        while (p < end && *p != '\n')
            p++;
        line_end = p;
        if (line_end > line && line_end[-1] == '\r')
            line_end--;
        if (p < end)
            p++; /* skip the '\n' itself */

        if (line_end > line && *line != '#') {
            if (!parse_config_line(cfg, line, line_end))
                return FALSE;
        }
    }
    return TRUE;
}

/* Reads and parses ENV:sana2loop/unit<N>.config, falling back to
 * S:sana2loop/unit<N>.config if that doesn't exist. Neither existing is
 * NOT an error -- most units on most systems have no config file at all,
 * and compiled-in defaults apply. Only a file that EXISTS but fails to
 * parse is a fail-fast error: returns FALSE (caller must fail the open),
 * TRUE otherwise (including "no config file found anywhere").
 *
 * The caller gates this on the opener being a real Process (checked
 * BEFORE this is ever reached, in open()): dos.library's Open()/Read()/
 * Close() need process-level context (current directory, IoErr()
 * scoping, ...) a bare Task doesn't have. This is the first and only
 * place this device touches dos.library at all -- per docs/PLAN.md's M5
 * risk note ("DOS I/O breaking Forbid inside open() on real 1.3"), that
 * makes this the on-target proof of whether open() actually runs in a
 * state hostile to dos.library I/O (which can legitimately Wait()
 * internally) or not; see open()'s own comment for what was found.
 *
 * Debugging note: an on-target run of this on this project's own minimal
 * boot ADF (deliberately no `Assign ENV:` at all -- see CLAUDE.md) hung
 * indefinitely inside Open("ENV:...") specifically (confirmed via
 * temporary inline-RawPutChar tracing, since the hang happens inside
 * OpenDevice() itself, before any IORequest completes for the usual
 * ios2_WireError-leak debugging trick to work). DOS treats an
 * unrecognized device/assign name as a possible removable-volume
 * reference and puts up a blocking "please insert volume" requester --
 * fine interactively, fatal headless, and not something to just tolerate
 * as a test-environment quirk: ANY real caller whose ENV: happens to be
 * unassigned would hang the exact same way. Fixed at the correct place --
 * the process's `pr_WindowPtr`, the standard AmigaOS mechanism for
 * "suppress DOS requesters for me, fail cleanly instead" -- not by
 * special-casing the test ADF. */
static BOOL read_unit_config(struct UnitConfig *cfg, UBYTE unit_index)
{
    static char path[40];
    static char cfgbuf[SANA2LOOP_CONFIG_BUF_SIZE];
    struct Process *proc;
    APTR old_window_ptr;
    BPTR file;
    LONG len;

    DOSBase = (struct DosLibrary *)OpenLibrary((STRPTR)"dos.library", 0);
    if (!DOSBase)
        return TRUE; /* can't even ask -- treat as "no config", not fatal */

    /* Caller (open()) already confirmed FindTask(NULL) is NT_PROCESS
     * before ever calling this, so this cast is safe. Restored
     * unconditionally below -- this must never leak a suppressed
     * requester state into the rest of the caller's own process. */
    proc = (struct Process *)FindTask(NULL);
    old_window_ptr = proc->pr_WindowPtr;
    proc->pr_WindowPtr = (APTR)-1L;

    build_config_path(path, CONFIG_ENV_PREFIX, unit_index);
    file = Open((STRPTR)path, MODE_OLDFILE);
    if (!file) {
        build_config_path(path, CONFIG_S_PREFIX, unit_index);
        file = Open((STRPTR)path, MODE_OLDFILE);
    }

    proc->pr_WindowPtr = old_window_ptr;

    if (!file) {
        CloseLibrary((struct Library *)DOSBase);
        DOSBase = NULL;
        return TRUE; /* no config file anywhere -- defaults apply */
    }

    len = Read(file, cfgbuf, sizeof(cfgbuf));
    Close(file);
    CloseLibrary((struct Library *)DOSBase);
    DOSBase = NULL;

    if (len < 0)
        return FALSE; /* a real I/O error reading a file that DID exist */

    if (!parse_config_buffer(cfg, cfgbuf, len))
        return FALSE;

    /* M15 `wire=` default resolution, BEFORE any cross-field validation
     * below: a non-Ethernet wire brings its own mtu/bps defaults, but an
     * explicit mtu=/bps= line always wins regardless of where it sat
     * relative to the wire= line -- hence the _explicit flags rather than
     * simple parse-order overwriting. Ethernet (the default) resolves to
     * the unit's own seeded defaults, i.e. exactly pre-M15 behavior. */
    if (cfg->wire != S2WireType_Ethernet) {
        if (!cfg->mtu_explicit)
            cfg->mtu = SANA2LOOP_SLIP_MTU;
        if (!cfg->bps_explicit)
            cfg->bps = (cfg->wire == S2WireType_PPP) ? SANA2LOOP_PPP_BPS
                                                     : SANA2LOOP_SLIP_BPS;
    }

    /* M15: a minimum no legal write could ever satisfy alongside this
     * unit's (resolved) mtu is a config mistake, same fail-fast contract
     * as every per-line range check. */
    if (cfg->mintu > cfg->mtu)
        return FALSE;

    /* M16 `addr=` cross-field checks, AFTER wire resolution (either key
     * may come first in the file): the supplied byte count must match the
     * wire's own address width exactly -- a 4-byte address on an Ethernet
     * unit (or 6 on a slip one) is a config mistake, not something to
     * zero-pad into plausibility. On Ethernet, the group (multicast) bit
     * in the first octet is additionally rejected: a STATION address with
     * the group bit set is not a station address, and a test tool
     * accepting one would just relocate the confusion into the consumer
     * under test. */
    if (cfg->addr_len != 0) {
        ULONG want = (cfg->wire == S2WireType_Ethernet)
                   ? SANA2LOOP_ADDR_ETHERNET : SANA2LOOP_ADDR_P2P;
        if (cfg->addr_len != want)
            return FALSE;
        if (cfg->wire == S2WireType_Ethernet && (cfg->addr[0] & 0x01))
            return FALSE;
    }

    /* M15: replay/record are Ethernet-only -- the pcap side of this
     * device (load_replay_file()'s linktype check, record_append()'s
     * dst[6]+src[6]+type[2] frame synthesis) is built around
     * LINKTYPE_ETHERNET framing that a point-to-point wire simply doesn't
     * have. Declining loudly at open beats writing captures that lie
     * about their own linktype. */
    if (cfg->wire != S2WireType_Ethernet &&
        (cfg->mode == SANA2LOOP_MODE_REPLAY || cfg->record_path[0] != 0))
        return FALSE;

    /* Cross-field check, not expressible per-line: crossover mode needs a
     * real, distinct peer. Catches both an explicit bad `peer=` value and
     * `mode=crossover` given with no `peer=` at all -- units 0/1's
     * compiled-in default peer is themselves (reset_unit()), which this
     * rejects same as any other self-referencing or out-of-range value.
     * Units 2/3 never hit this: their own compiled-in defaults are
     * already a valid mutual pair, and this only runs when a config file
     * actually exists to override them. */
    if (cfg->mode == SANA2LOOP_MODE_CROSSOVER &&
        (cfg->peer >= SANA2LOOP_NUM_UNITS || cfg->peer == unit_index))
        return FALSE;

    /* Same reasoning, M8: replay mode with no `replay=` path at all has
     * nothing to play back -- unlike a config FILE itself (missing = "use
     * defaults", read_unit_config()'s own top-level contract), a named
     * replay source that's simply absent from the config is a config
     * mistake, not a legitimate "no replay" default (there's no sensible
     * default pcap to fall back to). */
    if (cfg->mode == SANA2LOOP_MODE_REPLAY && cfg->replay_path[0] == 0)
        return FALSE;

    return TRUE;
}

/* One echoed packet, queued either waiting for a CMD_READ (su_PendingData)
 * or consumed immediately by one already queued (su_PendingReads holds
 * IORequests instead, in that case). Fixed-size buffer: see
 * SANA2LOOP_MAX_PACKET above. */
struct EchoPacket {
    struct Node ep_Node;
    ULONG       ep_PacketType;
    UBYTE       ep_SrcAddr[SANA2_MAX_ADDR_BYTES];
    UBYTE       ep_DstAddr[SANA2_MAX_ADDR_BYTES];
    ULONG       ep_Length;
    ULONG       ep_DueTick;  /* M17: absolute su_Tick this packet becomes
                                 deliverable at, while parked in a writer's
                                 su_DelayQueue -- meaningless (never read)
                                 once the packet reaches su_PendingData or
                                 any other list. */
    UBYTE       ep_Data[SANA2LOOP_MAX_PACKET];
};

struct Sana2LoopUnit {
    struct Unit su_Unit;              /* standard exec unit header -- must be first */
    UBYTE       su_Index;             /* this unit's number, for reset_unit() */
    UBYTE       su_Station[6];        /* CURRENT station address -- what
                                          S2_GETSTATIONADDRESS's ios2_SrcAddr
                                          reports; S2_CONFIGINTERFACE may
                                          change this once per unit lifetime */
    UBYTE       su_FactoryStation[6]; /* FACTORY/default address -- fixed for
                                          the unit's lifetime (until the next
                                          reset_unit()); surfaced via
                                          S2_GETSTATIONADDRESS's ios2_DstAddr */
    BOOL        su_Configured;        /* TRUE once S2_CONFIGINTERFACE has
                                          succeeded once -- a second attempt
                                          fails S2ERR_BAD_STATE/
                                          S2WERR_IS_CONFIGURED */
    BOOL        su_Exclusive;         /* M18: TRUE while a SANA2OPF_MINE
                                          opener holds this unit -- gates
                                          open()'s own busy check; cleared
                                          by reset_unit() (the exclusive
                                          holder is necessarily gone by
                                          the time open count reaches 0
                                          again) */
    ULONG       su_MTU;
    ULONG       su_BPS;               /* M15: S2_DEVICEQUERY's reported BPS
                                          (`bps=`, default
                                          SANA2LOOP_DEFAULT_BPS) */
    ULONG       su_MinTU;             /* M15: minimum accepted write size
                                          (`mintu=`, 0 = no lower bound) */
    ULONG       su_WireType;          /* M15: S2WireType_* (`wire=`, default
                                          S2WireType_Ethernet) */
    ULONG       su_AddrSize;          /* M15: station-address width in BYTES
                                          (6 for Ethernet, 4 for the
                                          point-to-point wires) -- every
                                          address compare/copy on this unit
                                          spans exactly this many bytes */
    BOOL        su_Online;
    ULONG       su_RxQueueMax;        /* bounded pending-data depth; 0 = strict
                                          SANA-II drop semantics (the default --
                                          M5's config knob overrides this) */
    ULONG       su_PendingDataCount;  /* current su_PendingData depth */
    ULONG       su_Mode;              /* SANA2LOOP_MODE_ECHO or _CROSSOVER (M6) */
    ULONG       su_Peer;              /* crossover only: the OTHER unit index
                                          packets written here are delivered
                                          to. Meaningless (defaults to this
                                          unit's own index) in echo mode --
                                          that self-reference is exactly what
                                          catches "crossover with no peer
                                          configured" in read_unit_config(). */
    struct Sana2DeviceStats su_Stats;
    /* S2_TRACKTYPE table: fixed SANA2LOOP_MAX_TRACKED_TYPES slots, shared
     * unit-wide state (any open can track/untrack/query any type) --
     * su_TypeInUse[i] gates whether slot i holds a live entry at all. */
    BOOL        su_TypeInUse[SANA2LOOP_MAX_TRACKED_TYPES];
    ULONG       su_TypeTracked[SANA2LOOP_MAX_TRACKED_TYPES];
    struct Sana2PacketTypeStats su_TypeStats[SANA2LOOP_MAX_TRACKED_TYPES];
    /* S2_ADD/DELMULTICASTADDRESS table: fixed SANA2LOOP_MAX_MULTICAST
     * slots, refcounted -- two opens joining the same group both need to
     * leave before it's actually dropped. Consulted for real once M6 gives
     * crossover-mode delivery destination-address filtering (see
     * dest_matches_unit()); echo mode still doesn't filter by group
     * membership at all (every opener of an echo unit already sees
     * everything on it, unchanged since Phase 1). */
    BOOL        su_McastInUse[SANA2LOOP_MAX_MULTICAST];
    UBYTE       su_McastAddr[SANA2LOOP_MAX_MULTICAST][6];
    ULONG       su_McastRefCount[SANA2LOOP_MAX_MULTICAST];
    /* M7 fault injection -- see docs/sana2-notes.md's M7 section for the
     * full pipeline story. All zero/disabled by default (reset_unit()):
     * fault injection is opt-in, never a surprise. */
    ULONG       su_Seed;              /* configured seed, re-applied (0->1
                                          coerced) to su_PrngState at every
                                          reset_unit() */
    ULONG       su_PrngState;         /* live xorshift32 state */
    ULONG       su_DropPct;           /* 0-100 */
    ULONG       su_DupPct;            /* 0-100 */
    ULONG       su_Truncate;          /* 0 = disabled, else cap packet
                                          length to this many bytes */
    BOOL        su_Oversilent;        /* MTU-vs-peer exceeded -> silent
                                          accept-and-vanish instead of
                                          S2ERR_MTU_EXCEEDED (crossover
                                          only -- see do_write()) */
    ULONG       su_ReorderWindow;     /* 0 = disabled, else 1-
                                          SANA2LOOP_MAX_REORDER */
    struct EchoPacket *su_ReorderBuf[SANA2LOOP_MAX_REORDER]; /* arrival-
                                          ordered while buffered -- see
                                          reorder_push()/reorder_flush() */
    ULONG       su_ReorderCount;      /* current fill level, <= su_ReorderWindow */
    ULONG       su_DelayTicks;        /* M17: `delay=N` -- fixed VBlank-tick
                                          delivery latency, 0 = disabled */
    ULONG       su_Deviation;         /* M17: `deviation=N` -- symmetric
                                          jitter around su_DelayTicks
                                          (ticks), 0 = none */
    struct List su_DelayQueue;        /* M17: packets awaiting their
                                          ep_DueTick, most recently written
                                          at the tail -- NOT due-tick
                                          ordered (jitter can make a later
                                          write due sooner than an earlier
                                          one), so vblank_server() scans
                                          the whole list every tick rather
                                          than assuming sorted order; same
                                          reasoning ruled out reusing
                                          su_ReorderBuf's fixed-array shape
                                          here (an unbounded number of
                                          packets can be in flight at once,
                                          unlike reorder's fixed window) */
    ULONG       su_ErrorsOneIn;       /* M16: `errors=N` -- 1-in-N writes
                                          fail visibly (S2ERR_TX_FAILURE +
                                          S2EVENT_TX|ERROR fired), 0 =
                                          disabled */
    ULONG       su_LossOneIn;         /* M16: `loss=N` -- 1-in-N writes
                                          silently lost, 0 = disabled */
    ULONG       su_OfflineAfter;      /* 0 = disabled, else auto-S2_OFFLINE
                                          after this many successful writes */
    ULONG       su_OnlineAfter;       /* 0 = disabled, else auto-S2_ONLINE
                                          after this many failed (offline)
                                          write attempts */
    ULONG       su_WriteCounter;      /* successful writes since the last
                                          online transition -- su_OfflineAfter */
    ULONG       su_FailCounter;       /* failed write attempts since going
                                          offline -- su_OnlineAfter */
    /* M8 consume-mode replay: a pre-loaded pcap file's records, parsed
     * once at open time (load_replay_file()) into the exact same
     * EchoPacket shape CMD_WRITE builds, so CMD_READ/S2_READORPHAN's
     * existing fill_read_from_packet() serves them with no separate code
     * path. su_ReplayPackets is NULL unless su_Mode==SANA2LOOP_MODE_REPLAY
     * and loading succeeded; AllocMem'd to hold su_ReplayCap slots (not a
     * fixed-size array embedded in every unit -- only replay-mode units
     * pay for this), of which su_ReplayCount actually ended up used. */
    ULONG       su_ReplayCap;         /* M14: `replaycap=N` config (default
                                          SANA2LOOP_MAX_REPLAY_PACKETS) --
                                          the ACTUAL allocation size for
                                          su_ReplayPackets/su_ReplayDueTicks/
                                          su_ReplayDeliveryCopies this
                                          lifetime; reset_unit()'s matching
                                          FreeMem() must read this BEFORE
                                          resetting it to the default for
                                          the NEXT lifetime, same pattern as
                                          su_RecordMax's own free-then-reset
                                          ordering. */
    struct EchoPacket *su_ReplayPackets; /* base of the allocation -- kept
                                          only so reset_unit() can FreeMem
                                          it; never indexed directly (see
                                          su_ReplayNext) */
    struct EchoPacket *su_ReplayNext; /* next record to serve -- a plain
                                          running POINTER, walked forward
                                          one struct at a time (su_ReplayNext++,
                                          a fixed compile-time-constant
                                          address step) rather than indexed
                                          as su_ReplayPackets[su_ReplayPos]:
                                          that indexing form needs a runtime
                                          count*sizeof(struct EchoPacket)
                                          multiply for a non-power-of-2,
                                          which compiles to an unavailable
                                          __mulsi3 call on this -nostdlib
                                          -m68000 build -- found the hard
                                          way building this exact feature,
                                          see load_replay_file()'s comment */
    ULONG       su_ReplayCount;       /* records actually loaded */
    ULONG       su_ReplayPos;         /* records served so far; once this
                                          reaches su_ReplayCount, reads just
                                          queue normally (identical to "no
                                          traffic arrived yet") -- a plain
                                          counter, only ever compared, never
                                          used to index su_ReplayPackets */
    /* M9: which of the two replay delivery models (see
     * SANA2LOOP_REPLAYMODE_CONSUME/_SCHEDULED's own comment). su_Tick
     * ticks once per VBlank (vblank_server()) while this unit is online
     * AND (scheduled-replay mode OR recording OR M17's delay/deviation
     * active) -- gated on su_Online so a replay session's own "elapsed
     * time" (and, as of M17, a write's own transit delay) only counts
     * while the link is actually up, matching how a real interface only
     * experiences traffic time while connected -- see force_offline()'s
     * own su_DelayQueue-flush call for what happens to anything already
     * in flight when that gate slams shut. su_ReplayDueTicks[i] (ULONG, safely
     * indexable -- power-of-2 element size, unlike su_ReplayPackets) is
     * each loaded record's own due tick, computed once at load time from
     * its captured pcap timestamp relative to the FIRST record's own
     * timestamp (whole-second granularity -- see SANA2LOOP_VBLANK_HZ's
     * comment for why not microsecond-exact). */
    ULONG       su_ReplayMode;
    ULONG       su_Tick;
    /* Whole-VBlank-second counter for record_append()'s pcap ts_sec, kept
     * SEPARATE from su_Tick: su_Tick is a raw per-VBlank counter (50/sec,
     * SANA2LOOP_VBLANK_HZ) used for scheduled-replay due-tick comparisons
     * (su_ReplayDueTicks[] is also in raw-tick units, so that math needs
     * no conversion) -- but record_append() used to write su_Tick
     * directly into a field the pcap format itself defines as SECONDS,
     * stretching every recorded inter-packet gap by 50x if fed back as a
     * scheduled-replay capture, and displaying a 2-second session as 100
     * seconds in a real tool like Wireshark. su_TickSub counts VBlanks
     * 0..49 (division-free -- see vblank_server()) and rolls su_TickSec
     * forward once per real virtual second. (This per-unit growth is
     * what pushed machine.toml's own memory floor from 1 MB to 2 MB --
     * see that file's own comment for the full account.) */
    ULONG       su_TickSec;
    UBYTE       su_TickSub;
    ULONG      *su_ReplayDueTicks;
    /* SCHEDULED mode only: one individually-AllocMem'd, pre-populated
     * EchoPacket per record, made ONCE at load time (load_replay_file(),
     * ordinary task context) rather than lazily inside vblank_server()'s
     * own interrupt context. su_ReplayDeliveryCopies[i] is NULL once
     * consumed (ownership handed to deliver_packet(), which owns freeing
     * it from there per its existing contract) or if this unit isn't in
     * SCHEDULED mode at all (CONSUME mode never populates this -- its
     * reads are served directly from su_ReplayPackets/su_ReplayNext,
     * no ownership transfer, no interrupt involvement). Exists so
     * vblank_server() never has to call AllocMem() (or anything else
     * that allocates) from real interrupt context -- a real, if purely
     * hygienic, improvement, but NOT what fixed this milestone's
     * bundled-AROS hang; see vblank_server()'s own comment for what
     * actually did. */
    struct EchoPacket **su_ReplayDeliveryCopies;
    /* M9 RAM record: `record=<path>` taps deliver_packet() (see
     * record_append()) and appends every packet this unit actually
     * receives, pcap-formatted, into su_RecordBuf -- flushed to
     * su_RecordPath at last close, only if the closer is a real Process
     * (same dos.library-needs-process-context reasoning as config/replay
     * file I/O). su_RecordBuf is NULL unless su_Recording is TRUE.
     * su_RecordPath is remembered on the unit (unlike replay_path, which
     * is transient) because the flush happens at CLOSE time, potentially
     * long after open()'s config was read. */
    BOOL        su_Recording;
    char        su_RecordPath[SANA2LOOP_MAX_REPLAY_PATH];
    UBYTE      *su_RecordBuf;
    ULONG       su_RecordMax;
    ULONG       su_RecordUsed;
    ULONG       su_RecordDropped;     /* packets NOT appended because
                                          su_RecordMax was already reached
                                          -- not a fatal error, whatever's
                                          already captured stays intact;
                                          surfaced via S2_GETSPECIALSTATS
                                          per docs/PLAN.md decision #7 */
    /* M13: Rev 2/3/7 buffer-hook negotiation. su_BuffHooks (config
       `buffhooks=8|16|32`, default 8) picks which negotiated hook width
       this unit actually INVOKES -- see pick_copy_to_buff()/
       pick_copy_from_buff(); an opener that didn't supply the configured
       width's tag falls back to the base 8-bit hook transparently.
       su_DMAHooksOffered counts opens that supplied at least one Rev 7
       DMA hook (S2_DMACopyTo/FromBuff32/64) -- recorded/recognized, never
       invoked (see apply_buffer_mgmt_tags()); surfaced via
       S2_GETSPECIALSTATS as SANA2LOOP_SPECIALSTAT_DMA_HOOKS_OFFERED. */
    ULONG       su_BuffHooks;
    ULONG       su_DMAHooksOffered;
    struct List su_PendingReads;      /* queued, TYPED CMD_READ IORequests */
    struct List su_OrphanReads;       /* queued S2_READORPHAN IORequests (untyped) */
    struct List su_PendingData;       /* packets awaiting a CMD_READ/S2_READORPHAN,
                                          bounded by su_RxQueueMax */
    struct List su_EventReqs;         /* queued S2_ONEVENT IORequests */
};

struct Sana2LoopDevice {
    struct Device         sld_Device;
    BPTR                  sld_SegList;   /* handed back to Exec by expunge() */
    /* M9: one VBlank interrupt server for the whole device (not per-unit
     * -- see vblank_server()'s own comment), added once in init_device()
     * and removed in expunge(). Must live at a stable address for as long
     * as the device is loaded, hence embedded in the device base itself
     * rather than a separate AllocMem'd struct. */
    struct Interrupt      sld_VBlankInt;
    struct Sana2LoopUnit  sld_Units[SANA2LOOP_NUM_UNITS];
};

/* SANA-II buffer-management calling convention (spec-fixed, not our
 * choice): a0=to, a1=from, d0=size, result BOOL in d0. Consumer hooks are
 * arbitrary compiled code, so the ABI has to be nailed down with explicit
 * register annotations rather than trusting whatever convention bebbo's
 * gcc would otherwise pick for a plain function-pointer call -- verified
 * via objdump on an isolated test that the compiler really does load
 * a0/a1/d0 immediately before the jsr when the pointer's type carries
 * these annotations. */
typedef BOOL (*Sana2CopyFunc)(APTR to asm("a0"), APTR from asm("a1"), ULONG size asm("d0"));

/* M18: S2_PacketFilter's calling convention -- ALSO spec-fixed, but a
 * DIFFERENT register assignment from Sana2CopyFunc above (this file's
 * third distinct hook ABI, after the copy-hook convention and the VBlank
 * interrupt-server's own a1-plus-Z-flag convention): per the AmigaOS
 * wiki's Autodocs (Revision 2+3 page, sourced the same way M13 sourced
 * the other Rev 2/3/7 gaps this toolchain's own <devices/sana2.h> never
 * had),
 *
 *   keep = PacketFilter(hook, ios2, data)
 *   d0                   a0    a2    a1
 *   BOOL PacketFilter(struct Hook *, struct IOSana2Req *, APTR);
 *
 * -- note the register order (a0, a2, a1) does NOT match C parameter
 * order (hook, ios2, data); this is the standard Amiga "Hook" calling
 * convention's own documented natural-register-order quirk, not a typo.
 * Verified via objdump the same way M1 verified Sana2CopyFunc's a0/a1/d0
 * assignment -- see call_packet_filter()'s own comment. */
typedef BOOL (*Sana2FilterFunc)(struct Hook *h asm("a0"), struct IOSana2Req *io asm("a2"), APTR data asm("a1"));

#define SANA2LOOP_OPEN_MAGIC 0x53324C4F /* 'S2LO' */

/* Per-OpenDevice() state. SANA-II's OpenDevice contract: the caller may
 * point ios2_BufferManagement at a tag list (S2_CopyToBuff/S2_CopyFromBuff/
 * S2_PacketFilter) before opening; the device parses it and REPLACES
 * ios2_BufferManagement with its own per-open cookie, which the caller is
 * then contractually required to copy into every subsequent IOSana2Req it
 * submits. That means a queued request already carries its opener's
 * identity -- no side table needed to find "whose hooks do I use to
 * complete this read". */
struct Sana2LoopOpen {
    struct Sana2LoopUnit *slo_Unit;
    ULONG                  slo_Magic;
    Sana2CopyFunc          slo_CopyToBuff;
    Sana2CopyFunc          slo_CopyFromBuff;
    struct Hook           *slo_PacketFilter;  /* M18: dispatched per candidate
                                                  read, before the copy hook --
                                                  see call_packet_filter() */
    BOOL                   slo_Promiscuous;   /* SANA2OPF_PROM at open time --
                                                  ignored in echo mode (every
                                                  opener already sees
                                                  everything on its unit), but
                                                  in crossover mode (M6) lets
                                                  a specific queued read
                                                  accept a packet regardless
                                                  of destination address */
    /* M13: Rev 2/3 optional buffer-hook widths -- NULL unless this opener's
       own tag list actually supplied the matching tag (advisory only, per
       spec: "may be ignored by the driver for any data buffer at any
       time"). Same calling convention as slo_CopyToBuff/slo_CopyFromBuff
       above (confirmed by wiki.amigaos.net's "Revision 2+3" page: "the
       same calling interface as for S2_CopyToBuff or S2_CopyFromBuff,
       respectively"), so Sana2CopyFunc is the right type here too --
       nothing new to verify via objdump beyond M1's own existing proof. */
    Sana2CopyFunc          slo_CopyToBuff16;
    Sana2CopyFunc          slo_CopyFromBuff16;
    Sana2CopyFunc          slo_CopyToBuff32;
    Sana2CopyFunc          slo_CopyFromBuff32;
    /* M13: Rev 7 DMA hooks -- recorded as opaque APTR, never invoked (see
       apply_buffer_mgmt_tags()'s own comment for why the real calling
       convention doesn't need to be nailed down: a hardware-less RAM
       loopback has nothing to hand a DMA-capable buffer address to in the
       first place, and spec-legally declining is always correct: "up to
       the driver... shall fall back to standard CPU callbacks if
       necessary"). NULL unless this opener's own tag list supplied the
       matching tag. */
    APTR                   slo_DMACopyToBuff32;
    APTR                   slo_DMACopyFromBuff32;
    APTR                   slo_DMACopyToBuff64;
    APTR                   slo_DMACopyFromBuff64;
};

/*-----------------------------------------------------------
A library or device with a romtag should start with moveq #-1,d0 (to
safely return an error if a user tries to execute the file), followed by a
Resident structure.
------------------------------------------------------------*/
int __attribute__((no_reorder)) _start(void)
{
    return -1;
}

asm("romtag:                                \n"
    "       dc.w    "XSTR(RTC_MATCHWORD)"   \n"
    "       dc.l    romtag                  \n"
    "       dc.l    endcode                 \n"
    "       dc.b    "XSTR(RTF_AUTOINIT)"    \n"
    "       dc.b    "XSTR(DEVICE_VERSION)"  \n"
    "       dc.b    "XSTR(NT_DEVICE)"       \n"
    "       dc.b    "XSTR(DEVICE_PRIORITY)" \n"
    "       dc.l    _device_name            \n"
    "       dc.l    _device_id_string       \n"
    "       dc.l    _auto_init_tables       \n"
    "endcode:                               \n");

char device_name[]      = DEVICE_NAME;
char device_id_string[] = DEVICE_ID_STRING;

/* Our own __mulsi3 -- the libgcc 32x32->32 multiply helper this
 * -nostdlib build has never been able to link. Until M16 the strategy
 * was avoidance: keep every 32-bit multiply either constant-by-constant
 * (folded), constant-by-variable with a cheap shift/add decomposition
 * (synthesized inline), or restructured away entirely
 * (load_replay_file()'s load-bearing noinline at M8; prng_hits_pct()'s
 * pct*655 trick at M7; nm/objdump verification at M14). That strategy
 * broke down at M16: growing struct Sana2LoopUnit by two ULONGs pushed
 * its sizeof past the backend's shift/add budget at PRE-EXISTING
 * sld_Units[] index sites -- i.e. new code being careful no longer
 * guarantees the build links. Providing the real symbol retires the
 * entire hazard class instead: GCC may now emit __mulsi3 freely and it
 * simply links, here and for every future milestone.
 *
 * Standard schoolbook decomposition, lowest 32 bits only (which is all
 * __mulsi3 contracts to return): a*b = al*bl + ((al*bh + ah*bl) << 16),
 * each partial product a native 16x16->32 mulu.w -- written as inline
 * asm because expressing them as C `*` inside the very function that
 * IMPLEMENTS `*` invites the compiler to close the loop with a
 * recursive libcall (and GCC 6.5's m68k backend was already seen NOT
 * recognizing the (UWORD)a*(UWORD)b idiom as mulu.w here). A plain C
 * function definition matches the libcall ABI on this target (stack
 * args, result in d0 -- the default C convention, which m68k libcalls
 * use). Named __mulsi3 in C = ___mulsi3 at the assembler level, exactly
 * the undefined symbol the linker reports when it's missing.
 *
 * POSITION IS LOAD-BEARING: this definition must stay textually AFTER
 * _start()/romtag above. As the file's only other extern function it
 * otherwise gets emitted at text offset 0 (observed, not theoretical --
 * the first build placed it there), displacing _start's moveq #-1 safety
 * return from the front of the hunk: the same
 * whatever-is-first-gets-executed hazard sana2test.c's own top-of-file
 * comment documents for AmigaDOS commands. */
ULONG __mulsi3(ULONG a, ULONG b)
{
    ULONG low    = a & 0xFFFF;         /* al */
    ULONG cross1 = a & 0xFFFF;         /* al (second copy, for al*bh) */
    ULONG cross2 = a >> 16;            /* ah */

    __asm__("mulu.w %1,%0" : "+d"(low)    : "d"((UWORD)b));         /* al*bl */
    __asm__("mulu.w %1,%0" : "+d"(cross1) : "d"((UWORD)(b >> 16))); /* al*bh */
    __asm__("mulu.w %1,%0" : "+d"(cross2) : "d"((UWORD)b));         /* ah*bl */

    return low + ((cross1 + cross2) << 16);
}

/* ---- small list helpers (Disable()/Enable() critical sections: BeginIO
 * and AbortIO can run on any calling task, and our lists are shared across
 * every open of a unit, so plain list ops need protecting from a task
 * switch mid-splice) ---- */

static void safe_add_tail(struct List *list, struct Node *node)
{
    Disable();
    AddTail(list, node);
    Enable();
}

static struct Node *safe_rem_head(struct List *list)
{
    struct Node *n;
    Disable();
    n = RemHead(list);
    Enable();
    return n;
}

/* Only Remove() a node if it's actually still queued here -- AbortIO may
 * race a natural completion, and Remove()ing a node that's already been
 * unlinked (or belongs to a different list) corrupts that list. */
static BOOL safe_unlink_if_queued(struct List *list, struct Node *target)
{
    struct Node *n;
    BOOL found = FALSE;
    Disable();
    for (n = list->lh_Head; n->ln_Succ; n = n->ln_Succ) {
        if (n == target) {
            Remove(n);
            found = TRUE;
            break;
        }
    }
    Enable();
    return found;
}

/* Installed when an opener supplies no S2_CopyToBuff/S2_CopyFromBuff tag
 * (or no tag list at all) -- a plain CopyMem, but still through the exact
 * hook ABI, so callers of fill_read_from_packet()/CMD_WRITE never need to
 * know whether a real hook is installed or not: one call path, always. */
static BOOL default_copy(APTR to asm("a0"), APTR from asm("a1"), ULONG size asm("d0"))
{
    CopyMem(from, to, size);
    return TRUE;
}

/* Hand-rolled tag walk -- utility.library's FindTagItem() doesn't exist on
 * Kickstart 1.3, so this is the local loop the proposal calls for. Must
 * handle the control tags (TAG_DONE/IGNORE/MORE/SKIP) correctly or a real
 * stack's chained tag list breaks silently; that handling is exactly what
 * FindTagItem() normally hides from callers who never think about it. */
static void apply_buffer_mgmt_tags(struct Sana2LoopOpen *slo, struct TagItem *tags)
{
    slo->slo_CopyToBuff   = default_copy;
    slo->slo_CopyFromBuff = default_copy;
    slo->slo_PacketFilter = NULL;
    /* M13: Rev 2/3/7 optional hooks all default to NULL (already true from
     * open()'s MEMF_CLEAR allocation, but set explicitly here too -- same
     * defensive, don't-rely-on-caller-zeroing discipline slo_PacketFilter
     * above already follows). NULL means "not negotiated by this opener":
     * pick_copy_to_buff()/pick_copy_from_buff() fall back to the base hook
     * above, and the Rev 7 DMA hooks are simply never invoked at all. */
    slo->slo_CopyToBuff16      = NULL;
    slo->slo_CopyFromBuff16    = NULL;
    slo->slo_CopyToBuff32      = NULL;
    slo->slo_CopyFromBuff32    = NULL;
    slo->slo_DMACopyToBuff32   = NULL;
    slo->slo_DMACopyFromBuff32 = NULL;
    slo->slo_DMACopyToBuff64   = NULL;
    slo->slo_DMACopyFromBuff64 = NULL;

    if (!tags)
        return;

    for (;;) {
        switch (tags->ti_Tag) {
        case TAG_DONE:
            return;
        case TAG_IGNORE:
            break;
        case TAG_MORE:
            tags = (struct TagItem *)tags->ti_Data;
            continue; /* TAG_MORE ends the current array -- no tags++ */
        case TAG_SKIP:
            tags += tags->ti_Data; /* + the common tags++ below = ti_Data+1 */
            break;
        case S2_CopyToBuff:
            slo->slo_CopyToBuff = (Sana2CopyFunc)tags->ti_Data;
            break;
        case S2_CopyFromBuff:
            slo->slo_CopyFromBuff = (Sana2CopyFunc)tags->ti_Data;
            break;
        case S2_PacketFilter:
            slo->slo_PacketFilter = (struct Hook *)tags->ti_Data;
            break;
        /* M13: Rev 2/3 16/32-bit hooks -- same calling convention as the
         * base hooks above (see slo_CopyToBuff16's own field comment),
         * genuinely invoked by pick_copy_to_buff()/pick_copy_from_buff()
         * when `buffhooks=` asks for this width and this opener actually
         * supplied it. */
        case S2_CopyToBuff16:
            slo->slo_CopyToBuff16 = (Sana2CopyFunc)tags->ti_Data;
            break;
        case S2_CopyFromBuff16:
            slo->slo_CopyFromBuff16 = (Sana2CopyFunc)tags->ti_Data;
            break;
        case S2_CopyToBuff32:
            slo->slo_CopyToBuff32 = (Sana2CopyFunc)tags->ti_Data;
            break;
        case S2_CopyFromBuff32:
            slo->slo_CopyFromBuff32 = (Sana2CopyFunc)tags->ti_Data;
            break;
        /* M13: Rev 7 DMA hooks -- recorded only (never invoked, see
         * slo_DMACopyToBuff32's own field comment); su_DMAHooksOffered is
         * bumped by open() right after this call returns, once, if any of
         * the four came back non-NULL. */
        case S2_DMACopyToBuff32:
            slo->slo_DMACopyToBuff32 = (APTR)tags->ti_Data;
            break;
        case S2_DMACopyFromBuff32:
            slo->slo_DMACopyFromBuff32 = (APTR)tags->ti_Data;
            break;
        case S2_DMACopyToBuff64:
            slo->slo_DMACopyToBuff64 = (APTR)tags->ti_Data;
            break;
        case S2_DMACopyFromBuff64:
            slo->slo_DMACopyFromBuff64 = (APTR)tags->ti_Data;
            break;
        default:
            break;
        }
        tags++;
    }
}

/* M13: which hook this unit actually INVOKES for a read's CopyToBuff step
 * -- `buffhooks=16|32` (default 8, meaning "always the base hook") only
 * takes effect if THIS opener's own tag list actually negotiated that
 * width; otherwise this transparently falls back to the base hook, same
 * as a real driver declining an offered hook it doesn't want to use (the
 * spec's own words: "advisory only and may be ignored by the driver for
 * any data buffer at any time"). This is the "stack actually invoking it"
 * half of Rev 2/3 conformance -- accepting the tag at open() time
 * (apply_buffer_mgmt_tags() above) is the other half. */
static Sana2CopyFunc pick_copy_to_buff(struct Sana2LoopUnit *unit, struct Sana2LoopOpen *slo)
{
    if (unit->su_BuffHooks == 32 && slo->slo_CopyToBuff32)
        return slo->slo_CopyToBuff32;
    if (unit->su_BuffHooks == 16 && slo->slo_CopyToBuff16)
        return slo->slo_CopyToBuff16;
    return slo->slo_CopyToBuff;
}

/* Write-side counterpart to pick_copy_to_buff() above -- same fallback
 * reasoning. */
static Sana2CopyFunc pick_copy_from_buff(struct Sana2LoopUnit *unit, struct Sana2LoopOpen *slo)
{
    if (unit->su_BuffHooks == 32 && slo->slo_CopyFromBuff32)
        return slo->slo_CopyFromBuff32;
    if (unit->su_BuffHooks == 16 && slo->slo_CopyFromBuff16)
        return slo->slo_CopyFromBuff16;
    return slo->slo_CopyFromBuff;
}

/* M18: dispatches `slo`'s S2_PacketFilter hook (if negotiated) for one
 * candidate read against `pkt` -- TRUE means deliver, FALSE means reject
 * this candidate for this packet (the read stays queued for a future
 * one). No hook negotiated -> TRUE, matching the spec's own "may be
 * ignored by the driver" advisory default every other Rev 2/3 hook here
 * already follows.
 *
 * Caller contract: `io` must already have prefill_read_metadata() run on
 * it (ios2_DataLength/SrcAddr/DstAddr populated) BEFORE this call -- the
 * wiki Autodocs describe the filter seeing the request "as it would
 * appear if the packet were successfully returned", not the bare
 * pre-read request. `data` is `pkt->ep_Data` directly: this device never
 * prepends a hardware header of its own to strip, matching the spec's
 * "excluding hardware-specific headers" carve-out trivially (there are
 * none here to exclude). */
static BOOL call_packet_filter(struct Sana2LoopOpen *slo, struct IOSana2Req *io, struct EchoPacket *pkt)
{
    if (!slo->slo_PacketFilter)
        return TRUE;
    return ((Sana2FilterFunc)slo->slo_PacketFilter->h_Entry)(slo->slo_PacketFilter, io, pkt->ep_Data);
}

/* Looks up a tracked S2_TRACKTYPE entry by packet type; returns NULL if
 * `type` isn't currently tracked. The table is shared unit-wide state (any
 * open can S2_TRACKTYPE/S2_UNTRACKTYPE/S2_GETTYPESTATS it, and CMD_WRITE/
 * deliver_packet() update counts as traffic flows), so -- like the
 * pending-read/pending-data lists -- every access, lookup or mutation, must
 * run under Disable(); this function assumes the caller already holds it. */
static struct Sana2PacketTypeStats *find_tracked_locked(struct Sana2LoopUnit *unit, ULONG type)
{
    int i;
    for (i = 0; i < SANA2LOOP_MAX_TRACKED_TYPES; i++)
        if (unit->su_TypeInUse[i] && unit->su_TypeTracked[i] == type)
            return &unit->su_TypeStats[i];
    return NULL;
}

/* Plain 6-byte compare, not memcmp(): this freestanding build has no libc,
 * and (per this file's own hard-won lesson from M3) even an innocent-looking
 * fixed-count loop can get miscompiled into a libc call under -O2 -- but
 * that idiom recognition targets COPY loops (store = load), not compare
 * loops, so this one is safe as an ordinary loop. */
static BOOL mcast_addr_eq(const UBYTE *a, const UBYTE *b)
{
    int k;
    for (k = 0; k < 6; k++)
        if (a[k] != b[k])
            return FALSE;
    return TRUE;
}

/* Looks up a joined S2_ADDMULTICASTADDRESS entry by group address; returns
 * -1 if `addr` isn't currently joined. Same Disable()-held-by-caller
 * contract as find_tracked_locked() above, for the same reason (shared
 * unit-wide table any open can mutate). */
static int find_mcast_locked(struct Sana2LoopUnit *unit, const UBYTE *addr)
{
    int i;
    for (i = 0; i < SANA2LOOP_MAX_MULTICAST; i++)
        if (unit->su_McastInUse[i] && mcast_addr_eq(unit->su_McastAddr[i], addr))
            return i;
    return -1;
}

/* Exec's real NewList() isn't reliably declared under this minimal include
 * set (a freestanding -nostdlib build, deliberately not pulling in more of
 * the NDK than needed) -- it's three pointer stores, so just do them.
 * Moved up here (was originally defined just before reset_unit(), its
 * first caller) so abort_requests_for_open()/fire_event() below can use
 * it too, for a private local list to collect nodes onto outside any
 * Disable() window -- see their own comments. */
static void list_init(struct List *list)
{
    list->lh_Head     = (struct Node *)&list->lh_Tail;
    list->lh_Tail      = NULL;
    list->lh_TailPred  = (struct Node *)&list->lh_Head;
}

/* Aborts any request still queued (reads or event waits) under a cookie
 * that's about to be freed -- a well-behaved consumer closes with nothing
 * outstanding, but a test device should survive one that doesn't rather
 * than hand a freed cookie's hooks to a later packet delivery. */
static void abort_requests_for_open(struct Sana2LoopUnit *unit, struct Sana2LoopOpen *slo)
{
    struct List *lists[3];
    int li;

    lists[0] = &unit->su_PendingReads;
    lists[1] = &unit->su_OrphanReads;
    lists[2] = &unit->su_EventReqs;
    for (li = 0; li < 3; li++) {
        struct List collected;
        struct IOSana2Req *io, *next;
        struct Node *n;

        list_init(&collected);

        /* Unlink every matching node onto a private local list under ONE
         * Disable(), instead of the earlier pattern of dropping Disable()
         * around each ReplyMsg() and resuming the walk from a `next`
         * captured before the drop: in that window a concurrent AbortIO,
         * CMD_FLUSH, or vblank_server() delivery (interrupts ignore
         * Forbid, so this applies even inside close()'s Forbid) can
         * Remove() and reply the very node `next` points at. Resuming
         * from a stale `next` then either Remove()s an already-unlinked
         * node (corrupting its now-stale Succ/Pred's neighbours) or
         * ReplyMsg()s the same message twice. Collecting first and
         * replying after Enable() never resumes a walk across a dropped
         * critical section. */
        Disable();
        for (io = (struct IOSana2Req *)lists[li]->lh_Head;
             io->ios2_Req.io_Message.mn_Node.ln_Succ;
             io = next) {
            next = (struct IOSana2Req *)io->ios2_Req.io_Message.mn_Node.ln_Succ;
            if ((struct Sana2LoopOpen *)io->ios2_BufferManagement == slo) {
                Remove((struct Node *)io);
                io->ios2_Req.io_Error = IOERR_ABORTED;
                AddTail(&collected, (struct Node *)io);
            }
        }
        Enable();

        while ((n = RemHead(&collected)) != NULL)
            ReplyMsg(&((struct IOSana2Req *)n)->ios2_Req.io_Message);
    }
}

/* M18: the metadata half of fill_read_from_packet() below, split out so
 * deliver_packet()'s per-candidate fan-out walk can prefill a read --
 * making its ios2_DataLength/SrcAddr/DstAddr/SANA2IOF_BCAST|MCAST flags
 * exactly what the wiki Autodocs say S2_PacketFilter is entitled to see
 * ("as it would appear if the packet were successfully returned") -- and
 * consult that candidate's filter hook (call_packet_filter()) BEFORE
 * committing to the copy step. Returns the resolved struct
 * Sana2LoopOpen* on success, NULL on the same SANA2LOOP_OPEN_MAGIC
 * failure fill_read_from_packet() itself used to detect inline --
 * callers must treat NULL exactly like that function's own FALSE return
 * (S2ERR_NO_RESOURCES/S2WERR_BUFF_ERROR); the contract is unchanged,
 * just surfaced one function earlier now that a real decision (the
 * filter call) has to happen in between prefill and copy. */
static struct Sana2LoopOpen *prefill_read_metadata(struct IOSana2Req *io, struct EchoPacket *pkt)
{
    struct Sana2LoopOpen *slo = (struct Sana2LoopOpen *)io->ios2_BufferManagement;

    /* SANA2LOOP_OPEN_MAGIC catches a consumer that never copied the
     * per-open cookie OpenDevice() installed (see the field's own
     * comment) into THIS request -- until now, nothing actually checked
     * it, so a caller leaving ios2_BufferManagement pointing at its
     * original OpenDevice() tag list (or NULL) fell straight through to
     * pick_copy_to_buff() dereferencing whatever garbage was there and
     * calling through it as a function pointer. Folding this into the
     * function's EXISTING "FALSE = hook failure" contract means every
     * call site already handles it correctly (S2ERR_NO_RESOURCES/
     * S2WERR_BUFF_ERROR) with no changes needed there. Reading slo_Magic
     * through a bad pointer is safe on this no-MMU 68000 target -- no
     * page faults, just a read of whatever ordinary RAM happens to be at
     * that address (0 itself is SysBase, always mapped). */
    if (!slo || slo->slo_Magic != SANA2LOOP_OPEN_MAGIC)
        return NULL;

    CopyMem(pkt->ep_SrcAddr, io->ios2_SrcAddr, SANA2_MAX_ADDR_BYTES);
    CopyMem(pkt->ep_DstAddr, io->ios2_DstAddr, SANA2_MAX_ADDR_BYTES);
    io->ios2_PacketType = pkt->ep_PacketType;
    io->ios2_DataLength = pkt->ep_Length;

    /* SANA2IOF_BCAST/MCAST: informational flags on a completed read,
     * derived from the packet's actual destination address -- standard
     * Ethernet convention (all-1s = broadcast; else the low bit of the
     * first octet = group/multicast address). Cross-checked against a real
     * driver (plipbox) for the broadcast half specifically: it detects
     * SANA2IOF_BCAST with exactly this all-1s scan. plipbox never
     * implements multicast at all, so the low-bit half is this device's
     * own choice, not something it confirms -- but it's the standard
     * 802.3 convention, not a guess. Recomputed on every read, masking any
     * stale bits first, since a caller may reuse one IORequest across many
     * commands without resetting io_Flags itself (this harness does
     * exactly that -- see sana2test.c's send_io()/do_io() comment). */
    io->ios2_Req.io_Flags &= ~(SANA2IOF_BCAST | SANA2IOF_MCAST);
    /* M15: both conventions read here (all-1s = broadcast, low bit of the
     * first octet = group) are 802.3 Ethernet addressing specifically --
     * on a point-to-point wire (4-byte IP-as-address) neither concept
     * exists, so the flags just stay cleared there. */
    if (slo->slo_Unit->su_WireType == S2WireType_Ethernet) {
        int k;
        BOOL all_ff = TRUE;
        for (k = 0; k < 6; k++) {
            if (pkt->ep_DstAddr[k] != 0xFF) {
                all_ff = FALSE;
                break;
            }
        }
        if (all_ff)
            io->ios2_Req.io_Flags |= SANA2IOF_BCAST;
        else if (pkt->ep_DstAddr[0] & 0x01)
            io->ios2_Req.io_Flags |= SANA2IOF_MCAST;
    }

    return slo;
}

/* Copies the packet payload through the completing reader's own
 * CopyToBuff hook, after prefill_read_metadata() (above) has already set
 * every driver-owned field. Returns FALSE on hook failure (or the same
 * magic-check failure prefill_read_metadata() detects) so the caller can
 * fail the read with S2ERR_NO_RESOURCES/S2WERR_BUFF_ERROR instead of
 * claiming success over data that was never actually copied.
 *
 * M18: now a thin composition of prefill_read_metadata() + the copy step
 * -- kept as its own function for the two call sites (consume-mode
 * replay, rxqueue-satisfied reads) that complete a read with no
 * per-candidate filter decision to make in between. deliver_packet()'s
 * own fan-out walk calls the two halves separately instead, running
 * call_packet_filter() in the gap between them. */
static BOOL fill_read_from_packet(struct IOSana2Req *io, struct EchoPacket *pkt)
{
    struct Sana2LoopOpen *slo = prefill_read_metadata(io, pkt);

    if (!slo)
        return FALSE;

    return pick_copy_to_buff(slo->slo_Unit, slo)(io->ios2_Data, pkt->ep_Data, pkt->ep_Length);
}

/* Completes every QUEUED S2_ONEVENT request whose mask matches `event`,
 * fired on an actual S2_ONLINE/S2_OFFLINE transition. A request whose mask
 * already matched the CURRENT state at S2_ONEVENT submission time never
 * reaches this queue at all -- it's completed immediately at dispatch
 * (see the S2_ONEVENT case in begin_io()). An earlier version of this
 * device queued unconditionally and called that "edge-triggered by
 * design," which was wrong: cross-checked against plipbox
 * (github.com/cnvogelg/plipbox), a real shipped SANA-II driver, which
 * completes immediately when the requested state already holds. That's
 * also the more useful behavior for the common case ("tell me once the
 * link is up," which should return right away if it already is) --
 * requiring a spurious down-then-up cycle first would be a real
 * consumer-facing bug, not just a spec nicety. */
static void fire_event(struct Sana2LoopUnit *unit, ULONG event)
{
    struct IOSana2Req *io, *next;
    struct List collected;
    struct Node *n;

    list_init(&collected);

    /* Collect matching nodes onto a private local list under ONE
     * Disable(), same reasoning as abort_requests_for_open()'s own
     * comment: dropping Disable() mid-walk to ReplyMsg() and resuming
     * from a `next` captured before the drop risks a concurrent
     * AbortIO/CMD_FLUSH/vblank_server() delivery unlinking that exact
     * node first. */
    Disable();
    for (io = (struct IOSana2Req *)unit->su_EventReqs.lh_Head;
         io->ios2_Req.io_Message.mn_Node.ln_Succ;
         io = next) {
        next = (struct IOSana2Req *)io->ios2_Req.io_Message.mn_Node.ln_Succ;
        if (io->ios2_WireError & event) {
            Remove((struct Node *)io);
            io->ios2_WireError = io->ios2_WireError & event;
            io->ios2_Req.io_Error = 0;
            AddTail(&collected, (struct Node *)io);
        }
    }
    Enable();

    while ((n = RemHead(&collected)) != NULL) {
        io = (struct IOSana2Req *)n;
        if (!(io->ios2_Req.io_Flags & IOF_QUICK))
            ReplyMsg(&io->ios2_Req.io_Message);
    }
}

/* Forward declaration: reorder_push()/reorder_flush() below need to call
 * this, but its own definition (unchanged since M2/M6) lives further down
 * near do_write(), its other caller. */
static void deliver_packet(struct Sana2LoopUnit *unit, struct EchoPacket *pkt);

/* Forward declaration, same reasoning: M17's reorder_push() needs to call
 * this (a naturally-evicted packet is still subject to delay=), but its
 * own definition lives after reorder_flush() below, near the rest of
 * this M17 delay-queue machinery. */
static void deliver_with_delay(struct Sana2LoopDevice *sld, struct Sana2LoopUnit *writer, struct EchoPacket *pkt);

/* Which unit does a packet WRITTEN on `unit` actually get delivered to?
 * `unit` itself for echo (Phase 1's reflexive self-delivery), its
 * configured peer for crossover (M6). Shared by do_write() and the M7
 * reorder buffer, both of which need to resolve this independently of
 * each other (the reorder buffer may release a packet long after the
 * write that produced it). */
static struct Sana2LoopUnit *delivery_target(struct Sana2LoopDevice *sld, struct Sana2LoopUnit *unit)
{
    return (unit->su_Mode == SANA2LOOP_MODE_CROSSOVER) ? &sld->sld_Units[unit->su_Peer] : unit;
}

/* xorshift32 -- a small, fast, fully deterministic (given a fixed seed)
 * PRNG for fault-injection jitter/permutation; not cryptographic, doesn't
 * need to be. Needs only shifts and XORs -- no libgcc dependency (see
 * prng_index() below for why avoiding a general 32-bit modulo mattered
 * enough to shape its design too, and docs/PLAN.md's deferred -lgcc
 * decision, which this milestone -- despite being flagged as the one that
 * might finally force it -- still doesn't). */
static ULONG xorshift32(ULONG *state)
{
    ULONG x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

/* Draws a uniform index in [0, window) without a general ULONG % ULONG --
 * that compiles to a __umodsi3 call on this -nostdlib -m68000 build
 * regardless of how small the operands turn out to be at runtime (the
 * compiler can't see that from a runtime variable divisor). Masking to 3
 * bits (0-7, covering SANA2LOOP_MAX_REORDER's whole range) and rejecting
 * an out-of-window draw instead is plain compares/ANDs -- always
 * terminates quickly (window=8 never rejects at all; the worst case,
 * window=5, still only rejects 3/8 draws on average). */
static ULONG prng_index(ULONG *state, ULONG window)
{
    ULONG idx;
    do {
        idx = xorshift32(state) & 0x07;
    } while (idx >= window);
    return idx;
}

/* "Roughly pct% chance", using only a multiply (native on plain 68000,
 * unlike division) -- 655 approximates 65536/100, so comparing the upper
 * 16 bits of a draw against pct*655 approximates a pct% hit rate. 0 and
 * 100 are handled as EXACT boundaries rather than leaning on the
 * approximation at the edges: 100*655=65500, not the full 0-65535 range,
 * so without this a configured droppct=100 would still let ~0.05% of
 * writes through -- not the absolute guarantee "100%" has to mean. */
static BOOL prng_hits_pct(ULONG *state, ULONG pct)
{
    ULONG roll;
    if (pct == 0)
        return FALSE;
    if (pct >= 100)
        return TRUE;
    roll = xorshift32(state) >> 16;
    return roll < pct * 655;
}

/* M16: "roughly 1-in-n chance" for `errors=`/`loss=` -- the
 * finer-than-percent counterpart to prng_hits_pct() above, same
 * no-division discipline: hit iff a 16-bit draw lands below 65536/n,
 * decided as `roll * n < 65536` -- and written as the plain C multiply
 * it is, because M16 finally provided this build's own __mulsi3 (see
 * its definition near the top of the file): the backend may compile
 * this however it likes now, libcall included. (This function is in
 * fact what forced that decision -- its first version tried the
 * (UWORD)a*(UWORD)b native-mulu idiom, GCC 6.5 emitted __mulsi3
 * anyway, and the same rebuild surfaced __mulsi3 at pre-existing
 * struct-index sites too, proving avoidance no longer scaled.)
 * 0 and 1 are exact no-draw boundaries like prng_hits_pct()'s own
 * 0/100: a disabled knob must never perturb the PRNG sequence of the
 * OTHER knobs sharing this unit's stream (existing seeded tests depend
 * on their sequences), and n=1 has to mean EVERY write, not a
 * 65535-in-65536 approximation. */
static BOOL prng_hits_one_in(ULONG *state, ULONG n)
{
    ULONG roll;
    if (n == 0)
        return FALSE;
    if (n == 1)
        return TRUE;
    roll = xorshift32(state) >> 16;
    return roll * n < 65536UL;
}

/* M17 `delay=`/`deviation=`: resolves the actual delivery latency (in
 * VBlank ticks) for one write, drawing jitter only if `deviation` is
 * nonzero -- same exact-boundary discipline as every other PRNG-gated
 * knob here (a disabled knob draws nothing, so it never perturbs the
 * seeded sequence of any OTHER knob sharing this unit's PRNG stream).
 *
 * Jitter is symmetric uniform in [-deviation, +deviation], clamped so the
 * result never goes negative (a packet cannot arrive before it was
 * sent): `idx = (roll16 * window) >> 16` draws uniformly in
 * [0, window) from a 16-bit roll, where window = 2*deviation+1 -- the
 * same "top bits of a 16x16->32 mulu.w" technique prng_hits_pct()/
 * prng_hits_one_in() already use, not `% window` (a general modulo by a
 * runtime divisor needs __umodsi3, which this build still doesn't
 * provide -- only __mulsi3 got that treatment at M16). `idx - deviation`
 * (done in two unsigned branches below, since this file has no signed
 * arithmetic precedent to lean on) then lands anywhere in
 * [-deviation, +deviation]. `deviation` is config-capped at
 * SANA2LOOP_CONFIG_MAX_DELAY (3000), so `window` always fits a UWORD --
 * required for the inline mulu.w operand. */
static ULONG compute_delay_ticks(ULONG *state, ULONG delay, ULONG deviation)
{
    ULONG window, roll, idx, below;

    if (deviation == 0)
        return delay;

    window = deviation * 2 + 1;
    roll = xorshift32(state) >> 16;              /* 16-bit draw */
    __asm__("mulu.w %1,%0" : "+d"(roll) : "d"((UWORD)window));
    idx = roll >> 16;                             /* uniform in [0, window) */

    if (idx >= deviation)
        return delay + (idx - deviation);
    below = deviation - idx;
    if (below > delay)
        return 0; /* would go negative -- clamp, don't wrap */
    return delay - below;
}

/* Buffers `pkt` into `writer`'s fixed-size reorder window; once the
 * window is full, PRNG-evicts ONE of the currently-buffered packets
 * (which may be the one just inserted, all equally likely) for actual
 * delivery now, left-shifting the remaining buffered packets down over
 * the gap so they stay in their original relative arrival order. That
 * arrival-order invariant is what lets reorder_flush() below just walk
 * the buffer front-to-back with no separate bookkeeping -- and it's what
 * bounds how far out of order a packet can arrive: at most
 * `su_ReorderWindow - 1` LATER packets can overtake it before it's
 * forced out, not an unbounded shuffle. */
static void reorder_push(struct Sana2LoopDevice *sld, struct Sana2LoopUnit *writer, struct EchoPacket *pkt)
{
    struct EchoPacket *evicted = NULL;

    /* Multiple tasks can call do_write() -> here concurrently on the same
     * unit (the device's own multi-opener premise -- every other piece of
     * shared unit state gets the same treatment). Without Disable(), two
     * concurrent pushes can both increment su_ReorderCount past
     * su_ReorderWindow before either evaluates the eviction check: one
     * write then lands past the end of the fixed-size su_ReorderBuf
     * array, into adjacent struct fields, and (with the original `==`
     * check) the eviction path is skipped forever afterward. Pop the
     * evicted packet under Disable, but call deliver_packet() outside it
     * -- same "act outside Disable" discipline the rest of this file
     * uses for ReplyMsg/consumer hooks. */
    Disable();
    if (writer->su_ReorderCount < writer->su_ReorderWindow) {
        writer->su_ReorderBuf[writer->su_ReorderCount++] = pkt;
        if (writer->su_ReorderCount >= writer->su_ReorderWindow) {
            ULONG idx = prng_index(&writer->su_PrngState, writer->su_ReorderWindow);
            ULONG k;
            evicted = writer->su_ReorderBuf[idx];
            for (k = idx; k + 1 < writer->su_ReorderCount; k++)
                writer->su_ReorderBuf[k] = writer->su_ReorderBuf[k + 1];
            writer->su_ReorderCount--;
        }
    }
    Enable();

    /* M17: a naturally-evicted packet is still ordinary pipeline flow
     * (not a flush event), so it's still subject to delay= -- see
     * deliver_with_delay()'s own comment for why reorder_flush() below,
     * unlike this, deliberately bypasses it. */
    if (evicted)
        deliver_with_delay(sld, writer, evicted);
}

/* Delivers everything still sitting in `writer`'s reorder buffer, in
 * their original arrival order (no further PRNG permutation) -- so
 * nothing is ever silently stuck there forever. Called on S2_OFFLINE
 * (manual or auto via su_OfflineAfter), CMD_FLUSH, and last-close
 * (drain_unit()), per docs/PLAN.md's M7 entry. */
static void reorder_flush(struct Sana2LoopDevice *sld, struct Sana2LoopUnit *writer)
{
    struct Sana2LoopUnit *target = delivery_target(sld, writer);
    /* Snapshotted under Disable(), not walked live: this can run
     * concurrently with reorder_push() above (CMD_FLUSH/S2_OFFLINE/close
     * on one open racing a CMD_WRITE on another), and deliver_packet()
     * must never be called with interrupts/scheduling disabled -- same
     * "pop under Disable, act outside" discipline as everywhere else in
     * this file. SANA2LOOP_MAX_REORDER bounds the copy. Via CopyMem(), not
     * a plain element-by-element loop -- a raw loop here got recognized
     * by -O2 and replaced with a call to `memcpy`, which doesn't exist in
     * this freestanding -nostdlib build (the exact same class of
     * miscompile M8/M13 already hit and fixed the identical way; see
     * S2_GETTYPESTATS' own comment). */
    struct EchoPacket *snapshot[SANA2LOOP_MAX_REORDER];
    ULONG count;
    ULONG i;

    Disable();
    count = writer->su_ReorderCount;
    CopyMem(writer->su_ReorderBuf, snapshot, count * sizeof(struct EchoPacket *));
    writer->su_ReorderCount = 0;
    Enable();

    for (i = 0; i < count; i++)
        deliver_packet(target, snapshot[i]);
}

/* M17: the pipeline's final stage, after reorder -- either hands `pkt`
 * straight to deliver_packet() (no delay configured, the common case,
 * unchanged pre-M17 behavior) or parks it on `writer`'s su_DelayQueue
 * with a resolved due tick for vblank_server()'s delay_queue_tick() to
 * release later. `writer`, not the delivery target: delay models the
 * WRITER's own link latency, same convention every other fault knob here
 * already uses (droppct/duppct/truncate/reorder are all keyed off the
 * writing unit's config, not the peer's).
 *
 * Deliberately the ONLY caller that can enqueue onto su_DelayQueue --
 * reorder_flush() and delay_queue_flush() itself both call
 * deliver_packet() DIRECTLY, bypassing this, because a flush's whole
 * point is "push everything out NOW" (see delay_queue_flush()'s own
 * comment): re-queuing an already-flushed packet through a fresh delay
 * would defeat that. */
static void deliver_with_delay(struct Sana2LoopDevice *sld, struct Sana2LoopUnit *writer, struct EchoPacket *pkt)
{
    ULONG delay;

    if (writer->su_DelayTicks == 0 && writer->su_Deviation == 0) {
        deliver_packet(delivery_target(sld, writer), pkt);
        return;
    }

    delay = compute_delay_ticks(&writer->su_PrngState, writer->su_DelayTicks, writer->su_Deviation);
    if (delay == 0) {
        /* Jitter clamped an already-small delay to 0, or deviation alone
         * (no base delay) happened to draw 0 this time -- immediate
         * delivery, same as the disabled case above. */
        deliver_packet(delivery_target(sld, writer), pkt);
        return;
    }

    pkt->ep_DueTick = writer->su_Tick + delay;
    Disable();
    AddTail(&writer->su_DelayQueue, (struct Node *)pkt);
    Enable();
}

/* Called once per VBlank (vblank_server()) for any unit whose
 * su_DelayQueue might have something in it. Walks the WHOLE queue every
 * tick rather than assuming due-tick order -- jitter means a packet
 * enqueued LATER can have an EARLIER due tick than one already waiting
 * (that's the whole point of jitter: it's allowed to reorder delivery),
 * so there's no sorted-prefix shortcut to take. Collects due packets
 * under Disable() (this can race a concurrent do_write() -> AddTail on
 * the same queue, or a concurrent CMD_FLUSH/S2_OFFLINE ->
 * delay_queue_flush() draining the whole thing), then delivers them
 * OUTSIDE Disable() -- same "pop under Disable, act outside" discipline
 * fire_event() and reorder_push() both already use, and required here
 * for the identical reason: deliver_packet() calls consumer hooks and
 * ReplyMsg(), neither legal with interrupts disabled. */
static void delay_queue_tick(struct Sana2LoopDevice *sld, struct Sana2LoopUnit *unit)
{
    struct EchoPacket *pkt, *next;
    struct List due;
    struct Node *n;

    list_init(&due);

    Disable();
    for (pkt = (struct EchoPacket *)unit->su_DelayQueue.lh_Head;
         pkt->ep_Node.ln_Succ;
         pkt = next) {
        next = (struct EchoPacket *)pkt->ep_Node.ln_Succ;
        if (pkt->ep_DueTick <= unit->su_Tick) {
            Remove((struct Node *)pkt);
            AddTail(&due, (struct Node *)pkt);
        }
    }
    Enable();

    while ((n = RemHead(&due)) != NULL)
        deliver_packet(delivery_target(sld, unit), (struct EchoPacket *)n);
}

/* Delivers EVERYTHING still sitting in `writer`'s delay queue immediately,
 * ignoring each packet's own remaining wait -- the delay-queue analogue
 * of reorder_flush() (same trigger set: S2_OFFLINE, CMD_FLUSH,
 * last-close), and for the same reason: vblank_server() only ticks a
 * unit while it's online (see su_Tick's own field comment), so anything
 * still parked here when a unit goes offline would otherwise wait
 * forever for a tick that never comes. Preserves original write
 * (insertion) order, not due-tick order -- same choice reorder_flush()
 * makes for its own buffer. */
static void delay_queue_flush(struct Sana2LoopDevice *sld, struct Sana2LoopUnit *writer)
{
    struct Sana2LoopUnit *target = delivery_target(sld, writer);
    struct List drained;
    struct Node *n;

    list_init(&drained);

    Disable();
    while ((n = RemHead(&writer->su_DelayQueue)) != NULL)
        AddTail(&drained, n);
    Enable();

    while ((n = RemHead(&drained)) != NULL)
        deliver_packet(target, (struct EchoPacket *)n);
}

/* The last step of the fault pipeline: either buffer `pkt` for later,
 * PRNG-permuted release (su_ReorderWindow > 0), or hand it to
 * deliver_with_delay() (M17: applies delay=/deviation= if configured,
 * else identical to a direct deliver_packet() call -- see that
 * function's own comment). Used for both the original packet and
 * duppct's duplicate, if any -- the plan's pipeline order (droppct ->
 * truncate -> duppct -> reorder -> delay) puts reorder AFTER
 * duplication (so a dup is just as subject to reordering as the
 * original) and delay LAST (so a packet's simulated transit latency is
 * whatever's left after every other fault decision already ran). */
static void deliver_or_reorder(struct Sana2LoopDevice *sld, struct Sana2LoopUnit *unit, struct EchoPacket *pkt)
{
    if (unit->su_ReorderWindow > 0)
        reorder_push(sld, unit, pkt);
    else
        deliver_with_delay(sld, unit, pkt);
}

/* Shared by S2_ONLINE and M7's su_OnlineAfter auto-recovery -- exactly
 * the same transition either way, just a different trigger. */
static void force_online(struct Sana2LoopUnit *unit)
{
    unit->su_Online = TRUE;
    unit->su_FailCounter = 0;
    fire_event(unit, S2EVENT_ONLINE);
}

/* Shared by S2_OFFLINE and M7's su_OfflineAfter auto-trigger. Flushes the
 * reorder buffer as part of going offline -- a unit that's about to stop
 * accepting writes shouldn't leave whatever it already accepted stuck
 * in a buffer with no future write ever arriving to push it back out.
 * M17: also flushes the delay queue, for a stronger reason than the
 * reorder buffer's own -- vblank_server() only ticks a unit while
 * su_Online is TRUE (just cleared above), so without this, anything
 * still parked in su_DelayQueue at this exact moment would simply never
 * become due again; there is no future tick left to release it. */
static void force_offline(struct Sana2LoopDevice *sld, struct Sana2LoopUnit *unit)
{
    unit->su_Online = FALSE;
    unit->su_WriteCounter = 0;
    fire_event(unit, S2EVENT_OFFLINE);
    reorder_flush(sld, unit);
    delay_queue_flush(sld, unit);
}

/* Ticks su_OfflineAfter's write counter; once it reaches the configured
 * threshold, auto-forces the unit offline (as if a caller had issued
 * S2_OFFLINE itself) and resets the counter. Called on every write this
 * unit's own pipeline accepts as "sent" -- whether or not the packet is
 * later dropped/deduplicated/reordered, since offlineafter counts what
 * the WRITER attempted, not what actually arrived anywhere. The write
 * that crosses the threshold still completes normally; the auto-offline
 * transition takes effect for the NEXT write attempt, not retroactively. */
static void note_write_success(struct Sana2LoopDevice *sld, struct Sana2LoopUnit *unit)
{
    if (unit->su_OfflineAfter == 0)
        return;
    if (++unit->su_WriteCounter >= unit->su_OfflineAfter)
        force_offline(sld, unit);
}

/* Ticks su_OnlineAfter's failure counter (write ATTEMPTS while offline);
 * once it reaches the configured threshold, auto-forces the unit back
 * online and resets the counter -- simulating a flaky link that recovers
 * on its own after enough retries. The attempt that crosses the threshold
 * still fails S2ERR_OUTOFSERVICE itself; recovery takes effect for the
 * NEXT attempt. */
static void note_write_failure(struct Sana2LoopUnit *unit)
{
    if (unit->su_OnlineAfter == 0)
        return;
    if (++unit->su_FailCounter >= unit->su_OnlineAfter)
        force_online(unit);
}

/* Called when a unit's open count drops to zero: nobody is left to read
 * queued data or notice queued waits, so fail/free them rather than leak
 * them across the next open. */
static void drain_unit(struct Sana2LoopUnit *unit)
{
    struct Node *n;
    struct IOSana2Req *io;

    while ((n = safe_rem_head(&unit->su_PendingData)) != NULL)
        FreeMem(n, sizeof(struct EchoPacket));
    unit->su_PendingDataCount = 0;

    while ((io = (struct IOSana2Req *)safe_rem_head(&unit->su_PendingReads)) != NULL) {
        io->ios2_Req.io_Error = IOERR_ABORTED;
        ReplyMsg(&io->ios2_Req.io_Message);
    }
    while ((io = (struct IOSana2Req *)safe_rem_head(&unit->su_OrphanReads)) != NULL) {
        io->ios2_Req.io_Error = IOERR_ABORTED;
        ReplyMsg(&io->ios2_Req.io_Message);
    }
    while ((io = (struct IOSana2Req *)safe_rem_head(&unit->su_EventReqs)) != NULL) {
        io->ios2_Req.io_Error = IOERR_ABORTED;
        ReplyMsg(&io->ios2_Req.io_Message);
    }
}

/* Resets a unit to a known-clean state: called once per unit at device
 * load (via init_device) and again every time a unit's open count
 * transitions 0->1 (see open()) -- the latter is what "config re-read on
 * next open" (M5) means precisely: a busy unit's live config is never
 * disturbed, but a fully-idle one starts fresh for whoever opens it next. */
static void reset_unit(struct Sana2LoopUnit *unit)
{
    int i;

    unit->su_MTU         = SANA2LOOP_DEFAULT_MTU;
    unit->su_BPS         = SANA2LOOP_DEFAULT_BPS;      /* M15 */
    unit->su_MinTU       = 0;                          /* M15: no lower bound */
    unit->su_WireType    = S2WireType_Ethernet;        /* M15 */
    unit->su_AddrSize    = SANA2LOOP_ADDR_ETHERNET;    /* M15 */
    unit->su_Online      = FALSE;
    unit->su_RxQueueMax  = 0; /* strict drop by default -- see docs/PLAN.md M2 */
    /* Free anything still sitting in su_PendingData before zeroing its
     * count, not just the count alone: close()'s own drain_unit() call
     * already does this at last-close, but nothing forces a unit offline
     * at close, so a scheduled-replay unit left online can keep
     * delivering via vblank_server() (which ignores Forbid, unlike a
     * task-context caller) into the idle window between last-close and
     * this reset -- repopulating su_PendingData after drain_unit() already
     * ran. Freeing here too closes that gap regardless of what happened
     * in between, matching this function's own "unit fully resets"
     * contract; a unit that was never idle-and-online this way just finds
     * the list already empty and this is a no-op. */
    {
        struct Node *pn;
        while ((pn = safe_rem_head(&unit->su_PendingData)) != NULL)
            FreeMem(pn, sizeof(struct EchoPacket));
    }
    unit->su_PendingDataCount = 0;
    /* M17: same defensive free (not deliver) for anything still parked in
     * su_DelayQueue -- close()'s own delay_queue_flush() call (mirroring
     * reorder_flush()'s exact placement) should already have emptied this
     * before drain_unit() ran, so this is normally a no-op; kept anyway
     * for the same "costs nothing, keeps this function trustworthy on its
     * own" reasoning as su_PendingData's block above. */
    {
        struct Node *pn;
        while ((pn = safe_rem_head(&unit->su_DelayQueue)) != NULL)
            FreeMem(pn, sizeof(struct EchoPacket));
    }
    unit->su_Configured  = FALSE;
    unit->su_Exclusive   = FALSE; /* M18 */
    unit->su_BuffHooks   = 8; /* M13: always the base hook by default --
                                  identical behavior to every milestone
                                  before this one until `buffhooks=` opts
                                  a unit into actually invoking a wider
                                  negotiated hook */
    unit->su_DMAHooksOffered = 0;

    /* Compiled-in default wiring (docs/PLAN.md M6): units 2 and 3 are a
     * crossover pair, every other unit is standalone echo. A config file
     * (M5/M6) can override either half of this per unit -- read_unit_config()
     * validates crossover mode always ends up with a real, distinct peer;
     * an echo-mode unit's su_Peer defaulting to itself here is what makes
     * "mode=crossover with no peer=" concretely fail that check instead of
     * silently doing something (self-delivery would just be echo again,
     * quietly, which is not what a config author asking for crossover
     * meant). */
    if (unit->su_Index == 2 || unit->su_Index == 3) {
        unit->su_Mode = SANA2LOOP_MODE_CROSSOVER;
        unit->su_Peer = (unit->su_Index == 2) ? 3 : 2;
    } else {
        unit->su_Mode = SANA2LOOP_MODE_ECHO;
        unit->su_Peer = unit->su_Index;
    }

    /* Locally-administered (bit set in the first octet) fake address,
     * spelling "S2L" in the middle bytes so it's recognisable in a packet
     * dump; last byte is the unit number. This is the FACTORY default --
     * su_Station (current) starts equal to it, but S2_CONFIGINTERFACE (M3)
     * may change su_Station away from it; su_FactoryStation itself never
     * changes until the next reset_unit(). */
    unit->su_FactoryStation[0] = 0x02;
    unit->su_FactoryStation[1] = 0x53;
    unit->su_FactoryStation[2] = 0x32;
    unit->su_FactoryStation[3] = 0x4C;
    unit->su_FactoryStation[4] = 0x00;
    unit->su_FactoryStation[5] = unit->su_Index;
    /* Explicit field-by-field, not a loop: a plain fixed-count
     * array-to-array copy loop is exactly the idiom GCC's -O2 loop
     * recognition replaces with a `memmove` call, which doesn't exist in
     * this -nostdlib freestanding build (found the hard way -- linker
     * error in a function that doesn't otherwise call anything struct-copy
     * related; see S2_CONFIGINTERFACE's matching comment below). */
    unit->su_Station[0] = unit->su_FactoryStation[0];
    unit->su_Station[1] = unit->su_FactoryStation[1];
    unit->su_Station[2] = unit->su_FactoryStation[2];
    unit->su_Station[3] = unit->su_FactoryStation[3];
    unit->su_Station[4] = unit->su_FactoryStation[4];
    unit->su_Station[5] = unit->su_FactoryStation[5];

    for (i = 0; i < SANA2LOOP_MAX_TRACKED_TYPES; i++)
        unit->su_TypeInUse[i] = FALSE;

    for (i = 0; i < SANA2LOOP_MAX_MULTICAST; i++)
        unit->su_McastInUse[i] = FALSE;

    /* M7 fault injection: all disabled by default. su_ReorderCount should
     * already be 0 here (close()'s drain_unit() path flushes any buffered
     * packets before a unit can ever reach 0 opens again), but resetting
     * it costs nothing and keeps this function trustworthy on its own. */
    unit->su_Seed         = 1;
    unit->su_PrngState    = 1;
    unit->su_DropPct      = 0;
    unit->su_DupPct       = 0;
    unit->su_Truncate     = 0;
    unit->su_Oversilent   = FALSE;
    unit->su_ReorderWindow = 0;
    unit->su_ReorderCount  = 0;
    unit->su_DelayTicks    = 0;                        /* M17 */
    unit->su_Deviation     = 0;                        /* M17 */
    unit->su_ErrorsOneIn   = 0;                        /* M16 */
    unit->su_LossOneIn     = 0;                        /* M16 */
    unit->su_OfflineAfter  = 0;
    unit->su_OnlineAfter   = 0;
    unit->su_WriteCounter  = 0;
    unit->su_FailCounter   = 0;

    /* M8: free any replay buffer loaded by a PREVIOUS lifetime of this
     * unit (i.e. it was mode=replay, ran, closed to 0, and is now being
     * reset for whoever opens it next -- who may not even ask for replay
     * mode again). open() calls load_replay_file() fresh, after this, only
     * if the new config actually says mode=replay. Freed using
     * unit->su_ReplayCap -- the PREVIOUS lifetime's actual allocation
     * size (M14: config-driven, not always SANA2LOOP_MAX_REPLAY_PACKETS
     * any more) -- which is why this MUST run before su_ReplayCap gets
     * reset to the default below, same free-before-reset ordering
     * su_RecordMax's own matching block already uses. */
    if (unit->su_ReplayPackets) {
        /* Matches load_replay_file()'s actual allocation size, not
         * su_ReplayCount (how many of those slots ended up used) -- see
         * that function's own comment for why it's always the full
         * su_ReplayCap-sized block. */
        FreeMem(unit->su_ReplayPackets, unit->su_ReplayCap * sizeof(struct EchoPacket));
        unit->su_ReplayPackets = NULL;
    }
    if (unit->su_ReplayDueTicks) {
        FreeMem(unit->su_ReplayDueTicks, unit->su_ReplayCap * sizeof(ULONG));
        unit->su_ReplayDueTicks = NULL;
    }
    if (unit->su_ReplayDeliveryCopies) {
        /* Free any copy that was made but never consumed (unit closed
         * before every scheduled record's due tick arrived) -- entries
         * already handed to deliver_packet() are already NULL by then. */
        ULONG i;
        for (i = 0; i < unit->su_ReplayCap; i++)
            if (unit->su_ReplayDeliveryCopies[i])
                FreeMem(unit->su_ReplayDeliveryCopies[i], sizeof(struct EchoPacket));
        FreeMem(unit->su_ReplayDeliveryCopies, unit->su_ReplayCap * sizeof(struct EchoPacket *));
        unit->su_ReplayDeliveryCopies = NULL;
    }
    unit->su_ReplayNext  = NULL;
    unit->su_ReplayCount = 0;
    unit->su_ReplayPos   = 0;
    unit->su_ReplayCap   = SANA2LOOP_MAX_REPLAY_PACKETS;
    unit->su_ReplayMode  = SANA2LOOP_REPLAYMODE_CONSUME;
    unit->su_Tick        = 0;
    unit->su_TickSec     = 0;
    unit->su_TickSub     = 0;

    /* M9 RAM record: free any buffer allocated by a PREVIOUS lifetime,
     * same reasoning as the replay buffer above. */
    if (unit->su_RecordBuf) {
        FreeMem(unit->su_RecordBuf, unit->su_RecordMax);
        unit->su_RecordBuf = NULL;
    }
    unit->su_Recording       = FALSE;
    unit->su_RecordPath[0]   = 0;
    unit->su_RecordMax       = SANA2LOOP_DEFAULT_RECORDMAX;
    unit->su_RecordUsed      = 0;
    unit->su_RecordDropped   = 0;

    unit->su_Stats.PacketsReceived      = 0;
    unit->su_Stats.PacketsSent          = 0;
    unit->su_Stats.BadData              = 0;
    unit->su_Stats.Overruns             = 0;
    unit->su_Stats.Unused               = 0;
    unit->su_Stats.UnknownTypesReceived = 0;
    unit->su_Stats.Reconfigurations     = 0;
    unit->su_Stats.LastStart.tv_secs    = 0;
    unit->su_Stats.LastStart.tv_usec    = 0;
}

/* M8: reads and parses `path` (a dos.library-openable pcap capture) into
 * unit->su_ReplayPackets, one struct EchoPacket per captured Ethernet
 * frame -- the SAME shape CMD_WRITE builds, so CMD_READ/S2_READORPHAN's
 * existing fill_read_from_packet() serves a replayed record with no
 * separate delivery path at all. dst[0:6]/src[6:12] become
 * ep_DstAddr/ep_SrcAddr (zero-padded past 6, same convention every other
 * address field in this file already uses); the on-wire type field
 * (bytes 12:14) is ALWAYS big-endian on the real wire, unaffected by the
 * pcap CONTAINER's own endianness (which only governs the file/record
 * HEADER's numeric fields, decided once here from the magic and reused
 * for every record); everything from byte 14 on becomes ep_Data.
 *
 * Fail-fast on anything that doesn't fit: wrong linktype, a record
 * shorter than a full 14-byte Ethernet header or bigger than this device
 * ever builds (SANA2LOOP_MAX_PACKET), a snaplen-truncated record
 * (incl_len != orig_len -- replaying a shortened frame would be replaying
 * something that never really existed on the wire), more records than
 * this unit's own su_ReplayCap (M14: `replaycap=`, default
 * SANA2LOOP_MAX_REPLAY_PACKETS), or an empty capture (nothing to replay
 * is as much a config mistake as a malformed one here). All open-time
 * failures, per open()'s own comment: unlike a missing CONFIG file
 * (defaults apply), a missing/unreadable REPLAY file has no sensible
 * default to fall back to -- `mode=replay` named a specific source and
 * meant it.
 *
 * __attribute__((noinline)) is load-bearing, not a style choice: this
 * function is only called once (from open()), the classic shape GCC
 * auto-inlines on sight -- and inlined here, it broke the build with an
 * undefined `__mulsi3` reference, found the hard way. struct
 * Sana2LoopUnit isn't power-of-2 sized, so `unit`'s address (derived in
 * open() as `&sld->sld_Units[unitnum]`, unitnum a genuine runtime value)
 * always technically needs a real multiply to compute -- every OTHER
 * sld_Units[] index site in this file (delivery_target(), init_device()'s
 * setup loop) needs exactly the same math. Un-inlined, `unit` just arrives
 * as an ordinary pointer argument already in a register, so nothing here
 * ever recomputes it. Inlined into open()'s much larger merged function
 * body, GCC's register allocator instead decided to rematerialize that
 * pointer from scratch at the point of use (`sld_Units_base +
 * unitnum*sizeof(unit)`) rather than keep it live in a callee-saved
 * register across the whole thing -- and synthesizing a multiply by
 * sizeof(struct Sana2LoopUnit) (an awkward, non-power-of-2, many-bit
 * constant) apparently exceeded whatever shift/add budget this backend
 * allows before falling back to a __mulsi3 call, which this -nostdlib
 * build had no libgcc to satisfy -- at the time. M16 retired that
 * hazard: exactly this failure fired again at pre-existing index sites
 * when the struct grew two more ULONGs, and the fix chosen was to
 * provide our own __mulsi3 (see its definition near the romtag) rather
 * than keep re-shaping innocent code around its absence. The noinline
 * here stays -- it still documents (and avoids re-litigating) a real
 * register-allocator behavior, it just no longer guards the link. */
/* `out_ioerr` is only meaningful when this returns FALSE: set to
 * S2ERR_NO_RESOURCES for a genuine AllocMem() failure (the capture itself
 * was fine; there just wasn't enough free memory to hold `cap` records --
 * matching userdocs/Replay-and-Record.md's/Configuration-Reference.md's
 * own documented `replaycap=` contract, "the device only fails loudly
 * with S2ERR_NO_RESOURCES if a raised value doesn't actually fit"), or
 * S2ERR_BAD_ARGUMENT for every other failure (missing/malformed file,
 * wrong linktype, non-monotonic timestamps, more records than `cap`) --
 * there's no sensible default capture to substitute, so those stay a
 * plain "this open's arguments don't work" rejection. Before this, every
 * failure here was reported as S2ERR_BAD_ARGUMENT regardless of cause,
 * contradicting the documented contract above -- see docs/PLAN.md's
 * pre-launch-review entry for the finding. */
static BOOL __attribute__((noinline)) load_replay_file(struct Sana2LoopUnit *unit, const char *path, ULONG *out_ioerr)
{
    static UBYTE filehdr[PCAP_FILE_HEADER_SIZE];
    static UBYTE rechdr[PCAP_PKTHDR_SIZE];
    static UBYTE framebuf[SANA2LOOP_MAX_PACKET];
    struct Process *proc;
    APTR old_window_ptr;
    BPTR file;
    struct pcap_file_header fh;
    struct EchoPacket *packets;
    ULONG *due_ticks;
    struct EchoPacket **copies = NULL;
    ULONG magic_raw;
    BOOL swapped;
    ULONG count = 0;
    BOOL ok = TRUE;
    BOOL resource_fail = FALSE; /* set alongside ok=FALSE ONLY at the
                                    mid-loop per-record copy AllocMem()
                                    below -- distinguishes that one
                                    genuine resource failure from every
                                    other `ok=FALSE` cause (malformed
                                    record, too many records, non-
                                    monotonic timestamps), which all stay
                                    S2ERR_BAD_ARGUMENT. */
    /* M14: cached from unit->su_ReplayCap (already applied by open()
     * before this call) -- a plain local, not re-read via the pointer on
     * every use, matching this function's existing style elsewhere.
     * `cap * sizeof(struct EchoPacket)` below is now a genuine RUNTIME
     * multiply by a non-power-of-2 constant (SANA2LOOP_MAX_REPLAY_PACKETS
     * was a compile-time constant pre-M14, so this exact multiply used to
     * be compile-time-folded, needing no runtime instruction at all) --
     * the same hazard class this file has hit __mulsi3 link failures from
     * before (see sld_Units[] indexing's own comment). Checked before
     * trusting it, same discipline as every other place this file
     * verifies a compiler-synthesized operation: `make docker` links
     * clean (no libgcc in this build, so a required __mulsi3 would be a
     * hard link error, not a silent gap), and neither `nm` nor `objdump`
     * on the built binary show any mulsi/undefined symbol -- the backend
     * genuinely synthesizes this one via shift/add, unlike
     * sizeof(struct Sana2LoopUnit)'s own larger multiply, which didn't
     * fit that budget. Re-verify the same way if EchoPacket's own size
     * changes. */
    ULONG cap = unit->su_ReplayCap;

    DOSBase = (struct DosLibrary *)OpenLibrary((STRPTR)"dos.library", 0);
    if (!DOSBase) {
        *out_ioerr = S2ERR_BAD_ARGUMENT;
        return FALSE;
    }

    /* Same pr_WindowPtr suppression as read_unit_config() -- see its
     * comment for why an unassigned/removable-volume-looking path must
     * never be allowed to put up a blocking DOS requester here. */
    proc = (struct Process *)FindTask(NULL);
    old_window_ptr = proc->pr_WindowPtr;
    proc->pr_WindowPtr = (APTR)-1L;
    file = Open((STRPTR)path, MODE_OLDFILE);
    proc->pr_WindowPtr = old_window_ptr;

    if (!file) {
        CloseLibrary((struct Library *)DOSBase);
        DOSBase = NULL;
        *out_ioerr = S2ERR_BAD_ARGUMENT;
        return FALSE;
    }

    if (Read(file, filehdr, PCAP_FILE_HEADER_SIZE) != PCAP_FILE_HEADER_SIZE) {
        Close(file);
        CloseLibrary((struct Library *)DOSBase);
        DOSBase = NULL;
        *out_ioerr = S2ERR_BAD_ARGUMENT;
        return FALSE;
    }

    /* Endianness is a property of THIS FILE, decided once from its magic
     * (read here with swapped=FALSE just to get the raw 32 bits out --
     * the comparison below is byte-pattern-vs-byte-pattern, not numeric,
     * so which order pcap_read_u32() itself used to assemble them doesn't
     * matter as long as it's used consistently) -- never this CPU's own
     * byte order (see pcap.h's own comment). */
    magic_raw = pcap_read_u32(filehdr, FALSE);
    if (magic_raw == PCAP_MAGIC_NATIVE) {
        swapped = FALSE;
    } else if (magic_raw == PCAP_MAGIC_SWAPPED) {
        swapped = TRUE;
    } else {
        Close(file);
        CloseLibrary((struct Library *)DOSBase);
        DOSBase = NULL;
        *out_ioerr = S2ERR_BAD_ARGUMENT;
        return FALSE;
    }
    pcap_parse_file_header(filehdr, swapped, &fh);
    if (fh.linktype != PCAP_LINKTYPE_ETHERNET) {
        Close(file);
        CloseLibrary((struct Library *)DOSBase);
        DOSBase = NULL;
        *out_ioerr = S2ERR_BAD_ARGUMENT;
        return FALSE;
    }

    /* Always the full `cap`-slot allocation, not sized to the capture's
     * actual record count -- that count isn't known until the whole file
     * has been read, and a two-pass read (count, then allocate exactly,
     * then re-read) buys nothing a fixed-size allocation doesn't already
     * give this device elsewhere (rxqueue, S2_TRACKTYPE,
     * S2_ADD/DELMULTICASTADDRESS). reset_unit()'s matching FreeMem() must
     * use this same size (via unit->su_ReplayCap, not su_ReplayCount) --
     * it's the actual allocation size, independent of how many slots
     * ended up used.
     *
     * `cap`'s DEFAULT (SANA2LOOP_MAX_REPLAY_PACKETS=4, when no
     * `replaycap=` is given) was picked empirically, not just "generous"
     * -- and had to be re-checked TWICE, both times via a temporary
     * AvailMem(MEMF_LARGEST) probe: M8's original guess (32, ~49KB)
     * reliably failed this exact AllocMem() under bundled AROS
     * specifically (a real ROM in true ROM space doesn't pay this cost,
     * but AROS has to load its own Kickstart replacement INTO this
     * profile's 512K+512K chip+fast budget, leaving measurably less free
     * than real Kickstart 1.3 on the identical profile) -- 16 (~25KB)
     * passed with headroom at M8. Then M9's own per-unit field growth
     * (su_RecordPath[64] alone, plus su_Tick/su_ReplayMode/
     * su_ReplayDueTicks/the recording bookkeeping fields, times
     * SANA2LOOP_NUM_UNITS units) ate enough of that headroom that even 8
     * started failing -- and not just from the fixed per-unit growth:
     * this device's "free memory only at NEXT open, not at close" design
     * (reset_unit()'s own contract) means a SECOND replay-mode unit
     * opened later in the same run has to coexist with the FIRST one's
     * still-held buffer, so the real constraint is two simultaneous
     * cap-sized allocations, not one. Settled on a default of 4 (~6KB
     * each, ~12KB combined), with real margin to spare against a budget
     * that keeps shrinking as this device's per-unit state grows -- and,
     * as of M14, a unit that needs more than the default no longer
     * requires a source change at all: `replaycap=N` opts in per-unit,
     * with the same AvailMem() re-verification discipline applying to
     * whatever value an operator actually configures against their own
     * target's real memory budget, not to this device's own compiled-in
     * default. */
    packets = AllocMem(cap * sizeof(struct EchoPacket), MEMF_CLEAR);
    if (!packets) {
        Close(file);
        CloseLibrary((struct Library *)DOSBase);
        DOSBase = NULL;
        *out_ioerr = S2ERR_NO_RESOURCES;
        return FALSE;
    }
    /* Due ticks: ULONG is power-of-2 sized, so (unlike su_ReplayPackets)
     * indexing this array by a runtime count is safe -- no __mulsi3 risk. */
    due_ticks = AllocMem(cap * sizeof(ULONG), MEMF_CLEAR);
    if (!due_ticks) {
        FreeMem(packets, cap * sizeof(struct EchoPacket));
        Close(file);
        CloseLibrary((struct Library *)DOSBase);
        DOSBase = NULL;
        *out_ioerr = S2ERR_NO_RESOURCES;
        return FALSE;
    }

    /* SCHEDULED mode only: su_ReplayDeliveryCopies' own array, allocated
     * here (ordinary task context) rather than left NULL for
     * vblank_server() to lazily AllocMem() per-record from real interrupt
     * context. This array holds POINTERS -- sizeof(struct EchoPacket *)
     * is power-of-2, so indexing it by a runtime count is a safe shift,
     * unlike su_ReplayPackets itself (see that array's own comment).
     * CONSUME mode leaves `copies` NULL: its reads are served directly
     * from su_ReplayPackets/su_ReplayNext with no ownership transfer and
     * no interrupt involvement, so it needs none of this. */
    if (unit->su_ReplayMode == SANA2LOOP_REPLAYMODE_SCHEDULED) {
        copies = AllocMem(cap * sizeof(struct EchoPacket *), MEMF_CLEAR);
        if (!copies) {
            FreeMem(due_ticks, cap * sizeof(ULONG));
            FreeMem(packets, cap * sizeof(struct EchoPacket));
            Close(file);
            CloseLibrary((struct Library *)DOSBase);
            DOSBase = NULL;
            *out_ioerr = S2ERR_NO_RESOURCES;
            return FALSE;
        }
    }

    /* `cur` walks forward one struct EchoPacket at a time via plain
     * pointer increment (cur++ below) -- deliberately NOT `packets[count]`
     * array indexing. struct EchoPacket isn't power-of-2 sized (its
     * ep_Data[SANA2LOOP_MAX_PACKET] dominates), so indexing it by a
     * RUNTIME variable needs an actual count*sizeof(struct EchoPacket)
     * multiply -- which this -nostdlib -m68000 build has no libgcc to
     * satisfy (found the hard way: linked clean until this function tried
     * exactly that, then failed with an undefined `__mulsi3` reference).
     * `cur++` is just address += a compile-time-constant step, never a
     * multiply -- same reasoning the per-unit su_ReplayNext field this
     * loads into exists for (see its own comment). */
    {
        struct EchoPacket *cur = packets;
        ULONG first_sec = 0, prev_sec = 0;

        for (;;) {
            struct pcap_pkthdr ph;
            LONG got;
            ULONG flen;
            int k;

            got = Read(file, rechdr, PCAP_PKTHDR_SIZE);
            if (got == 0)
                break; /* clean EOF between records -- done */
            if (got != PCAP_PKTHDR_SIZE) {
                ok = FALSE;
                break;
            }
            pcap_parse_pkthdr(rechdr, swapped, &ph);
            flen = ph.incl_len;

            if (ph.incl_len != ph.orig_len || flen < SANA2LOOP_RAW_HDR_SIZE ||
                flen > SANA2LOOP_MAX_PACKET || count >= cap) {
                ok = FALSE;
                break;
            }
            /* M9: timestamps must be non-decreasing -- a well-formed
             * capture's records already appear in the order they were
             * captured, and this device's tick-scheduled replay (M9)
             * needs that to compute each record's due tick as a safe
             * subtraction (ts_sec - first_sec) rather than risk
             * underflowing on an out-of-order or corrupt capture. */
            if (count == 0) {
                first_sec = ph.ts_sec;
                prev_sec = ph.ts_sec;
            } else if (ph.ts_sec < prev_sec) {
                ok = FALSE;
                break;
            } else {
                prev_sec = ph.ts_sec;
            }

            if (Read(file, framebuf, (LONG)flen) != (LONG)flen) {
                ok = FALSE;
                break;
            }

            /* Address fields: same "16-iteration, conditional-value" loop
             * shape do_write() already uses successfully elsewhere in this
             * file -- the ternary defeats the compiler's copy-loop
             * idiom-recognition (it isn't a bare dst[k]=src[k] load-store),
             * unlike the payload copy just below, which IS that shape and
             * so goes through CopyMem instead. */
            for (k = 0; k < SANA2_MAX_ADDR_BYTES; k++) {
                cur->ep_DstAddr[k] = (k < 6) ? framebuf[k] : 0;
                cur->ep_SrcAddr[k] = (k < 6) ? framebuf[6 + k] : 0;
            }
            /* On-wire Ethernet type field: always big-endian on the real
             * wire, independent of this FILE's own container endianness
             * (see this function's header comment). */
            cur->ep_PacketType = ((ULONG)framebuf[12] << 8) | (ULONG)framebuf[13];
            cur->ep_Length = flen - SANA2LOOP_RAW_HDR_SIZE;
            /* CopyMem, not a hand-rolled dst[k]=src[k+C] loop -- exactly
             * the idiom that just broke the `replay=` config-value copy
             * above (see parse_config_line()'s comment); same fix. */
            CopyMem(framebuf + SANA2LOOP_RAW_HDR_SIZE, cur->ep_Data, cur->ep_Length);
            /* SCHEDULED mode: `cur` is now fully populated, so make its
             * delivery copy right here -- ordinary task context, same as
             * everything else in this loop. `copy` is individually
             * AllocMem'd (not a pointer into the `packets` array) because
             * deliver_packet() unconditionally takes ownership of
             * whatever it's given (delivers-then-frees, queues-then-
             * later-frees, or drops-then-frees -- see its own comment),
             * and su_ReplayPackets/su_ReplayNext must survive untouched
             * for CONSUME-mode-style access patterns elsewhere. A failed
             * AllocMem here fails the whole load (fail-fast, same as a
             * malformed record) rather than silently producing a capture
             * with holes in its schedule. */
            if (copies) {
                struct EchoPacket *copy = AllocMem(sizeof(struct EchoPacket), MEMF_CLEAR);
                if (!copy) {
                    ok = FALSE;
                    resource_fail = TRUE;
                    break;
                }
                CopyMem(cur, copy, sizeof(struct EchoPacket));
                copies[count] = copy;
            }
            /* M9: due tick, whole-second granularity -- (ts_sec -
             * first_sec) is a safe subtraction (monotonic check above
             * guarantees first_sec <= ts_sec), and *50 is a small-constant
             * multiply of a runtime value, the same safely-synthesizable
             * shape as M7's proven pct*655 (see SANA2LOOP_VBLANK_HZ's own
             * comment for why this stays whole-second, never touching
             * ts_usec at all, to avoid a genuinely risky
             * runtime-value-vs-non-power-of-2-constant division/multiply). */
            due_ticks[count] = (ph.ts_sec - first_sec) * SANA2LOOP_VBLANK_HZ;
            count++;
            cur++;
        }
    }

    Close(file);
    CloseLibrary((struct Library *)DOSBase);
    DOSBase = NULL;

    if (!ok || count == 0) {
        if (copies) {
            ULONG i;
            for (i = 0; i < count; i++)
                if (copies[i])
                    FreeMem(copies[i], sizeof(struct EchoPacket));
            FreeMem(copies, cap * sizeof(struct EchoPacket *));
        }
        FreeMem(packets, cap * sizeof(struct EchoPacket));
        FreeMem(due_ticks, cap * sizeof(ULONG));
        /* count == 0 (empty capture) is a malformed-input case, not a
         * resource one, even though ok may still be TRUE in that case --
         * only the mid-loop per-record copy AllocMem() failure above
         * (resource_fail) is ever a genuine resource cause here. */
        *out_ioerr = resource_fail ? S2ERR_NO_RESOURCES : S2ERR_BAD_ARGUMENT;
        return FALSE;
    }

    unit->su_ReplayPackets  = packets;
    unit->su_ReplayNext     = packets;
    unit->su_ReplayCount    = count;
    unit->su_ReplayPos      = 0;
    unit->su_ReplayDueTicks = due_ticks;
    unit->su_ReplayDeliveryCopies = copies; /* NULL unless SCHEDULED mode */
    return TRUE;
}

/* M9: one VBlank interrupt server for the whole device (see
 * struct Sana2LoopDevice's own comment), added once in init_device().
 * Ticks su_Tick for any unit that's online AND either SCHEDULED-mode
 * replay or recording (echo/crossover units doing neither pay nothing --
 * the loop still visits them, but the `wants_tick` check short-circuits
 * before touching anything else). Delivering a due SCHEDULED-replay
 * record hands a PRE-ALLOCATED heap copy (su_ReplayDeliveryCopies[pos],
 * made once at load_replay_file() time, ordinary task context) to the
 * EXISTING deliver_packet() precedence (typed read / orphan read /
 * rxqueue / drop+stats) -- per docs/PLAN.md decision #3's stated goal of
 * ONE delivery path shared by echo, crossover, AND replay, this is not a
 * parallel mechanism.
 *
 * This function calls NO allocator of any kind. An earlier version
 * called AllocMem()+CopyMem() right here instead of indexing a
 * pre-made su_ReplayDeliveryCopies[] -- AllocMem() from real interrupt
 * context is long-established as legal on real AmigaOS (Exec's
 * allocator wraps its own list ops in Disable()/Enable(), same
 * discipline this file already uses throughout), and this rewrite was
 * NOT done because that turned out to be false: it stayed legal, and
 * removing it did NOT, on its own, fix the bundled-AROS hang this
 * milestone originally shipped with (see docs/PLAN.md's M9 risk note
 * and docs/sana2-notes.md's M9 section for the real root cause --
 * bundled AROS's CheckIO()/WaitIO() can falsely report an IOSana2Req
 * as already complete immediately after SendIO(), before the device
 * has actually replied it, specifically for requests destined to
 * complete via a genuine hardware-interrupt-driven ReplyMsg() rather
 * than task-context ReplyMsg() -- confirmed by instrumentation and
 * worked around in tests/copperline/sana2test.c, not here). This
 * rewrite is kept anyway as a real, independent improvement: avoiding
 * any allocator call from real interrupt context is more conservative
 * than relying on Disable()/Enable() coverage holding on every target,
 * and costs nothing (the copies are already fully known at
 * load_replay_file() time).
 *
 * Server-type interrupt (VERTB is a shared chain, not an exclusive
 * handler): per the Exec interrupt-server calling convention, entered
 * with A1 = is_Data (this device's own base, set at registration below)
 * and D0/D1/A0/A6 all scratch/unreliable -- reached exactly like this
 * file's open/close/begin_io/abort_io hooks reach their own register
 * parameters, just a different register. Must return with the CPU's Z
 * flag SET (VERTB server convention: "always return with Z set" so the
 * dispatcher keeps walking the rest of the chain) -- returning ULONG 0
 * makes GCC's own epilogue end in a `moveq #0,d0`-shaped return, which
 * leaves Z set; verified this holds via objdump before trusting it (the
 * same verification discipline M1 already established for register
 * annotations on hook calls), since this is exactly the kind of subtle
 * calling-convention detail this project doesn't take on faith. */
static ULONG __attribute__((used)) vblank_server(struct Sana2LoopDevice *sld asm("a1"))
{
    int i;

    for (i = 0; i < SANA2LOOP_NUM_UNITS; i++) {
        struct Sana2LoopUnit *unit = &sld->sld_Units[i];
        BOOL scheduled = (unit->su_Mode == SANA2LOOP_MODE_REPLAY &&
                          unit->su_ReplayMode == SANA2LOOP_REPLAYMODE_SCHEDULED);
        /* M17: delay=/deviation= is the third reason (besides scheduled
         * replay and recording) a unit needs su_Tick advancing -- a
         * config-derived flag, not queue-depth-derived: delay is either
         * on for this whole open session or off for all of it (config is
         * only ever read at the unit's own 0->1 open), so this never
         * misses a drain the way gating on "is the queue empty right
         * now" could. */
        BOOL delaying = (unit->su_DelayTicks > 0 || unit->su_Deviation > 0);

        if (!unit->su_Online || !(scheduled || unit->su_Recording || delaying))
            continue;

        unit->su_Tick++;
        /* Division-free whole-seconds rollover for record_append()'s
         * pcap ts_sec -- see su_TickSec's own field comment for why this
         * is a separate counter from su_Tick itself. */
        if (++unit->su_TickSub >= SANA2LOOP_VBLANK_HZ) {
            unit->su_TickSub = 0;
            unit->su_TickSec++;
        }

        if (scheduled) {
            while (unit->su_ReplayPos < unit->su_ReplayCount &&
                   unit->su_ReplayDueTicks[unit->su_ReplayPos] <= unit->su_Tick) {
                /* Pointer-array indexing (safe -- see su_ReplayDeliveryCopies'
                 * own comment), not AllocMem()+CopyMem() -- see this
                 * function's header comment for why that changed. The
                 * slot is cleared to NULL here: ownership passes to
                 * deliver_packet() (which unconditionally frees or hands
                 * off whatever it's given), so this slot must never be
                 * read again after this point. */
                struct EchoPacket *fresh = unit->su_ReplayDeliveryCopies[unit->su_ReplayPos];
                unit->su_ReplayDeliveryCopies[unit->su_ReplayPos] = NULL;
                if (fresh)
                    deliver_packet(unit, fresh);
                unit->su_ReplayNext++; /* plain pointer step -- see the field's own comment */
                unit->su_ReplayPos++;
            }
        }

        /* M17: release anything in this unit's delay queue that's come
         * due. Safe to call unconditionally here too (an empty list is a
         * cheap no-op), but gating on `delaying` avoids even that check
         * for the two-thirds of units that never enqueue anything --
         * consistent with this loop's existing pay-only-if-configured
         * shape. Uses deliver_packet() (via delay_queue_tick(), see its
         * own comment) -- the SAME real-interrupt-context invocation path
         * M9 already proved legal for scheduled replay above, not a new
         * risk this milestone introduces. */
        if (delaying)
            delay_queue_tick(sld, unit);
    }
    return 0;
}

/*------- init_device ---------------------------------------
CAUTION: This function runs in a forbidden state, single-threaded by Exec.
------------------------------------------------------------*/
static struct Library __attribute__((used)) *
init_device(BPTR seg_list asm("a0"), struct Library *dev asm("d0"))
{
    struct Sana2LoopDevice *sld = (struct Sana2LoopDevice *)dev;
    int i;

    SysBase = *(struct ExecBase **)4UL;

    dev->lib_Node.ln_Type = NT_DEVICE;
    dev->lib_Node.ln_Name = device_name;
    dev->lib_Flags        = LIBF_SUMUSED | LIBF_CHANGED;
    dev->lib_Version      = DEVICE_VERSION;
    dev->lib_Revision     = DEVICE_REVISION;
    dev->lib_IdString     = (APTR)device_id_string;
    dev->lib_OpenCnt      = 0;
    sld->sld_SegList      = seg_list; /* expunge() hands this back to Exec for UnLoadSeg() */

    for (i = 0; i < SANA2LOOP_NUM_UNITS; i++) {
        struct Sana2LoopUnit *u = &sld->sld_Units[i];

        u->su_Unit.unit_OpenCnt = 0;
        u->su_Index = (UBYTE)i;

        /* Lists must be list_init()'d BEFORE reset_unit(): reset_unit()
         * now drains su_PendingData itself (freeing anything left over
         * from a scheduled-replay delivery between last-close and this
         * reset -- see its own comment), and an AllocMem(MEMF_CLEAR)'d
         * struct List (all-NULL) is not a valid empty list under Exec's
         * sentinel-node convention -- RemHead() on it would dereference
         * a NULL node. */
        list_init(&u->su_PendingReads);
        list_init(&u->su_OrphanReads);
        list_init(&u->su_PendingData);
        list_init(&u->su_EventReqs);
        list_init(&u->su_DelayQueue); /* M17 */

        reset_unit(u);
    }

    /* M9: one VBlank server for the device's whole lifetime -- see
     * vblank_server()'s own comment and struct Sana2LoopDevice's field
     * comment for why this lives embedded in the device base rather than
     * a separately-allocated struct. */
    sld->sld_VBlankInt.is_Node.ln_Type = NT_INTERRUPT;
    sld->sld_VBlankInt.is_Node.ln_Pri  = 0;
    sld->sld_VBlankInt.is_Node.ln_Name = device_name;
    sld->sld_VBlankInt.is_Data = sld;
    sld->sld_VBlankInt.is_Code = (void (*)())vblank_server;
    AddIntServer(INTB_VERTB, &sld->sld_VBlankInt);

    return dev;
}

/* device dependent expunge function
 * CAUTION: runs in a forbidden state; guaranteed single-threaded. */
static BPTR __attribute__((used)) expunge(struct Library *dev asm("a6"))
{
    struct Sana2LoopDevice *sld = (struct Sana2LoopDevice *)dev;
    BPTR seg_list;
    int i;

    if (dev->lib_OpenCnt != 0) {
        dev->lib_Flags |= LIBF_DELEXP;
        return 0;
    }
    /* Any unit that ever used mode=replay/record= and was then closed
     * still holds its su_ReplayPackets/su_RecordBuf etc. -- the device's
     * whole "free only at next open" design (see reset_unit()'s own
     * comment) means those pointers are otherwise still live right up to
     * this point. Free them here too, or they become unreachable the
     * instant the device base below is freed -- a real leak, not just
     * "held a bit longer", since nothing will ever point at them again.
     * reset_unit() already has this exact free logic; reusing it here
     * (rather than duplicating it) is safe even though it also resets
     * unit fields that are about to be freed anyway. */
    for (i = 0; i < SANA2LOOP_NUM_UNITS; i++)
        reset_unit(&sld->sld_Units[i]);
    /* Must happen before FreeMem below -- the interrupt struct itself
     * lives inside the device base being freed. */
    RemIntServer(INTB_VERTB, &sld->sld_VBlankInt);
    seg_list = sld->sld_SegList;
    Remove(&dev->lib_Node);
    FreeMem((char *)dev - dev->lib_NegSize, dev->lib_NegSize + dev->lib_PosSize);
    return seg_list;
}

/* device dependent open function
 * CAUTION: runs in a forbidden state; guaranteed single-threaded. */
static void __attribute__((used))
open(struct Library *dev asm("a6"), struct IORequest *io asm("a1"),
     ULONG unitnum asm("d0"), ULONG flags asm("d1"))
{
    struct Sana2LoopDevice *sld = (struct Sana2LoopDevice *)dev;
    struct IOSana2Req *io2 = (struct IOSana2Req *)io;
    struct Sana2LoopUnit *unit;
    struct Sana2LoopOpen *slo;

    io->io_Error = S2ERR_BAD_ARGUMENT;
    io->io_Message.mn_Node.ln_Type = NT_REPLYMSG;

    if (unitnum >= SANA2LOOP_NUM_UNITS)
        return;

    unit = &sld->sld_Units[unitnum];

    /* M18: SANA2OPF_MINE -- exclusive access. Checked against the unit's
     * CURRENT state (before any reset_unit() below, which only runs on
     * a genuine 0->1 transition anyway): a MINE open fails if the unit
     * already has ANY opens (exclusive or not), and ANY open (MINE or
     * not) fails while an existing MINE holder is open. Neither case
     * ever becomes a real open, so this runs before dev->lib_OpenCnt is
     * even bumped below -- no matching decrement needed, unlike every
     * later failure path in this function. IOERR_UNITBUSY is the
     * standard Exec code for exactly this ("device opens ok, but
     * requested unit is busy") -- not an S2ERR_*, since this rejection
     * happens at the Exec OpenDevice() level, before SANA-II's own error
     * space is even reachable (io->io_Error is genuinely an ioerr here,
     * not an S2ERR_ -- see how every OTHER early-return in this function
     * already sets it before the S2ERR_BAD_ARGUMENT default would
     * otherwise apply). */
    if (unit->su_Unit.unit_OpenCnt > 0 &&
        (unit->su_Exclusive || (flags & SANA2OPF_MINE))) {
        io->io_Error = IOERR_UNITBUSY;
        return;
    }

    /* dev->lib_OpenCnt bumped here, BEFORE any dos.library I/O below
     * (read_unit_config()/load_replay_file() can each legitimately
     * Wait() internally -- see read_unit_config()'s own comment on the
     * accepted DOS-I/O-under-broken-Forbid gap this leaves), not at the
     * very end of a successful open as before: with the old placement,
     * dev->lib_OpenCnt stayed at its PRE-open value for this entire DOS
     * I/O window, so a concurrent `Avail FLUSH` on another task (seeing
     * lib_OpenCnt still 0 for what looks like an idle device) could
     * expunge() the device base out from under this open() call, which
     * would then resume inside freed code/data once its DOS Wait
     * returns. Decremented back on every failure path below that returns
     * before a successful open completes -- this call never actually
     * became "an open" if it didn't reach the end. */
    dev->lib_OpenCnt++;

    if (unit->su_Unit.unit_OpenCnt == 0) {
        reset_unit(unit); /* first opener of an idle unit starts clean --
                              see reset_unit()'s comment */

        /* M5 config: only from a real Process (see read_unit_config()'s
         * comment for why) -- a plain Task opener just gets the compiled-in
         * defaults reset_unit() already set, no dos.library touched at
         * all. A file that exists but fails to parse fails the WHOLE open
         * (fail-fast, per docs/PLAN.md) -- reset_unit() has already run,
         * so the unit itself is left in a valid default state regardless;
         * only this caller's open attempt is rejected. */
        if (FindTask(NULL)->tc_Node.ln_Type == NT_PROCESS) {
            struct UnitConfig cfg;
            cfg.mtu          = unit->su_MTU;
            cfg.bps          = unit->su_BPS;          /* M15 */
            cfg.mintu        = unit->su_MinTU;        /* M15 */
            cfg.wire         = unit->su_WireType;     /* M15 */
            cfg.mtu_explicit = FALSE;                 /* M15: per-parse state,
                                                          never persisted */
            cfg.bps_explicit = FALSE;
            cfg.addr_len     = 0;                     /* M16: transient like
                                                          replay_path -- the
                                                          applied address
                                                          lives in
                                                          su_FactoryStation */
            cfg.errors       = unit->su_ErrorsOneIn;  /* M16 */
            cfg.loss         = unit->su_LossOneIn;    /* M16 */
            cfg.rxqueue      = unit->su_RxQueueMax;
            cfg.mode         = unit->su_Mode;
            cfg.peer         = unit->su_Peer;
            cfg.seed         = unit->su_Seed;
            cfg.droppct      = unit->su_DropPct;
            cfg.duppct       = unit->su_DupPct;
            cfg.truncate     = unit->su_Truncate;
            cfg.oversilent   = unit->su_Oversilent;
            cfg.reorder      = unit->su_ReorderWindow;
            cfg.delay        = unit->su_DelayTicks;   /* M17 */
            cfg.deviation    = unit->su_Deviation;    /* M17 */
            cfg.offlineafter = unit->su_OfflineAfter;
            cfg.onlineafter  = unit->su_OnlineAfter;
            cfg.replay_path[0] = 0; /* M8: not persisted on the unit --
                                        transient, see UnitConfig's field
                                        comment -- so always starts empty,
                                        never seeded from prior state */
            cfg.replaymode   = unit->su_ReplayMode;
            cfg.record_path[0] = 0; /* M9: same transient-only contract as
                                        replay_path -- su_RecordPath (the
                                        persisted copy) is set below only
                                        if this comes back non-empty */
            cfg.recordmax    = unit->su_RecordMax;
            cfg.buffhooks    = unit->su_BuffHooks;
            cfg.replaycap    = unit->su_ReplayCap;
            if (!read_unit_config(&cfg, unit->su_Index)) {
                io->io_Error = S2ERR_BAD_ARGUMENT;
                dev->lib_OpenCnt--; /* this attempt never became an open --
                                        see the early increment's own comment */
                return;
            }
            unit->su_MTU          = cfg.mtu;
            unit->su_BPS          = cfg.bps;          /* M15 */
            unit->su_MinTU        = cfg.mintu;        /* M15 */
            unit->su_WireType     = cfg.wire;         /* M15 */
            /* M15: a point-to-point wire narrows the station address to 4
             * bytes and re-derives the factory address in the same
             * recognisable 0x?? 'S' '2' <unit> shape reset_unit() uses for
             * Ethernet -- but led by 0x0A (a private 10.x.x.x IP octet)
             * instead of the locally-administered-MAC 0x02, since for
             * these wires the "hardware address" is an IP by the same
             * convention nullsana established. Field-by-field, no loop --
             * see reset_unit()'s memmove-miscompile comment. Bytes past
             * su_AddrSize stay whatever reset_unit() put there; nothing on
             * this unit reads past su_AddrSize again until the next
             * reset. */
            if (cfg.wire != S2WireType_Ethernet) {
                unit->su_AddrSize = SANA2LOOP_ADDR_P2P;
                unit->su_FactoryStation[0] = 0x0A;
                unit->su_FactoryStation[1] = 0x53;
                unit->su_FactoryStation[2] = 0x32;
                unit->su_FactoryStation[3] = unit->su_Index;
                unit->su_Station[0] = unit->su_FactoryStation[0];
                unit->su_Station[1] = unit->su_FactoryStation[1];
                unit->su_Station[2] = unit->su_FactoryStation[2];
                unit->su_Station[3] = unit->su_FactoryStation[3];
            }
            /* M16 `addr=`: an operator-pinned address replaces the derived
             * factory address entirely -- BOTH factory and current, so
             * S2_GETSTATIONADDRESS reports it in both slots and
             * S2_CONFIGINTERFACE's one-shot override semantics (M3) work
             * on top of it unchanged. Deliberately AFTER the wire block
             * above, which would otherwise clobber it for a
             * point-to-point unit. read_unit_config() already guaranteed
             * addr_len matches this wire's su_AddrSize exactly. CopyMem
             * (an Exec LVO), not a variable-count copy loop -- the same
             * memcpy-miscompile hazard replay_path's own parse comment
             * documents. */
            if (cfg.addr_len != 0) {
                CopyMem(cfg.addr, unit->su_FactoryStation, cfg.addr_len);
                CopyMem(cfg.addr, unit->su_Station, cfg.addr_len);
            }
            unit->su_ErrorsOneIn  = cfg.errors;       /* M16 */
            unit->su_LossOneIn    = cfg.loss;         /* M16 */
            unit->su_RxQueueMax   = cfg.rxqueue;
            unit->su_Mode         = cfg.mode;
            unit->su_Peer         = cfg.peer;
            unit->su_Seed         = cfg.seed;
            /* PRNG re-seeded fresh at every 0->1 transition, same
             * determinism boundary as everything else reset_unit()
             * covers -- 0 coerced to 1 (docs/PLAN.md), since xorshift32
             * with a zero seed never leaves the zero state. */
            unit->su_PrngState    = cfg.seed ? cfg.seed : 1;
            unit->su_DropPct      = cfg.droppct;
            unit->su_DupPct       = cfg.duppct;
            unit->su_Truncate     = cfg.truncate;
            unit->su_Oversilent   = cfg.oversilent ? TRUE : FALSE;
            unit->su_ReorderWindow = cfg.reorder;
            unit->su_DelayTicks    = cfg.delay;       /* M17 */
            unit->su_Deviation     = cfg.deviation;   /* M17 */
            unit->su_OfflineAfter = cfg.offlineafter;
            unit->su_OnlineAfter  = cfg.onlineafter;
            unit->su_BuffHooks    = cfg.buffhooks;

            /* M9: applied BEFORE load_replay_file() below (not after, as
             * an earlier version had it) specifically so that function
             * can see the real su_ReplayMode and decide whether to
             * pre-allocate SCHEDULED mode's per-record delivery copies --
             * see su_ReplayDeliveryCopies' own field comment for why that
             * matters (avoiding AllocMem() from inside vblank_server()'s
             * real interrupt context). */
            unit->su_ReplayMode = cfg.replaymode;
            /* M14: applied BEFORE load_replay_file() below for the same
             * reason su_ReplayMode is -- that function AllocMem()s against
             * unit->su_ReplayCap directly. */
            unit->su_ReplayCap = cfg.replaycap;

            /* M8: mode=replay's source is loaded once, right here -- same
             * 0->1 determinism boundary as everything else this block
             * applies. read_unit_config() already guaranteed replay_path
             * is non-empty whenever cfg.mode is SANA2LOOP_MODE_REPLAY. A
             * load failure fails the WHOLE open -- unlike a missing
             * config file, there's no sensible default to fall back to
             * for a specifically-named, missing/malformed capture. The
             * actual io_Error is load_replay_file()'s own choice now (M14
             * follow-up): S2ERR_NO_RESOURCES for a genuine AllocMem()
             * failure (the capture was fine; there just wasn't enough
             * free memory for `cap` records), S2ERR_BAD_ARGUMENT for
             * everything else -- see that function's own out_ioerr
             * comment and userdocs/Configuration-Reference.md's
             * `replaycap=` row, which already documented this exact
             * distinction. */
            if (unit->su_Mode == SANA2LOOP_MODE_REPLAY) {
                ULONG replay_ioerr = S2ERR_BAD_ARGUMENT;
                if (!load_replay_file(unit, cfg.replay_path, &replay_ioerr)) {
                    io->io_Error = replay_ioerr;
                    dev->lib_OpenCnt--; /* see the early increment's own comment */
                    return;
                }
            }

            /* M9: `record=<path>` -- allocate the RAM buffer and write its
             * pcap file header immediately (so record_append() below only
             * ever needs to append records, never special-case "is this
             * the first packet"). su_RecordMax must be applied BEFORE this
             * check (it's the allocation size), independent of whether
             * recording actually turns out to be enabled -- read_unit_config()
             * accepts `recordmax=` regardless of whether `record=` is also
             * given, same as every other independently-parsed key. */
            unit->su_RecordMax = cfg.recordmax;
            if (cfg.record_path[0] != 0) {
                unit->su_RecordBuf = AllocMem(unit->su_RecordMax, MEMF_CLEAR);
                if (!unit->su_RecordBuf) {
                    io->io_Error = S2ERR_NO_RESOURCES;
                    dev->lib_OpenCnt--; /* see the early increment's own comment */
                    return;
                }
                CopyMem(cfg.record_path, unit->su_RecordPath, sizeof(unit->su_RecordPath));
                pcap_build_file_header(unit->su_RecordBuf, SANA2LOOP_MAX_PACKET, PCAP_LINKTYPE_ETHERNET);
                unit->su_RecordUsed = PCAP_FILE_HEADER_SIZE;
                unit->su_Recording = TRUE;
            }
        }
    }

    slo = AllocMem(sizeof(struct Sana2LoopOpen), MEMF_CLEAR);
    if (!slo) {
        io->io_Error = S2ERR_NO_RESOURCES;
        dev->lib_OpenCnt--; /* see the early increment's own comment */
        return;
    }
    slo->slo_Unit  = unit;
    slo->slo_Magic = SANA2LOOP_OPEN_MAGIC;
    /* ios2_BufferManagement arrives pointing at the caller's tag list (or
     * NULL); apply_buffer_mgmt_tags() reads it before we overwrite the
     * field below with our own cookie. */
    apply_buffer_mgmt_tags(slo, (struct TagItem *)io2->ios2_BufferManagement);
    /* M13: count this open as a DMA-hook negotiation if it offered ANY of
     * the four Rev 7 DMA hooks -- surfaced via S2_GETSPECIALSTATS. Not
     * gated on the 0->1 open-count check above: unlike unit-level config,
     * this is per-open negotiation, tallied across the unit's whole
     * current session (same "accumulate until reset_unit()" pattern as
     * su_Stats). */
    if (slo->slo_DMACopyToBuff32 || slo->slo_DMACopyFromBuff32 ||
        slo->slo_DMACopyToBuff64 || slo->slo_DMACopyFromBuff64)
        unit->su_DMAHooksOffered++;
    /* SANA2OPF_PROM: irrelevant in echo mode (every opener already sees
     * everything on its unit, unchanged since Phase 1), but in crossover
     * mode (M6) lets THIS open's queued reads accept a packet regardless
     * of destination address -- see dest_matches_unit(). */
    slo->slo_Promiscuous = (flags & SANA2OPF_PROM) ? TRUE : FALSE;
    /* M18: SANA2OPF_MINE -- the busy-check above already rejected this
     * open outright if it couldn't have exclusive access; reaching here
     * means it genuinely gets it. */
    if (flags & SANA2OPF_MINE)
        unit->su_Exclusive = TRUE;

    io->io_Device = (struct Device *)dev;
    io->io_Unit   = &unit->su_Unit;
    io->io_Unit->unit_OpenCnt++;
    /* dev->lib_OpenCnt itself was already bumped at the top of this
     * function, before any dos.library I/O -- see that increment's own
     * comment. Nothing to do here for it. */
    /* Standard Exec convention: a successful open cancels a pending
     * delayed expunge. Without this, once something (e.g. an `Avail
     * FLUSH` racing an earlier open/close window) has set LIBF_DELEXP,
     * the flag persists across any number of later successful opens, and
     * the device self-expunges at the next transient open-count-zero --
     * surprising for open/close-cycling consumers like the documented
     * SanaDump/SanaSend `Run` pattern (see userdocs/SanaDump-and-SanaSend.md). */
    dev->lib_Flags &= ~LIBF_DELEXP;
    io2->ios2_BufferManagement = slo; /* per SANA-II: the opener must copy
                                          this into every future request */
    io->io_Error = 0;
}

/* M9: flushes unit->su_RecordBuf (su_RecordUsed bytes -- the pcap file
 * header plus every appended record) to su_RecordPath, at last close,
 * only if the closer is a real Process (dos.library needs process-level
 * context, same reasoning as read_unit_config()/load_replay_file()).
 * Best-effort: a Task closer, or any failure opening the output file,
 * silently loses the recording rather than failing CloseDevice() itself
 * -- CloseDevice() has no error-reporting channel back to the caller
 * (Exec's own contract), so there is nothing else a failure here could
 * usefully do. */
static void flush_record(struct Sana2LoopUnit *unit)
{
    struct Process *proc;
    APTR old_window_ptr;
    BPTR file;

    if (!unit->su_Recording || FindTask(NULL)->tc_Node.ln_Type != NT_PROCESS)
        return;

    DOSBase = (struct DosLibrary *)OpenLibrary((STRPTR)"dos.library", 0);
    if (!DOSBase)
        return;

    proc = (struct Process *)FindTask(NULL);
    old_window_ptr = proc->pr_WindowPtr;
    proc->pr_WindowPtr = (APTR)-1L;
    file = Open((STRPTR)unit->su_RecordPath, MODE_NEWFILE);
    proc->pr_WindowPtr = old_window_ptr;

    if (file) {
        Write(file, unit->su_RecordBuf, (LONG)unit->su_RecordUsed);
        Close(file);
    }

    CloseLibrary((struct Library *)DOSBase);
    DOSBase = NULL;
}

/* device dependent close function
 * CAUTION: runs in a forbidden state; guaranteed single-threaded. */
static BPTR __attribute__((used))
close(struct Library *dev asm("a6"), struct IORequest *io asm("a1"))
{
    struct Sana2LoopUnit *unit = (struct Sana2LoopUnit *)io->io_Unit;
    struct IOSana2Req *io2 = (struct IOSana2Req *)io;
    struct Sana2LoopOpen *slo = (struct Sana2LoopOpen *)io2->ios2_BufferManagement;

    abort_requests_for_open(unit, slo);

    if (--unit->su_Unit.unit_OpenCnt == 0) {
        /* M7's reorder_flush() runs BEFORE drain_unit(), not after: for
         * an ECHO unit (or any crossover pair somehow peered to itself),
         * reorder_flush() delivers straight back into THIS SAME unit's
         * own su_PendingData if rxqueue>0 -- draining first and flushing
         * second would silently repopulate the very queue drain_unit()
         * just emptied, leaving packets sitting there with no reader left
         * to ever collect them (nobody's open anymore) until the unit's
         * next open reclaims them via reset_unit()'s own drain. Flushing
         * first means drain_unit() sweeps up anything reorder_flush() just
         * queued back onto this same unit, matching "last-close leaves
         * nothing queued behind" for the common (non-crossover) case; a
         * genuine crossover peer delivery is a DIFFERENT unit's queue
         * either way, so the order doesn't affect that case at all. */
        reorder_flush((struct Sana2LoopDevice *)dev, unit);
        /* M17: same reasoning and same BEFORE-drain_unit() ordering as
         * reorder_flush() just above -- delay_queue_flush() can also
         * deliver straight back into this same unit's own su_PendingData
         * (echo mode, or a self-peered crossover), and drain_unit() must
         * run last so it sweeps up whatever either flush just queued. */
        delay_queue_flush((struct Sana2LoopDevice *)dev, unit);
        drain_unit(unit);
        /* M9: same last-close boundary flushes any RAM-recorded traffic
         * to its named output file. */
        flush_record(unit);
    }

    FreeMem(slo, sizeof(struct Sana2LoopOpen));
    io2->ios2_BufferManagement = NULL;
    io->io_Device = NULL;
    io->io_Unit   = NULL;
    dev->lib_OpenCnt--;

    if (dev->lib_OpenCnt == 0 && (dev->lib_Flags & LIBF_DELEXP))
        return expunge(dev);

    return 0;
}

/* Does `dst` make a packet deliverable to `unit` at all? Only consulted
 * for CROSSOVER-mode delivery (echo mode's reflexive self-delivery never
 * filters by destination -- see deliver_packet()'s own comment): standard
 * 802.3 receive filtering -- unicast to this unit's own current station
 * address, the all-1s broadcast address, or a multicast group this unit
 * has joined (M4's su_McastInUse/su_McastAddr table) -- OR the specific
 * candidate reader is promiscuous (`prom`, from that request's own open
 * cookie), which accepts a packet regardless of its destination.
 * Must be called under Disable() (it touches su_McastInUse/su_McastAddr,
 * the same shared table find_mcast_locked() already requires that of). */
static BOOL dest_matches_unit(struct Sana2LoopUnit *unit, const UBYTE *dst, BOOL prom)
{
    int k;
    BOOL station_match = TRUE, all_ff = TRUE;

    if (prom)
        return TRUE;

    /* M15: the station compare spans exactly this unit's own address
     * width; broadcast/multicast are Ethernet-only concepts, so a
     * point-to-point unit accepts only its own address (or a promiscuous
     * reader, above). */
    for (k = 0; k < (int)unit->su_AddrSize; k++) {
        if (dst[k] != unit->su_Station[k])
            station_match = FALSE;
        if (dst[k] != 0xFF)
            all_ff = FALSE;
    }
    if (station_match)
        return TRUE;
    if (unit->su_WireType != S2WireType_Ethernet)
        return FALSE;
    if (all_ff)
        return TRUE;

    if ((dst[0] & 0x01) && find_mcast_locked(unit, dst) >= 0)
        return TRUE;

    return FALSE;
}

/* M9: appends `pkt`, pcap-formatted, into unit->su_RecordBuf if
 * `record=<path>` is active for this unit -- a no-op otherwise. Called
 * from deliver_packet() unconditionally (whether the packet ends up
 * delivered, queued, or dropped -- a real capture point sees traffic
 * regardless of whether a consumer was there to read it, matching how
 * su_Stats.PacketsReceived already counts unconditionally too).
 *
 * Timestamps are su_TickSec (whole "virtual seconds", ts_usec always
 * 0) -- NOT real wall-clock time; see SANA2LOOP_VBLANK_HZ's own comment
 * for why this device only ever deals in tick-granularity time. Once
 * su_RecordMax would be exceeded, further packets are silently NOT
 * appended -- su_RecordDropped counts how many, surfaced via
 * S2_GETSPECIALSTATS per docs/PLAN.md decision #7 ("overflow = stop +
 * count") -- whatever was already captured stays intact, never truncated
 * or corrupted to make room. */
static void record_append(struct Sana2LoopUnit *unit, struct EchoPacket *pkt)
{
    ULONG frame_len, need;
    UBYTE *p;

    if (!unit->su_Recording)
        return;

    frame_len = SANA2LOOP_RAW_HDR_SIZE + pkt->ep_Length;
    need = PCAP_PKTHDR_SIZE + frame_len;
    if (unit->su_RecordUsed + need > unit->su_RecordMax) {
        unit->su_RecordDropped++;
        return;
    }

    p = unit->su_RecordBuf + unit->su_RecordUsed;
    pcap_build_pkthdr(p, unit->su_TickSec, 0, frame_len, frame_len);
    CopyMem(pkt->ep_DstAddr, p + PCAP_PKTHDR_SIZE, 6);
    CopyMem(pkt->ep_SrcAddr, p + PCAP_PKTHDR_SIZE + 6, 6);
    pcap_write_u16(p + PCAP_PKTHDR_SIZE + 12, (UWORD)pkt->ep_PacketType);
    CopyMem(pkt->ep_Data, p + PCAP_PKTHDR_SIZE + SANA2LOOP_RAW_HDR_SIZE, pkt->ep_Length);

    unit->su_RecordUsed += need;
}

/* Delivers one packet to `unit` (the receiving side -- for echo that's the
 * same unit the write happened on; crossover mode (M6) calls this on the
 * PEER unit instead, which is the whole reason this is its own function
 * rather than inlined into CMD_WRITE).
 *
 * SANA-II precedence, strictly: a pending CMD_READ queued for this exact
 * ios2_PacketType, else a pending S2_READORPHAN (any type), else -- only
 * if the unit's rxqueue allows it -- a bounded pending-data queue, else
 * drop. Real SANA-II drivers hold no internal buffers; rxqueue defaults to
 * 0 (strict drop) for that reason, per the proposal's teaching claim --
 * see docs/PLAN.md's M2 entry for why a nonzero default was considered and
 * rejected. `pkt` is always consumed by this call: delivered, queued, or
 * freed, never left dangling.
 *
 * For a CROSSOVER-mode `unit`, every candidate (typed read, orphan read,
 * or the rxqueue itself) must also pass dest_matches_unit() -- a real
 * link has two independently-addressed ends, so "does this frame's
 * destination actually belong to me" is a real question here, unlike echo
 * mode (still fully unfiltered -- there is no separate peer to have
 * misaddressed the packet at, so filtering would only get in the way of
 * the simplest test scenario, per the original Phase 1 design intent).
 * rxqueue never gets promiscuous treatment (`prom=FALSE` always at that
 * check): it's a shared, unit-wide buffer for a not-yet-known future
 * reader, not a specific open's own queued request, so there is no
 * meaningful "this reader is promiscuous" to consult at queue time.
 *
 * Must never be called with a Disable() already held by the caller: it
 * calls consumer hooks and ReplyMsg, neither of which may run disabled. */
/* M18: TRUE if `list` (a private, not-unit-shared collection being built
 * by deliver_packet()'s own typed-candidate walk below) already holds a
 * node whose open matches `slo` -- enforces "at most one candidate per
 * open per packet": a packet must fan out to every DISTINCT open with a
 * matching read, not drain one open's own pipelined read queue with N
 * identical fills. `list` is never touched by anything outside the
 * current deliver_packet() call, so this needs no Disable() of its own. */
static BOOL open_already_collected(struct List *list, struct Sana2LoopOpen *slo)
{
    struct IOSana2Req *io;
    for (io = (struct IOSana2Req *)list->lh_Head;
         io->ios2_Req.io_Message.mn_Node.ln_Succ;
         io = (struct IOSana2Req *)io->ios2_Req.io_Message.mn_Node.ln_Succ) {
        if ((struct Sana2LoopOpen *)io->ios2_BufferManagement == slo)
            return TRUE;
    }
    return FALSE;
}

/* M18: attempts to deliver `pkt` to ONE candidate read that's already
 * been dequeued (Remove()d, by the caller, under Disable()) from
 * whichever list it came from. Runs that candidate's own S2_PacketFilter
 * (if negotiated) and, on acceptance, its copy hook; on rejection, puts
 * the read back onto `requeue_list` (the SAME unit list it was dequeued
 * from -- su_PendingReads for a typed candidate, su_OrphanReads for an
 * orphan one) so it's still queued for a FUTURE packet, at the front
 * (AddHead) so it isn't shuffled behind anything that queued while the
 * filter ran (exact relative order among several requeued-in-one-call
 * candidates isn't preserved beyond that -- SANA-II doesn't promise FIFO
 * ordering across queued reads either, so this doesn't matter). Returns
 * TRUE if `pkt` was consumed by this candidate: either a real delivery,
 * or the same bad-SANA2LOOP_OPEN_MAGIC failure fill_read_from_packet()
 * always treated as "claims the packet, fails its own read" (preserved
 * unchanged from pre-M18 behavior -- see prefill_read_metadata()'s own
 * comment).
 *
 * Must be called OUTSIDE Disable() -- calls a consumer-supplied filter
 * hook and ReplyMsg(), neither legal disabled. Interrupt-context callers
 * (M9 scheduled replay, M17 delay-queue release) inherit the identical
 * contract S2_CopyToBuff already has there -- see docs/sana2-notes.md's
 * M18 section. */
static BOOL try_deliver_candidate(struct IOSana2Req *io, struct EchoPacket *pkt, struct List *requeue_list)
{
    struct Sana2LoopOpen *slo = prefill_read_metadata(io, pkt);

    if (!slo) {
        io->ios2_Req.io_Error = S2ERR_NO_RESOURCES;
        io->ios2_WireError = S2WERR_BUFF_ERROR;
        if (!(io->ios2_Req.io_Flags & IOF_QUICK))
            ReplyMsg(&io->ios2_Req.io_Message);
        return TRUE;
    }

    if (!call_packet_filter(slo, io, pkt)) {
        Disable();
        AddHead(requeue_list, (struct Node *)io);
        Enable();
        return FALSE;
    }

    if (pick_copy_to_buff(slo->slo_Unit, slo)(io->ios2_Data, pkt->ep_Data, pkt->ep_Length)) {
        io->ios2_Req.io_Error = 0;
    } else {
        io->ios2_Req.io_Error = S2ERR_NO_RESOURCES;
        io->ios2_WireError = S2WERR_BUFF_ERROR;
    }
    if (!(io->ios2_Req.io_Flags & IOF_QUICK))
        ReplyMsg(&io->ios2_Req.io_Message);
    return TRUE;
}

/* M18: multi-open fan-out (GitHub #25, nullsana-inspired -- "open each
 * unit as many times as you want and monitor all data"). TYPED reads
 * (CMD_READ) fan out to every DISTINCT open with a matching queued read
 * (one fill per open, per open_already_collected()'s own contract).
 * S2_READORPHAN stays a GLOBAL, single-delivery fallback for packet
 * types no typed reader anywhere claimed -- deliberately NOT fanned out:
 * the SANA-II spec defines an orphan read as the unclaimed-type
 * catch-all, not a general sniffing tool, and preserving that is what
 * lets the pre-M18 "delivery" sub-test's own typed-beats-orphan
 * assertion keep passing unchanged (see docs/sana2-notes.md's M18
 * section for the full reasoning -- this was a real design fork, not an
 * arbitrary choice). rxqueue and drop+stats are consulted only once
 * NEITHER typed nor orphan claimed the packet, same "else" position in
 * the precedence chain as before M18.
 *
 * AbortIO() race note: a candidate is briefly off every unit-shared list
 * while try_deliver_candidate() runs its filter/copy outside Disable().
 * A concurrent AbortIO() landing in that exact window finds nothing on
 * any list and returns -1 ("already in-flight" -- abort_io()'s own
 * pre-existing, documented contract, unchanged by M18): the caller's
 * WaitIO() either sees the real completion (filter accepted) or the
 * request is back on its queue moments later (filter rejected), where a
 * retried AbortIO() finds it normally. This is the same class of race
 * abort_io()'s own "-1: let WaitIO see it naturally" comment already
 * accepted pre-M18 for a read already Remove()'d and mid-ReplyMsg() --
 * M18 widens the window (a filter hook can run arbitrarily long) but
 * doesn't introduce a new FAILURE mode, only a longer instance of an
 * already-accepted one. Considered and deliberately NOT adding new
 * per-request transient-state tracking (an io_Flags bit, or a 4th
 * abort_requests_for_open()-searched list) for this: SANA2IOF_* is
 * spec-owned and SendIO()/DoIO() already clear it wholesale before every
 * dispatch (see M4's own finding), making it an unsafe place to stash
 * device-private state; a transit list would add exactly the kind of
 * extra list-walk surface the pre-launch review's own concurrency fixes
 * were about REDUCING, for a race window this device's own delivery
 * model (synchronous, task-context, not deferred past the write's own
 * do_io()) makes unreachable from any test that follows this file's
 * existing write-then-check sequencing. */
static void deliver_packet(struct Sana2LoopUnit *unit, struct EchoPacket *pkt)
{
    BOOL crossover = (unit->su_Mode == SANA2LOOP_MODE_CROSSOVER);
    BOOL delivered = FALSE;
    BOOL queued = FALSE;
    struct List typed_candidates;
    struct IOSana2Req *io, *next;

    list_init(&typed_candidates);

    Disable();
    unit->su_Stats.PacketsReceived++;
    record_append(unit, pkt); /* M9: no-op unless record=<path> is active */
    for (io = (struct IOSana2Req *)unit->su_PendingReads.lh_Head;
         io->ios2_Req.io_Message.mn_Node.ln_Succ;
         io = next) {
        next = (struct IOSana2Req *)io->ios2_Req.io_Message.mn_Node.ln_Succ;
        if (io->ios2_PacketType == pkt->ep_PacketType &&
            (!crossover || dest_matches_unit(unit, pkt->ep_DstAddr,
                ((struct Sana2LoopOpen *)io->ios2_BufferManagement)->slo_Promiscuous)) &&
            !open_already_collected(&typed_candidates, (struct Sana2LoopOpen *)io->ios2_BufferManagement)) {
            Remove((struct Node *)io);
            AddTail(&typed_candidates, (struct Node *)io);
        }
    }
    Enable();

    {
        struct Node *n;
        while ((n = RemHead(&typed_candidates)) != NULL)
            if (try_deliver_candidate((struct IOSana2Req *)n, pkt, &unit->su_PendingReads))
                delivered = TRUE;
    }

    if (!delivered) {
        struct List orphan_candidates;
        struct Node *n;

        list_init(&orphan_candidates);

        Disable();
        for (io = (struct IOSana2Req *)unit->su_OrphanReads.lh_Head;
             io->ios2_Req.io_Message.mn_Node.ln_Succ;
             io = next) {
            next = (struct IOSana2Req *)io->ios2_Req.io_Message.mn_Node.ln_Succ;
            if (!crossover || dest_matches_unit(unit, pkt->ep_DstAddr,
                    ((struct Sana2LoopOpen *)io->ios2_BufferManagement)->slo_Promiscuous)) {
                Remove((struct Node *)io);
                AddTail(&orphan_candidates, (struct Node *)io);
            }
        }
        Enable();

        /* Try each candidate in turn (a rejected one is requeued by
         * try_deliver_candidate() itself and the search moves on) until
         * one accepts -- single delivery overall, per this function's
         * own header comment. */
        while (!delivered && (n = RemHead(&orphan_candidates)) != NULL)
            if (try_deliver_candidate((struct IOSana2Req *)n, pkt, &unit->su_OrphanReads))
                delivered = TRUE;

        /* Anything left in orphan_candidates was never even tried
         * (we stopped once one candidate accepted) -- put it back
         * exactly as untouched as a not-yet-considered candidate should
         * be. */
        while ((n = RemHead(&orphan_candidates)) != NULL) {
            Disable();
            AddHead(&unit->su_OrphanReads, n);
            Enable();
        }
    }

    Disable();
    if (!delivered && unit->su_PendingDataCount < unit->su_RxQueueMax &&
        (!crossover || dest_matches_unit(unit, pkt->ep_DstAddr, FALSE))) {
        AddTail(&unit->su_PendingData, (struct Node *)pkt);
        unit->su_PendingDataCount++;
        queued = TRUE;
    }
    if (!delivered && !queued)
        unit->su_Stats.UnknownTypesReceived++;

    /* S2_TRACKTYPE accounting: PacketsReceived/BytesReceived count the
     * packet arriving at this interface at all (delivered to one or more
     * opens OR accepted into the rxqueue for later reading) -- exactly
     * ONCE per wire-packet regardless of how many opens got a fill (M18:
     * stats describe what arrived at the interface, matching what these
     * numbers mean on a real driver, not a per-listener multiplier).
     * PacketsDropped is reserved for the one outcome that's genuinely a
     * loss: nobody -- typed, orphan, or rxqueue -- wanted it. */
    {
        struct Sana2PacketTypeStats *ts = find_tracked_locked(unit, pkt->ep_PacketType);
        if (ts) {
            if (delivered || queued) {
                ts->PacketsReceived++;
                ts->BytesReceived += pkt->ep_Length;
            } else {
                ts->PacketsDropped++;
            }
        }
    }
    Enable();

    /* M18: every delivered candidate copied OUT of pkt via its own copy
     * hook (fill_read_from_packet()'s pre-M18 contract, unchanged) --
     * nothing ever retains pkt itself except the rxqueue path, so one
     * free covers every outcome except "queued". */
    if (!queued)
        FreeMem(pkt, sizeof(struct EchoPacket));
}

/* Shared body for CMD_WRITE and its S2_BROADCAST/S2_MULTICAST cousins --
 * all three ultimately hand one packet to deliver_packet(). `is_plain_write`
 * distinguishes CMD_WRITE (TRUE) from S2_BROADCAST/S2_MULTICAST (FALSE);
 * combined with `unit`'s own mode, that decides addressing:
 *  - Echo mode's plain CMD_WRITE (the only case that swaps): unchanged
 *    since Phase 1 -- there's no real peer to ask, so the reader on this
 *    SAME unit is told it's the recipient (this unit's own station
 *    address) and that whatever address it wrote to is the "sender".
 *  - Every other case (crossover mode of ANY write type; echo's own
 *    S2_BROADCAST/S2_MULTICAST) uses real, unswapped addressing: src is
 *    this unit's own actual station address (a real driver never lets a
 *    caller fake their own source address -- fixing a latent bug from
 *    M4, whose src=dst-of-write shortcut was never caught because M4's
 *    own broadcast/multicast tests only ever checked the delivered dst),
 *    dst is exactly what the caller specified (S2_BROADCAST already
 *    forced it to all-1s before do_write() was ever called).
 *
 * `sld` is needed only to reach a crossover peer's struct
 * Sana2LoopUnit -- echo-mode writes never touch it.
 *
 * M7 fault injection pipeline, fixed order (docs/PLAN.md): MTU (own, then
 * -- crossover only -- the peer's, where `oversilent` matters) -> droppct
 * -> truncate -> duppct -> reorder. su_OfflineAfter is a side effect of
 * every successful write, not really "part of" the pipeline's shaping of
 * this one packet -- ticked once at the very end via note_write_success(),
 * after the packet (and its duplicate, if any) have already been handed
 * off. All of it defaults to off (reset_unit()): a plain write with no
 * config behaves exactly as it did before this milestone. */
static void do_write(struct Sana2LoopDevice *sld, struct Sana2LoopUnit *unit,
                      struct IORequest *io0, struct IOSana2Req *io, BOOL is_plain_write)
{
    struct EchoPacket *pkt;
    struct Sana2LoopUnit *target;
    struct Sana2LoopOpen *writer_slo = (struct Sana2LoopOpen *)io->ios2_BufferManagement;
    ULONG max_len = unit->su_MTU;
    BOOL swap_to_self;
    int k;

    /* Same SANA2LOOP_OPEN_MAGIC check as fill_read_from_packet()'s own
     * comment: a writer that never copied its open cookie into this
     * request would otherwise reach pick_copy_from_buff() below with a
     * garbage writer_slo, calling through whatever function pointer
     * happens to be at that address. */
    if (!writer_slo || writer_slo->slo_Magic != SANA2LOOP_OPEN_MAGIC) {
        io0->io_Error = S2ERR_BAD_ARGUMENT;
        io->ios2_WireError = S2WERR_BUFF_ERROR;
        return;
    }

    if (io->ios2_Req.io_Flags & SANA2IOF_RAW) {
        /* M15: the +14 relaxation is the Ethernet dst/src/type header's
         * size -- meaningless on a point-to-point wire, whose
         * S2_DEVICEQUERY already reports RawMTU=0 ("those without set it
         * to zero"). Declined loudly, matching the replay-mode precedent
         * below for "not a supported operation on this unit's mode/wire",
         * rather than silently accepting a frame shape that doesn't
         * exist on this wire. */
        if (unit->su_WireType != S2WireType_Ethernet) {
            io0->io_Error = S2ERR_NOT_SUPPORTED;
            return;
        }
        max_len += SANA2LOOP_RAW_HDR_SIZE;
    }

    /* M8: a replay-mode unit plays back a canned trace -- there is no
     * meaningful destination for anything written to it to go. Checked
     * before su_Online (a replay unit's online state has nothing to do
     * with whether writing to it makes sense at all -- this is a
     * fundamental "not supported for this mode", not a temporary
     * unavailability S2ERR_OUTOFSERVICE would imply). */
    if (unit->su_Mode == SANA2LOOP_MODE_REPLAY) {
        io0->io_Error = S2ERR_NOT_SUPPORTED;
        return;
    }

    if (!unit->su_Online) {
        io0->io_Error = S2ERR_OUTOFSERVICE;
        note_write_failure(unit); /* M7: su_OnlineAfter -- may auto-recover
                                      online for the NEXT attempt */
        return;
    }
    if (io->ios2_DataLength > max_len || io->ios2_DataLength > SANA2LOOP_MAX_PACKET) {
        io0->io_Error = S2ERR_MTU_EXCEEDED;
        return;
    }
    /* M15 `mintu=`: a write below the configured minimum is rejected, not
     * padded -- S2ERR_BAD_ARGUMENT, deliberately NOT S2ERR_MTU_EXCEEDED
     * (which names the too-LARGE failure specifically): the caller's
     * arguments don't satisfy this interface's floor, which is an
     * argument problem, not an MTU one. Default su_MinTU=0 makes this
     * unreachable on any unit that never configured it. */
    if (io->ios2_DataLength < unit->su_MinTU) {
        io0->io_Error = S2ERR_BAD_ARGUMENT;
        return;
    }

    /* M16 `errors=`: 1-in-N writes fail VISIBLY -- S2ERR_TX_FAILURE with
     * S2WERR_TOO_MANY_RETRIES (the classic "transmitter gave up" wire
     * error real Ethernet drivers report), plus S2EVENT_TX|S2EVENT_ERROR
     * fired to any queued S2_ONEVENT -- the error-path counterpart to
     * droppct/loss's silent vanishing, exercising a stack's
     * error-handling/retry code rather than its timeout recovery. Checked
     * before the packet buffer is even allocated: a failed transmit has
     * no delivery side to clean up, and everything after this point
     * (stats, fault pipeline) is about packets that were at least
     * accepted. Deliberately does NOT bump PacketsSent (nothing was) or
     * the su_OfflineAfter/su_OnlineAfter counters (those model link
     * flapping, a different failure class). */
    if (prng_hits_one_in(&unit->su_PrngState, unit->su_ErrorsOneIn)) {
        io0->io_Error = S2ERR_TX_FAILURE;
        io->ios2_WireError = S2WERR_TOO_MANY_RETRIES;
        fire_event(unit, S2EVENT_ERROR | S2EVENT_TX);
        return;
    }

    pkt = AllocMem(sizeof(struct EchoPacket), MEMF_CLEAR);
    if (!pkt) {
        io0->io_Error = S2ERR_NO_RESOURCES;
        return;
    }
    pkt->ep_Length     = io->ios2_DataLength;
    pkt->ep_PacketType = io->ios2_PacketType;

    swap_to_self = is_plain_write && (unit->su_Mode == SANA2LOOP_MODE_ECHO);
    if (swap_to_self) {
        CopyMem(io->ios2_DstAddr, pkt->ep_SrcAddr, SANA2_MAX_ADDR_BYTES);
        for (k = 0; k < SANA2_MAX_ADDR_BYTES; k++)
            pkt->ep_DstAddr[k] = (k < (int)unit->su_AddrSize) ? unit->su_Station[k] : 0;
    } else {
        for (k = 0; k < SANA2_MAX_ADDR_BYTES; k++)
            pkt->ep_SrcAddr[k] = (k < (int)unit->su_AddrSize) ? unit->su_Station[k] : 0;
        CopyMem(io->ios2_DstAddr, pkt->ep_DstAddr, SANA2_MAX_ADDR_BYTES);
    }
    if (!pick_copy_from_buff(unit, writer_slo)(pkt->ep_Data, io->ios2_Data, pkt->ep_Length)) {
        FreeMem(pkt, sizeof(struct EchoPacket));
        io0->io_Error = S2ERR_NO_RESOURCES;
        io->ios2_WireError = S2WERR_BUFF_ERROR;
        return;
    }

    Disable();
    unit->su_Stats.PacketsSent++;
    {
        struct Sana2PacketTypeStats *ts = find_tracked_locked(unit, pkt->ep_PacketType);
        if (ts) {
            ts->PacketsSent++;
            ts->BytesSent += pkt->ep_Length;
        }
    }
    Enable();

    target = delivery_target(sld, unit);

    /* MTU-vs-peer / oversilent: only meaningful for crossover -- echo's
     * target IS unit, so target->su_MTU == unit->su_MTU and the earlier
     * own-MTU check already covered this case. Simulates a real PMTUD
     * black hole: the caller's own MTU check passed, but the frame is
     * still too big for what's actually on the other end of the link,
     * and (with oversilent=1) nothing ever tells the caller that. */
    /* M15: crossover across MISMATCHED wire types fails loudly
     * (S2ERR_BAD_STATE) rather than silently never matching the peer's
     * differently-sized station address in dest_matches_unit() -- a
     * confusing blackhole that, unlike oversilent's, nobody asked for.
     * Note the peer's wire type is whatever ITS last 0->1 open applied
     * (or the Ethernet default if it has never opened) -- config is only
     * ever read at a unit's own first open, so bringing up a non-Ethernet
     * crossover pair means opening both ends before writing, which the
     * pair's consumers necessarily do anyway. */
    if (unit->su_Mode == SANA2LOOP_MODE_CROSSOVER &&
        target->su_WireType != unit->su_WireType) {
        FreeMem(pkt, sizeof(struct EchoPacket));
        io0->io_Error = S2ERR_BAD_STATE;
        return;
    }

    if (unit->su_Mode == SANA2LOOP_MODE_CROSSOVER && pkt->ep_Length > target->su_MTU) {
        FreeMem(pkt, sizeof(struct EchoPacket));
        if (unit->su_Oversilent) {
            note_write_success(sld, unit); /* accepted, silently vanished */
            return;                       /* io0->io_Error stays 0 */
        }
        io0->io_Error = S2ERR_MTU_EXCEEDED;
        return;
    }

    if (prng_hits_pct(&unit->su_PrngState, unit->su_DropPct)) {
        FreeMem(pkt, sizeof(struct EchoPacket));
        note_write_success(sld, unit); /* still "sent" -- silently lost,
                                           same as real best-effort Ethernet */
        return;
    }

    /* M16 `loss=`: 1-in-N silent loss, immediately after droppct and with
     * identical observable behavior (write succeeded, packet vanished) --
     * just expressible at rates a whole percent can't reach. The two
     * compose: each draws independently when enabled. */
    if (prng_hits_one_in(&unit->su_PrngState, unit->su_LossOneIn)) {
        FreeMem(pkt, sizeof(struct EchoPacket));
        note_write_success(sld, unit);
        return;
    }

    if (unit->su_Truncate > 0 && pkt->ep_Length > unit->su_Truncate)
        pkt->ep_Length = unit->su_Truncate;

    if (prng_hits_pct(&unit->su_PrngState, unit->su_DupPct)) {
        struct EchoPacket *dup = AllocMem(sizeof(struct EchoPacket), MEMF_CLEAR);
        if (dup) {
            /* CopyMem, not a struct assignment: EchoPacket is large enough
             * (its ep_Data[SANA2LOOP_MAX_PACKET] dominates) that a plain
             * `*dup = *pkt` risks the same class of miscompile M3 already
             * found the hard way for a much smaller struct -- GCC can
             * still decide a big enough copy is worth a library call this
             * -nostdlib build doesn't have. CopyMem is an Exec LVO, not a
             * compiler-synthesized one, so it's always safe here. */
            CopyMem(pkt, dup, sizeof(struct EchoPacket));
            deliver_or_reorder(sld, unit, dup);
        }
    }

    deliver_or_reorder(sld, unit, pkt);
    note_write_success(sld, unit);
}

/* device dependent beginio function */
static void __attribute__((used))
begin_io(struct Library *dev asm("a6"), struct IORequest *io0 asm("a1"))
{
    struct IOSana2Req *io = (struct IOSana2Req *)io0;
    struct Sana2LoopUnit *unit = (struct Sana2LoopUnit *)io0->io_Unit;
    struct Sana2LoopDevice *sld = (struct Sana2LoopDevice *)dev;

    io0->io_Error = 0;

    switch (io0->io_Command) {

    case CMD_WRITE:
        /* Echo mode: the address we were asked to send TO becomes the src
         * the reader sees, and the reader's dst is our own station
         * address -- "src/dst addresses swapped", per the proposal.
         * Crossover mode uses real, unswapped addressing instead -- see
         * do_write()'s own comment for why the mode check lives there,
         * not here. */
        do_write(sld, unit, io0, io, TRUE);
        break;

    case S2_BROADCAST:
        /* Same as CMD_WRITE, with the destination forced to the all-1s
         * Ethernet broadcast address -- confirmed against a real driver
         * (plipbox), which implements S2_BROADCAST as exactly this
         * "stomp ios2_DstAddr, then fall into the normal write path"
         * pattern. */
        /* M15: broadcast is an Ethernet addressing concept -- a
         * point-to-point wire has exactly one place a frame can go and
         * no all-1s convention to name it with. Same S2ERR_NOT_SUPPORTED
         * shape as every other "this command has no meaning on this
         * unit's mode/wire" rejection here. */
        if (unit->su_WireType != S2WireType_Ethernet) {
            io0->io_Error = S2ERR_NOT_SUPPORTED;
            break;
        }
        {
            int k;
            for (k = 0; k < 6; k++)
                io->ios2_DstAddr[k] = 0xFF;
            for (k = 6; k < SANA2_MAX_ADDR_BYTES; k++)
                io->ios2_DstAddr[k] = 0;
        }
        do_write(sld, unit, io0, io, FALSE);
        break;

    case S2_MULTICAST:
        /* Unlike S2_BROADCAST, the caller already supplies the
         * destination (the multicast group to send to) in ios2_DstAddr --
         * this device's only extra behavior is validating that address
         * actually has the Ethernet group bit set (low bit of the first
         * octet), per the standard 802.3 multicast-addressing convention.
         * plipbox doesn't implement S2_MULTICAST at all, so this
         * validation is this device's own choice, not something it
         * confirms. */
        if (unit->su_WireType != S2WireType_Ethernet) {
            /* M15: same reasoning as S2_BROADCAST above. */
            io0->io_Error = S2ERR_NOT_SUPPORTED;
            break;
        }
        if (!(io->ios2_DstAddr[0] & 0x01)) {
            io0->io_Error = S2ERR_BAD_ARGUMENT;
            io->ios2_WireError = S2WERR_BAD_MULTICAST;
            break;
        }
        do_write(sld, unit, io0, io, FALSE);
        break;

    case CMD_READ: {
        /* Typed: only a packet whose ep_PacketType matches what the
         * caller set in ios2_PacketType before submitting satisfies this
         * read (per SANA-II convention) -- S2_READORPHAN below is the
         * catch-anything counterpart. */
        struct EchoPacket *pkt = NULL, *scan, *next;

        /* M8: a CONSUME-mode replay unit's CMD_READ ignores typing
         * entirely -- "consume mode" is a scripted, in-order playback of
         * whatever the capture actually contains, not a filter --
         * identical treatment to S2_READORPHAN below (see this case's own
         * comment there for why). Once the loaded sequence is exhausted,
         * falls straight through to the exact same queuing behavior as
         * any other unit with no traffic waiting -- a replay unit doesn't
         * hand out a special "end of stream" signal; SANA-II has no such
         * concept. M9's SCHEDULED replay mode deliberately does NOT take
         * this branch: its whole point is that a read submitted before a
         * record's due tick queues and WAITS for vblank_server() to
         * deliver it through the normal deliver_packet() precedence, not
         * grab whatever's next on demand -- see SANA2LOOP_REPLAYMODE_*'s
         * own comment. */
        if (unit->su_Mode == SANA2LOOP_MODE_REPLAY &&
            unit->su_ReplayMode == SANA2LOOP_REPLAYMODE_CONSUME) {
            /* Take-and-advance under Disable(): multiple tasks can issue
             * CMD_READ/S2_READORPHAN on the same consume-mode unit
             * concurrently, and without this, two readers racing the
             * last record can both pass the su_ReplayPos < su_ReplayCount
             * check before either increments -- the loser then gets a
             * su_ReplayNext one whole struct EchoPacket PAST the loaded
             * allocation, and fill_read_from_packet() trusts its garbage
             * ep_Length as a CopyMem length into the reader's buffer.
             * fill_read_from_packet() itself (a consumer hook call) stays
             * outside Disable() per this file's own discipline. */
            struct EchoPacket *rp = NULL;

            Disable();
            if (unit->su_ReplayPos < unit->su_ReplayCount) {
                rp = unit->su_ReplayNext;
                unit->su_ReplayNext++; /* plain pointer step -- see the
                                           field's own comment for why not
                                           su_ReplayPackets[su_ReplayPos] */
                unit->su_ReplayPos++;
            }
            Enable();

            if (rp) {
                if (fill_read_from_packet(io, rp)) {
                    io0->io_Error = 0;
                } else {
                    io0->io_Error = S2ERR_NO_RESOURCES;
                    io->ios2_WireError = S2WERR_BUFF_ERROR;
                }
                break;
            }
            /* else: exhausted -- falls through to normal queuing below,
               same as every non-replay unit with no traffic waiting. */
        }

        Disable();
        for (scan = (struct EchoPacket *)unit->su_PendingData.lh_Head;
             scan->ep_Node.ln_Succ;
             scan = next) {
            next = (struct EchoPacket *)scan->ep_Node.ln_Succ;
            if (scan->ep_PacketType == io->ios2_PacketType) {
                Remove((struct Node *)scan);
                unit->su_PendingDataCount--;
                pkt = scan;
                break;
            }
        }
        Enable();

        if (pkt) {
            if (fill_read_from_packet(io, pkt)) {
                io0->io_Error = 0;
            } else {
                io0->io_Error = S2ERR_NO_RESOURCES;
                io->ios2_WireError = S2WERR_BUFF_ERROR;
            }
            FreeMem(pkt, sizeof(struct EchoPacket));
        } else {
            io0->io_Flags &= ~IOF_QUICK;
            safe_add_tail(&unit->su_PendingReads, (struct Node *)io);
            return; /* completed later by a matching CMD_WRITE, or aborted */
        }
        break;
    }

    case S2_READORPHAN: {
        /* Untyped: takes whatever's at the head of su_PendingData, which
         * (by deliver_packet()'s precedence) only ever holds packets that
         * already failed to match any pending typed read -- i.e. already
         * orphaned, no further type filtering needed here. */
        struct EchoPacket *pkt;

        /* M8: see CMD_READ's matching comment above -- same replay-buffer
         * check, same exhausted-falls-through-to-normal-queuing behavior.
         * CMD_READ and S2_READORPHAN are indistinguishable on a
         * replay-mode unit: both just mean "give me the next captured
         * frame, whatever type it turns out to be". */
        if (unit->su_Mode == SANA2LOOP_MODE_REPLAY &&
            unit->su_ReplayMode == SANA2LOOP_REPLAYMODE_CONSUME) {
            /* Take-and-advance under Disable() -- same concurrent-reader
             * race as CMD_READ's identical block above; see its comment
             * for the failure scenario this avoids. */
            struct EchoPacket *rp = NULL;

            Disable();
            if (unit->su_ReplayPos < unit->su_ReplayCount) {
                rp = unit->su_ReplayNext;
                unit->su_ReplayNext++; /* plain pointer step -- see the
                                           field's own comment for why not
                                           su_ReplayPackets[su_ReplayPos] */
                unit->su_ReplayPos++;
            }
            Enable();

            if (rp) {
                if (fill_read_from_packet(io, rp)) {
                    io0->io_Error = 0;
                } else {
                    io0->io_Error = S2ERR_NO_RESOURCES;
                    io->ios2_WireError = S2WERR_BUFF_ERROR;
                }
                break;
            }
            /* else: exhausted -- falls through to normal queuing below. */
        }

        pkt = (struct EchoPacket *)safe_rem_head(&unit->su_PendingData);
        if (pkt) {
            Disable();
            unit->su_PendingDataCount--;
            Enable();
            if (fill_read_from_packet(io, pkt)) {
                io0->io_Error = 0;
            } else {
                io0->io_Error = S2ERR_NO_RESOURCES;
                io->ios2_WireError = S2WERR_BUFF_ERROR;
            }
            FreeMem(pkt, sizeof(struct EchoPacket));
        } else {
            io0->io_Flags &= ~IOF_QUICK;
            safe_add_tail(&unit->su_OrphanReads, (struct Node *)io);
            return; /* completed later by a matching CMD_WRITE, or aborted */
        }
        break;
    }

    case CMD_FLUSH: {
        struct IOSana2Req *pending;
        while ((pending = (struct IOSana2Req *)safe_rem_head(&unit->su_PendingReads)) != NULL) {
            pending->ios2_Req.io_Error = IOERR_ABORTED;
            ReplyMsg(&pending->ios2_Req.io_Message);
        }
        while ((pending = (struct IOSana2Req *)safe_rem_head(&unit->su_OrphanReads)) != NULL) {
            pending->ios2_Req.io_Error = IOERR_ABORTED;
            ReplyMsg(&pending->ios2_Req.io_Message);
        }
        /* M7: also flush anything this unit has buffered as a WRITER in
         * its own reorder window, per docs/PLAN.md's M7 entry. M17: same
         * for anything sitting in its delay queue awaiting a due tick. */
        reorder_flush(sld, unit);
        delay_queue_flush(sld, unit);
        break;
    }

    case CMD_RESET:
    case CMD_START:
    case CMD_STOP:
    case CMD_CLEAR:
        break; /* nothing unit-specific to do yet -- succeed */

    case S2_DEVICEQUERY: {
        /* M13: struct Sana2LoopDeviceQuery (this file's own extended copy
         * of the header's struct, see its own comment near the top of
         * this file) adds Rev 4's RawMTU past HardwareType. Every field
         * past SizeAvailable/SizeSupplied is now gated on `given`
         * actually covering it -- docs/sana2-notes.md's own
         * "S2_DEVICEQUERY convention" section already documents step 2
         * as "fills as many fields as fit," which this handler didn't
         * actually do before this fix: it wrote DevQueryFormat through
         * HardwareType unconditionally, so a caller that honestly
         * advertised a SizeAvailable smaller than the base struct (while
         * still >= the SizeAvailable/SizeSupplied header itself, which
         * must already exist for `given` to have been readable at all)
         * had its buffer overrun by however many bytes those fields
         * needed past `given`, even though dq->SizeSupplied correctly
         * reported the smaller number. Byte offsets are computed via
         * pointer arithmetic against the actual compiled struct layout,
         * not hand-derived, so this stays correct regardless of any
         * m68k alignment padding. */
        struct Sana2LoopDeviceQuery *dq = (struct Sana2LoopDeviceQuery *)io->ios2_StatData;
        ULONG mine, given;

        if (!dq) {
            io0->io_Error = S2ERR_BAD_ARGUMENT;
            io->ios2_WireError = S2WERR_NULL_POINTER;
            break;
        }
        mine = sizeof(struct Sana2LoopDeviceQuery);
        given = dq->SizeAvailable;

#define SANA2LOOP_DQ_FITS(field) \
        (given >= (ULONG)((UBYTE *)&dq->field - (UBYTE *)dq) + sizeof(dq->field))

        if (SANA2LOOP_DQ_FITS(DevQueryFormat))
            dq->DevQueryFormat = 0;
        if (SANA2LOOP_DQ_FITS(DeviceLevel))
            dq->DeviceLevel = 0;
        if (SANA2LOOP_DQ_FITS(AddrFieldSize))
            dq->AddrFieldSize = unit->su_AddrSize * 8; /* M15: in BITS */
        if (SANA2LOOP_DQ_FITS(MTU))
            dq->MTU = unit->su_MTU;
        if (SANA2LOOP_DQ_FITS(BPS))
            dq->BPS = unit->su_BPS;                    /* M15: `bps=` */
        if (SANA2LOOP_DQ_FITS(HardwareType))
            dq->HardwareType = unit->su_WireType;      /* M15: `wire=` */

#undef SANA2LOOP_DQ_FITS

        dq->SizeSupplied   = (given < mine) ? given : mine;
        dq->SizeAvailable  = mine;
        if (given >= mine) {
            /* Replay-mode units never support SANA2IOF_RAW writes at all
             * (do_write() fails them S2ERR_NOT_SUPPORTED before any MTU
             * check runs) -- RawMTU=0 per spec ("those without set it to
             * zero") is the honest signal for that, distinct from echo/
             * crossover's real MTU+SANA2LOOP_RAW_HDR_SIZE allowance
             * (exactly M4's own existing "buffer -> MTU+14" raw-write
             * relaxation -- RawMTU is that same number given a name). */
            /* M15: also 0 for a point-to-point wire -- the +14 allowance
             * IS the Ethernet header's size; there is no raw framing to
             * allow for on slip/cslip/ppp (do_write() declines
             * SANA2IOF_RAW there outright, same as replay mode). */
            dq->RawMTU = (unit->su_Mode == SANA2LOOP_MODE_REPLAY ||
                          unit->su_WireType != S2WireType_Ethernet)
                       ? 0 : unit->su_MTU + SANA2LOOP_RAW_HDR_SIZE;
        }
        break;
    }

    case S2_GETSTATIONADDRESS: {
        /* Convention (not enforced by the struct itself, since IOSana2Req
         * only has the one SrcAddr/DstAddr pair to reuse): ios2_SrcAddr =
         * CURRENT address, ios2_DstAddr = FACTORY/default address -- so a
         * caller can always tell whether S2_CONFIGINTERFACE has changed
         * anything without a separate query. */
        int k;
        for (k = 0; k < SANA2_MAX_ADDR_BYTES; k++) {
            io->ios2_SrcAddr[k] = (k < (int)unit->su_AddrSize) ? unit->su_Station[k] : 0;
            io->ios2_DstAddr[k] = (k < (int)unit->su_AddrSize) ? unit->su_FactoryStation[k] : 0;
        }
        break;
    }

    case S2_CONFIGINTERFACE: {
        /* First S2_CONFIGINTERFACE on a freshly-reset unit adopts
         * ios2_SrcAddr's low 6 bytes as the CURRENT station address
         * (su_FactoryStation never changes because of this). A second
         * attempt on an already-configured unit fails
         * S2ERR_BAD_STATE/S2WERR_IS_CONFIGURED -- SANA-II's documented way
         * of saying an interface only gets configured once per open cycle,
         * not a limitation specific to this device.
         *
         * Deliberately NOT gated on su_Online, and S2_ONLINE is NOT gated
         * on su_Configured: unlike a real wire, this device already hands
         * out a usable factory address at reset, precisely so the tool
         * works with zero setup -- requiring S2_CONFIGINTERFACE first
         * would only get in the way of that. */
        BOOL already;

        Disable();
        already = unit->su_Configured;
        if (!already) {
            /* Explicit field-by-field, not a loop -- see reset_unit()'s
             * matching comment: a plain fixed-count array-copy loop here
             * got recognized by -O2 and replaced with a call to `memmove`,
             * which doesn't exist in this freestanding build (the actual
             * link failure surfaced in init_device()/open(), not here,
             * because reset_unit() is small enough to get inlined into
             * both callers -- worth remembering next time a linker error
             * blames an innocent-looking function). */
            unit->su_Station[0] = io->ios2_SrcAddr[0];
            unit->su_Station[1] = io->ios2_SrcAddr[1];
            unit->su_Station[2] = io->ios2_SrcAddr[2];
            unit->su_Station[3] = io->ios2_SrcAddr[3];
            /* M15: bytes 4/5 only exist on a 6-byte (Ethernet) wire --
             * adopting the caller's bytes there on a 4-byte unit would
             * scribble past this wire's own address width for nothing
             * (harmless today since nothing reads past su_AddrSize, but
             * "harmless scribble" is how latent bugs start). */
            if (unit->su_AddrSize > 4) {
                unit->su_Station[4] = io->ios2_SrcAddr[4];
                unit->su_Station[5] = io->ios2_SrcAddr[5];
            }
            unit->su_Configured = TRUE;
            unit->su_Stats.Reconfigurations++;
        }
        Enable();

        if (already) {
            io0->io_Error = S2ERR_BAD_STATE;
            io->ios2_WireError = S2WERR_IS_CONFIGURED;
        }
        break;
    }

    case S2_TRACKTYPE: {
        /* Fixed SANA2LOOP_MAX_TRACKED_TYPES-slot table -- see its #define
         * for why fixed-size, not dynamically grown. */
        int i, slot = -1;
        BOOL already = FALSE;

        Disable();
        for (i = 0; i < SANA2LOOP_MAX_TRACKED_TYPES; i++) {
            if (unit->su_TypeInUse[i] && unit->su_TypeTracked[i] == io->ios2_PacketType) {
                already = TRUE;
                break;
            }
        }
        if (!already) {
            for (i = 0; i < SANA2LOOP_MAX_TRACKED_TYPES; i++) {
                if (!unit->su_TypeInUse[i]) {
                    slot = i;
                    break;
                }
            }
            if (slot >= 0) {
                unit->su_TypeInUse[slot]   = TRUE;
                unit->su_TypeTracked[slot] = io->ios2_PacketType;
                unit->su_TypeStats[slot].PacketsSent     = 0;
                unit->su_TypeStats[slot].PacketsReceived = 0;
                unit->su_TypeStats[slot].BytesSent       = 0;
                unit->su_TypeStats[slot].BytesReceived   = 0;
                unit->su_TypeStats[slot].PacketsDropped  = 0;
            }
        }
        Enable();

        if (already) {
            io0->io_Error = S2ERR_BAD_STATE;
            io->ios2_WireError = S2WERR_ALREADY_TRACKED;
        } else if (slot < 0) {
            /* Table full. The real header has no dedicated S2WERR_* for
             * this specific case -- S2ERR_NO_RESOURCES is the closest
             * documented fit ("driver couldn't allocate what this request
             * needed"), same as an AllocMem() failure elsewhere. */
            io0->io_Error = S2ERR_NO_RESOURCES;
        }
        break;
    }

    case S2_UNTRACKTYPE: {
        int i;
        BOOL found = FALSE;

        Disable();
        for (i = 0; i < SANA2LOOP_MAX_TRACKED_TYPES; i++) {
            if (unit->su_TypeInUse[i] && unit->su_TypeTracked[i] == io->ios2_PacketType) {
                unit->su_TypeInUse[i] = FALSE;
                found = TRUE;
                break;
            }
        }
        Enable();

        if (!found) {
            io0->io_Error = S2ERR_BAD_STATE;
            io->ios2_WireError = S2WERR_NOT_TRACKED;
        }
        break;
    }

    case S2_GETTYPESTATS: {
        struct Sana2PacketTypeStats *dst = (struct Sana2PacketTypeStats *)io->ios2_StatData;
        int i;
        BOOL found = FALSE;

        if (!dst) {
            io0->io_Error = S2ERR_BAD_ARGUMENT;
            io->ios2_WireError = S2WERR_NULL_POINTER;
            break;
        }

        Disable();
        for (i = 0; i < SANA2LOOP_MAX_TRACKED_TYPES; i++) {
            if (unit->su_TypeInUse[i] && unit->su_TypeTracked[i] == io->ios2_PacketType) {
                /* Field-by-field, not a struct assignment: on this
                 * freestanding -nostdlib build a `*dst = ...[i]` struct
                 * copy through a variably-indexed array compiled to a call
                 * to `memmove`, which doesn't exist without libc -- same
                 * reasoning as this file's own default_copy()/CopyMem use
                 * for packet payloads, just discovered here instead. */
                dst->PacketsSent     = unit->su_TypeStats[i].PacketsSent;
                dst->PacketsReceived = unit->su_TypeStats[i].PacketsReceived;
                dst->BytesSent       = unit->su_TypeStats[i].BytesSent;
                dst->BytesReceived   = unit->su_TypeStats[i].BytesReceived;
                dst->PacketsDropped  = unit->su_TypeStats[i].PacketsDropped;
                found = TRUE;
                break;
            }
        }
        Enable();

        if (!found) {
            io0->io_Error = S2ERR_BAD_STATE;
            io->ios2_WireError = S2WERR_NOT_TRACKED;
        }
        break;
    }

    case S2_ADDMULTICASTADDRESS: {
        /* The group address to join, low 6 bytes -- ios2_SrcAddr, matching
         * the same "single address of interest" convention
         * S2_CONFIGINTERFACE already uses (real header/plipbox don't
         * document this field explicitly since plipbox never implements
         * multicast at all -- this is the well-established SANA-II
         * convention, not a guess specific to this device). Refcounted:
         * joining an address already joined (by this or another open on
         * the unit) just bumps the count, since delivery here isn't
         * filtered per-open anyway -- see the struct field comment. */
        UBYTE *addr = io->ios2_SrcAddr;
        int slot = -1;
        BOOL already;

        if (unit->su_WireType != S2WireType_Ethernet) {
            /* M15: group membership is Ethernet-only, like the group bit
             * check right below it. */
            io0->io_Error = S2ERR_NOT_SUPPORTED;
            break;
        }
        if (!(addr[0] & 0x01)) {
            io0->io_Error = S2ERR_BAD_ARGUMENT;
            io->ios2_WireError = S2WERR_BAD_MULTICAST;
            break;
        }

        Disable();
        {
            int existing = find_mcast_locked(unit, addr);
            already = existing >= 0;
            if (already) {
                unit->su_McastRefCount[existing]++;
            } else {
                int i;
                for (i = 0; i < SANA2LOOP_MAX_MULTICAST; i++) {
                    if (!unit->su_McastInUse[i]) {
                        slot = i;
                        break;
                    }
                }
                if (slot >= 0) {
                    int k;
                    unit->su_McastInUse[slot] = TRUE;
                    for (k = 0; k < 6; k++)
                        unit->su_McastAddr[slot][k] = addr[k];
                    unit->su_McastRefCount[slot] = 1;
                }
            }
        }
        Enable();

        if (!already && slot < 0) {
            /* Fixed SANA2LOOP_MAX_MULTICAST-slot table is full -- unlike
             * S2_TRACKTYPE's table-full case (M3), the real header has a
             * dedicated wire error for exactly this. */
            io0->io_Error = S2ERR_NO_RESOURCES;
            io->ios2_WireError = S2WERR_MULTICAST_FULL;
        }
        break;
    }

    case S2_DELMULTICASTADDRESS: {
        UBYTE *addr = io->ios2_SrcAddr;
        int existing;

        if (unit->su_WireType != S2WireType_Ethernet) {
            /* M15: same as S2_ADDMULTICASTADDRESS above. */
            io0->io_Error = S2ERR_NOT_SUPPORTED;
            break;
        }
        Disable();
        existing = find_mcast_locked(unit, addr);
        if (existing >= 0 && --unit->su_McastRefCount[existing] == 0)
            unit->su_McastInUse[existing] = FALSE;
        Enable();

        if (existing < 0) {
            io0->io_Error = S2ERR_BAD_STATE;
            io->ios2_WireError = S2WERR_BAD_MULTICAST;
        }
        break;
    }

    case S2_ONLINE:
        force_online(unit);
        break;

    case S2_OFFLINE:
        force_offline(sld, unit);
        break;

    case S2_ONEVENT: {
        /* Only ONLINE/OFFLINE map to a persistent state that can already
         * be "true" at submission time -- the other event bits (ERROR/TX/
         * RX/BUFF/HARDWARE/SOFTWARE) describe momentary conditions with no
         * current-state concept, so they always queue. */
        ULONG immediate = io->ios2_WireError &
                          (unit->su_Online ? S2EVENT_ONLINE : S2EVENT_OFFLINE);
        if (immediate) {
            io->ios2_WireError = immediate;
            break; /* completes via the normal bottom-of-switch reply below */
        }
        io0->io_Flags &= ~IOF_QUICK;
        safe_add_tail(&unit->su_EventReqs, (struct Node *)io);
        return; /* completed later by fire_event() on an actual transition */
    }

    case S2_GETGLOBALSTATS: {
        /* Unlike Sana2DeviceQuery, Sana2DeviceStats has no SizeAvailable/
         * SizeSupplied negotiation in the spec -- it's just a fixed struct,
         * copied whole. LastStart stays zeroed until a virtual tick
         * counter exists (Phase 3) to fill it deterministically; a real
         * wall-clock timestamp would work fine functionally but has no
         * business in a device whose whole point is deterministic replay. */
        struct Sana2DeviceStats *st = (struct Sana2DeviceStats *)io->ios2_StatData;
        if (!st) {
            io0->io_Error = S2ERR_BAD_ARGUMENT;
            io->ios2_WireError = S2WERR_NULL_POINTER;
            break;
        }
        *st = unit->su_Stats;
        break;
    }

    case S2_GETSPECIALSTATS: {
        /* Up to two records now (M13 added the second): one if this unit
         * is both recording AND has actually dropped something to
         * su_RecordMax (docs/PLAN.md decision #7: "overflow = stop +
         * count via S2_GETSPECIALSTATS"), one if any open on this unit
         * negotiated a Rev 7 DMA hook (su_DMAHooksOffered -- M13). Zero
         * records if neither condition holds. Per the standard SANA-II
         * convention (not spelled out in the bare Sana2SpecialStatHeader
         * struct itself, since it has no explicit array-pointer field):
         * the caller allocates RecordCountMax Sana2SpecialStatRecord
         * slots CONTIGUOUS with the header in the SAME ios2_StatData
         * buffer; the driver writes up to that many immediately after the
         * header, never more than RecordCountMax regardless of how many
         * it actually has. A caller with RecordCountMax=1 that qualifies
         * for both stats gets only the record-overflow one -- a
         * deterministic priority (declaration order below), not a
         * regression from M13: RecordCountMax=1 callers saw exactly this
         * stat, and only this stat, before M13 existed at all. */
        struct Sana2SpecialStatHeader *hdr =
            (struct Sana2SpecialStatHeader *)io->ios2_StatData;
        struct Sana2SpecialStatRecord *rec;
        ULONG supplied = 0;

        if (!hdr) {
            io0->io_Error = S2ERR_BAD_ARGUMENT;
            io->ios2_WireError = S2WERR_NULL_POINTER;
            break;
        }
        rec = (struct Sana2SpecialStatRecord *)(hdr + 1);
        if (unit->su_RecordDropped > 0 && supplied < hdr->RecordCountMax) {
            rec[supplied].Type   = SANA2LOOP_SPECIALSTAT_RECORD_OVERFLOW;
            rec[supplied].Count  = unit->su_RecordDropped;
            rec[supplied].String = NULL;
            supplied++;
        }
        if (unit->su_DMAHooksOffered > 0 && supplied < hdr->RecordCountMax) {
            rec[supplied].Type   = SANA2LOOP_SPECIALSTAT_DMA_HOOKS_OFFERED;
            rec[supplied].Count  = unit->su_DMAHooksOffered;
            rec[supplied].String = NULL;
            supplied++;
        }
        hdr->RecordCountSupplied = supplied;
        break;
    }

    /* M13: Rev 4 peer/DNS address queries. Same ios2_SrcAddr/ios2_DstAddr
     * reuse convention S2_GETSTATIONADDRESS already established above for
     * a different current/factory pairing -- here it's local/peer, per
     * wiki.amigaos.net's "Revision 4" page. */
    case S2_GETPEERADDRESS: {
        /* Only meaningful when there's a real, distinct peer -- exactly
         * crossover mode. Echo's "peer" is itself (not a real link
         * partner); a replay-mode unit has no live link at all (do_write()
         * already rejects writes to one the same way). Both fail
         * S2ERR_NOT_SUPPORTED, matching do_write()'s own replay-mode
         * precedent for "not a supported operation on this mode", not a
         * temporary condition. */
        struct Sana2LoopUnit *peer;
        int k;

        if (unit->su_Mode != SANA2LOOP_MODE_CROSSOVER) {
            io0->io_Error = S2ERR_NOT_SUPPORTED;
            break;
        }
        peer = delivery_target(sld, unit);
        for (k = 0; k < SANA2_MAX_ADDR_BYTES; k++) {
            io->ios2_SrcAddr[k] = (k < (int)unit->su_AddrSize) ? unit->su_Station[k] : 0;
            io->ios2_DstAddr[k] = (k < (int)peer->su_AddrSize) ? peer->su_Station[k] : 0;
        }
        break;
    }

    case S2_GETDNSADDRESS: {
        /* Meaningful for PPP-style/dynamically-addressed links, which this
         * device has none of. Deliberately a DIFFERENT "not applicable"
         * shape from S2_GETPEERADDRESS above -- zero-filled addresses with
         * io_Error left at 0 (success), per docs/PLAN.md's own M13 section
         * ("loopback can synthesize plausible values or zero-fill per spec
         * when N/A") -- a real stack has to tolerate both an outright
         * S2ERR_NOT_SUPPORTED and a soft "supported, nothing to report"
         * response, and this device now demonstrates both. */
        int k;
        for (k = 0; k < SANA2_MAX_ADDR_BYTES; k++) {
            io->ios2_SrcAddr[k] = 0;
            io->ios2_DstAddr[k] = 0;
        }
        break;
    }

    /* S2_ADD/DELMULTICASTADDRESSES (plural -- the batch-list variants,
     * 0xc000/0xc001) have no consumer needing them yet, same treatment as
     * the Rev 2/3/7 buffer-management hook variants got before M13 --
     * falling to default (IOERR_NOCMD) here is correct, not a gap. */
    default:
        io0->io_Error = IOERR_NOCMD;
        break;
    }

    if (!(io0->io_Flags & IOF_QUICK))
        ReplyMsg(&io0->io_Message);
}

/* device dependent abortio function */
static ULONG __attribute__((used))
abort_io(struct Library *dev asm("a6"), struct IORequest *io asm("a1"))
{
    struct Sana2LoopUnit *unit = (struct Sana2LoopUnit *)io->io_Unit;
    (void)dev;

    if (safe_unlink_if_queued(&unit->su_PendingReads, (struct Node *)io) ||
        safe_unlink_if_queued(&unit->su_OrphanReads, (struct Node *)io) ||
        safe_unlink_if_queued(&unit->su_EventReqs, (struct Node *)io)) {
        io->io_Error = IOERR_ABORTED;
        ReplyMsg(&io->io_Message);
        return 0;
    }
    return -1; /* already in-flight or completed -- let WaitIO see it naturally */
}

static ULONG device_vectors[] = {
    (ULONG)open,
    (ULONG)close,
    (ULONG)expunge,
    0, /* ExtFunc: unused */
    (ULONG)begin_io,
    (ULONG)abort_io,
    (ULONG)-1
};

const ULONG auto_init_tables[4] = {
    sizeof(struct Sana2LoopDevice),
    (ULONG)device_vectors,
    0,
    (ULONG)init_device
};
