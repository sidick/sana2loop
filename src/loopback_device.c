/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Simon Dick
 */

/* loopback_device.c -- sana2loop's loopback.device, Phase 1.
 *
 * A hardware-free SANA-II device: echo mode, S2_DEVICEQUERY, S2_ONLINE/
 * S2_OFFLINE, and S2_ONEVENT. Crossover pairs, fault injection, and pcap
 * replay/record are later phases (see the project proposal).
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
 * ReadArgs, no dos.library from the unit's perspective (there is no
 * separate unit task in Phase 1 -- echo completes synchronously inside
 * BeginIO on the calling task, matching from the caller's write straight to
 * a queued reader's CMD_READ).
 */

#include <exec/types.h>
#include <exec/nodes.h>
#include <exec/lists.h>
#include <exec/io.h>
#include <exec/errors.h>
#include <exec/resident.h>
#include <exec/devices.h>
#include <exec/memory.h>
#include <devices/sana2.h>
#include <utility/tagitem.h>
#include <proto/exec.h>

#define STR(s)  #s
#define XSTR(s) STR(s)

#define DEVICE_NAME       "loopback.device"
#define DEVICE_DATE       "(30 Jul 2026)"
#ifndef DEVICE_VERSION
#define DEVICE_VERSION 1
#endif
#ifndef DEVICE_REVISION
#define DEVICE_REVISION 0
#endif
#define DEVICE_ID_STRING  "sana2loop " XSTR(DEVICE_VERSION) "." XSTR(DEVICE_REVISION) " " DEVICE_DATE
#define DEVICE_PRIORITY   0

/* Unit 0 is the Phase-1 echo default (per the proposal's mode-selection
 * plan). Units 1-3 exist so Phase 2 crossover pairs have somewhere to go
 * without changing the device base layout again. */
#define SANA2LOOP_NUM_UNITS   4
#define SANA2LOOP_DEFAULT_MTU 1500
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

/* SysBase -- the one file-scope global this file needs. Safe here: this is
 * a plain -nostdlib freestanding build (no -fbaserel), so globals use
 * normal addressing, not an A4-relative small-data segment that some
 * framework may or may not point at us. Set once in init_device(), which
 * Exec guarantees runs single-threaded before any Open. */
struct ExecBase *SysBase;

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
    ULONG       su_MTU;
    BOOL        su_Online;
    ULONG       su_RxQueueMax;        /* bounded pending-data depth; 0 = strict
                                          SANA-II drop semantics (the default --
                                          M5's config knob overrides this) */
    ULONG       su_PendingDataCount;  /* current su_PendingData depth */
    struct Sana2DeviceStats su_Stats;
    /* S2_TRACKTYPE table: fixed SANA2LOOP_MAX_TRACKED_TYPES slots, shared
     * unit-wide state (any open can track/untrack/query any type) --
     * su_TypeInUse[i] gates whether slot i holds a live entry at all. */
    BOOL        su_TypeInUse[SANA2LOOP_MAX_TRACKED_TYPES];
    ULONG       su_TypeTracked[SANA2LOOP_MAX_TRACKED_TYPES];
    struct Sana2PacketTypeStats su_TypeStats[SANA2LOOP_MAX_TRACKED_TYPES];
    /* S2_ADD/DELMULTICASTADDRESS table: fixed SANA2LOOP_MAX_MULTICAST
     * slots, refcounted -- two opens joining the same group both need to
     * leave before it's actually dropped. Bookkeeping only: delivery stays
     * unit-wide and unfiltered by group membership, same as it always has
     * been (this device has no per-open receive filtering at all yet, so
     * there's nothing for group membership to gate). */
    BOOL        su_McastInUse[SANA2LOOP_MAX_MULTICAST];
    UBYTE       su_McastAddr[SANA2LOOP_MAX_MULTICAST][6];
    ULONG       su_McastRefCount[SANA2LOOP_MAX_MULTICAST];
    struct List su_PendingReads;      /* queued, TYPED CMD_READ IORequests */
    struct List su_OrphanReads;       /* queued S2_READORPHAN IORequests (untyped) */
    struct List su_PendingData;       /* packets awaiting a CMD_READ/S2_READORPHAN,
                                          bounded by su_RxQueueMax */
    struct List su_EventReqs;         /* queued S2_ONEVENT IORequests */
};

struct Sana2LoopDevice {
    struct Device         sld_Device;
    BPTR                  sld_SegList;   /* handed back to Exec by expunge() */
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

#define SANA2LOOP_OPEN_MAGIC 0x53324C4F /* 'S2LO' */

struct Hook; /* forward decl only -- recorded, not yet dispatched (later phase) */

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
    struct Hook           *slo_PacketFilter;  /* recorded; not yet dispatched */
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
        /* S2_CopyTo/FromBuff16/32, S2_DMACopyTo/FromBuff32 (Rev 7 extras):
         * no consumer of them exists in this device, so recorded nowhere;
         * falling to default (ignored) here is correct, not a gap. */
        default:
            break;
        }
        tags++;
    }
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
        struct IOSana2Req *io, *next;
        Disable();
        for (io = (struct IOSana2Req *)lists[li]->lh_Head;
             io->ios2_Req.io_Message.mn_Node.ln_Succ;
             io = next) {
            next = (struct IOSana2Req *)io->ios2_Req.io_Message.mn_Node.ln_Succ;
            if ((struct Sana2LoopOpen *)io->ios2_BufferManagement == slo) {
                Remove((struct Node *)io);
                io->ios2_Req.io_Error = IOERR_ABORTED;
                Enable();
                ReplyMsg(&io->ios2_Req.io_Message);
                Disable();
            }
        }
        Enable();
    }
}

/* Copies address/type/length directly (driver-owned fields, never subject
 * to buffer-management hooks) and the packet payload through the
 * completing reader's own CopyToBuff hook. Returns FALSE on hook failure
 * so the caller can fail the read with S2ERR_NO_RESOURCES/S2WERR_BUFF_ERROR
 * instead of claiming success over data that was never actually copied. */
static BOOL fill_read_from_packet(struct IOSana2Req *io, struct EchoPacket *pkt)
{
    struct Sana2LoopOpen *slo = (struct Sana2LoopOpen *)io->ios2_BufferManagement;

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
    {
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

    return slo->slo_CopyToBuff(io->ios2_Data, pkt->ep_Data, pkt->ep_Length);
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

    Disable();
    for (io = (struct IOSana2Req *)unit->su_EventReqs.lh_Head;
         io->ios2_Req.io_Message.mn_Node.ln_Succ;
         io = next) {
        next = (struct IOSana2Req *)io->ios2_Req.io_Message.mn_Node.ln_Succ;
        if (io->ios2_WireError & event) {
            Remove((struct Node *)io);
            io->ios2_WireError = io->ios2_WireError & event;
            io->ios2_Req.io_Error = 0;
            Enable();
            if (!(io->ios2_Req.io_Flags & IOF_QUICK))
                ReplyMsg(&io->ios2_Req.io_Message);
            Disable();
        }
    }
    Enable();
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
    unit->su_Online      = FALSE;
    unit->su_RxQueueMax  = 0; /* strict drop by default -- see docs/PLAN.md M2 */
    unit->su_PendingDataCount = 0;
    unit->su_Configured  = FALSE;

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

/* Exec's real NewList() isn't reliably declared under this minimal include
 * set (a freestanding -nostdlib build, deliberately not pulling in more of
 * the NDK than needed) -- it's three pointer stores, so just do them. */
static void list_init(struct List *list)
{
    list->lh_Head     = (struct Node *)&list->lh_Tail;
    list->lh_Tail      = NULL;
    list->lh_TailPred  = (struct Node *)&list->lh_Head;
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
        reset_unit(u);

        list_init(&u->su_PendingReads);
        list_init(&u->su_OrphanReads);
        list_init(&u->su_PendingData);
        list_init(&u->su_EventReqs);
    }

    return dev;
}

/* device dependent expunge function
 * CAUTION: runs in a forbidden state; guaranteed single-threaded. */
static BPTR __attribute__((used)) expunge(struct Library *dev asm("a6"))
{
    struct Sana2LoopDevice *sld = (struct Sana2LoopDevice *)dev;
    BPTR seg_list;

    if (dev->lib_OpenCnt != 0) {
        dev->lib_Flags |= LIBF_DELEXP;
        return 0;
    }
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
    if (unit->su_Unit.unit_OpenCnt == 0)
        reset_unit(unit); /* first opener of an idle unit starts clean --
                              see reset_unit()'s comment */

    slo = AllocMem(sizeof(struct Sana2LoopOpen), MEMF_CLEAR);
    if (!slo) {
        io->io_Error = S2ERR_NO_RESOURCES;
        return;
    }
    slo->slo_Unit  = unit;
    slo->slo_Magic = SANA2LOOP_OPEN_MAGIC;
    /* ios2_BufferManagement arrives pointing at the caller's tag list (or
     * NULL); apply_buffer_mgmt_tags() reads it before we overwrite the
     * field below with our own cookie. */
    apply_buffer_mgmt_tags(slo, (struct TagItem *)io2->ios2_BufferManagement);

    io->io_Device = (struct Device *)dev;
    io->io_Unit   = &unit->su_Unit;
    io->io_Unit->unit_OpenCnt++;
    dev->lib_OpenCnt++;
    io2->ios2_BufferManagement = slo; /* per SANA-II: the opener must copy
                                          this into every future request */
    io->io_Error = 0;
    (void)flags; /* SANA2OPF_MINE / SANA2OPF_PROM: no exclusivity or
                    promiscuous distinction in a software loopback -- every
                    opener already sees everything on its unit. */
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

    if (--unit->su_Unit.unit_OpenCnt == 0)
        drain_unit(unit);

    FreeMem(slo, sizeof(struct Sana2LoopOpen));
    io2->ios2_BufferManagement = NULL;
    io->io_Device = NULL;
    io->io_Unit   = NULL;
    dev->lib_OpenCnt--;

    if (dev->lib_OpenCnt == 0 && (dev->lib_Flags & LIBF_DELEXP))
        return expunge(dev);

    return 0;
}

/* Delivers one packet to `unit` (the receiving side -- for echo that's the
 * same unit the write happened on; a later milestone's crossover mode
 * calls this on the PEER unit instead, which is the whole reason this is
 * its own function rather than inlined into CMD_WRITE).
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
 * Must never be called with a Disable() already held by the caller: it
 * calls consumer hooks and ReplyMsg, neither of which may run disabled. */
static void deliver_packet(struct Sana2LoopUnit *unit, struct EchoPacket *pkt)
{
    struct IOSana2Req *waiting = NULL;
    BOOL queued = FALSE;

    Disable();
    unit->su_Stats.PacketsReceived++;
    {
        struct IOSana2Req *io, *next;
        for (io = (struct IOSana2Req *)unit->su_PendingReads.lh_Head;
             io->ios2_Req.io_Message.mn_Node.ln_Succ;
             io = next) {
            next = (struct IOSana2Req *)io->ios2_Req.io_Message.mn_Node.ln_Succ;
            if (io->ios2_PacketType == pkt->ep_PacketType) {
                Remove((struct Node *)io);
                waiting = io;
                break;
            }
        }
        if (!waiting)
            waiting = (struct IOSana2Req *)RemHead(&unit->su_OrphanReads);
        if (!waiting && unit->su_PendingDataCount < unit->su_RxQueueMax) {
            AddTail(&unit->su_PendingData, (struct Node *)pkt);
            unit->su_PendingDataCount++;
            queued = TRUE;
        }
        if (!waiting && !queued)
            unit->su_Stats.UnknownTypesReceived++;

        /* S2_TRACKTYPE accounting: PacketsReceived/BytesReceived count the
         * packet arriving at this interface at all (delivered immediately
         * OR accepted into the rxqueue for later reading) -- PacketsDropped
         * is reserved for the one outcome that's genuinely a loss. */
        {
            struct Sana2PacketTypeStats *ts = find_tracked_locked(unit, pkt->ep_PacketType);
            if (ts) {
                if (waiting || queued) {
                    ts->PacketsReceived++;
                    ts->BytesReceived += pkt->ep_Length;
                } else {
                    ts->PacketsDropped++;
                }
            }
        }
    }
    Enable();

    if (waiting) {
        if (fill_read_from_packet(waiting, pkt)) {
            waiting->ios2_Req.io_Error = 0;
        } else {
            waiting->ios2_Req.io_Error = S2ERR_NO_RESOURCES;
            waiting->ios2_WireError = S2WERR_BUFF_ERROR;
        }
        FreeMem(pkt, sizeof(struct EchoPacket));
        if (!(waiting->ios2_Req.io_Flags & IOF_QUICK))
            ReplyMsg(&waiting->ios2_Req.io_Message);
    } else if (!queued) {
        FreeMem(pkt, sizeof(struct EchoPacket));
    }
    /* else: queued -- pkt now belongs to su_PendingData, nothing more to do */
}

/* Shared body for CMD_WRITE and its S2_BROADCAST/S2_MULTICAST cousins --
 * all three ultimately hand one packet to deliver_packet(); they differ
 * only in what ios2_DstAddr means and whether the reader should be told
 * that unit's own station address, or the actual destination:
 *  - plain CMD_WRITE (echo_dst_to_self=TRUE): unchanged since Phase 1 --
 *    the reader sees itself as the recipient (this unit's own station
 *    address), the "src/dst addresses swapped" echo semantics.
 *  - S2_BROADCAST/S2_MULTICAST (echo_dst_to_self=FALSE): the reader must
 *    see the ACTUAL destination (all-1s broadcast, or the caller's chosen
 *    multicast group) or SANA2IOF_BCAST/MCAST (set in
 *    fill_read_from_packet(), from this same address) would never mean
 *    anything on the receiving end. */
static void do_write(struct Sana2LoopUnit *unit, struct IORequest *io0,
                      struct IOSana2Req *io, BOOL echo_dst_to_self)
{
    struct EchoPacket *pkt;
    struct Sana2LoopOpen *writer_slo = (struct Sana2LoopOpen *)io->ios2_BufferManagement;
    ULONG max_len = unit->su_MTU;

    if (io->ios2_Req.io_Flags & SANA2IOF_RAW)
        max_len += SANA2LOOP_RAW_HDR_SIZE;

    if (!unit->su_Online) {
        io0->io_Error = S2ERR_OUTOFSERVICE;
        return;
    }
    if (io->ios2_DataLength > max_len || io->ios2_DataLength > SANA2LOOP_MAX_PACKET) {
        io0->io_Error = S2ERR_MTU_EXCEEDED;
        return;
    }

    pkt = AllocMem(sizeof(struct EchoPacket), MEMF_CLEAR);
    if (!pkt) {
        io0->io_Error = S2ERR_NO_RESOURCES;
        return;
    }
    pkt->ep_Length     = io->ios2_DataLength;
    pkt->ep_PacketType = io->ios2_PacketType;
    CopyMem(io->ios2_DstAddr, pkt->ep_SrcAddr, SANA2_MAX_ADDR_BYTES);
    if (echo_dst_to_self) {
        int k;
        for (k = 0; k < SANA2_MAX_ADDR_BYTES; k++)
            pkt->ep_DstAddr[k] = (k < 6) ? unit->su_Station[k] : 0;
    } else {
        CopyMem(io->ios2_DstAddr, pkt->ep_DstAddr, SANA2_MAX_ADDR_BYTES);
    }
    if (!writer_slo->slo_CopyFromBuff(pkt->ep_Data, io->ios2_Data, pkt->ep_Length)) {
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
    deliver_packet(unit, pkt);
}

/* device dependent beginio function */
static void __attribute__((used))
begin_io(struct Library *dev asm("a6"), struct IORequest *io0 asm("a1"))
{
    struct IOSana2Req *io = (struct IOSana2Req *)io0;
    struct Sana2LoopUnit *unit = (struct Sana2LoopUnit *)io0->io_Unit;
    (void)dev;

    io0->io_Error = 0;

    switch (io0->io_Command) {

    case CMD_WRITE:
        /* Echo semantics: the address we were asked to send TO becomes the
         * src the reader sees; the reader's dst is our own station address
         * -- "src/dst addresses swapped", per the proposal. */
        do_write(unit, io0, io, TRUE);
        break;

    case S2_BROADCAST:
        /* Same as CMD_WRITE, with the destination forced to the all-1s
         * Ethernet broadcast address -- confirmed against a real driver
         * (plipbox), which implements S2_BROADCAST as exactly this
         * "stomp ios2_DstAddr, then fall into the normal write path"
         * pattern. */
        {
            int k;
            for (k = 0; k < 6; k++)
                io->ios2_DstAddr[k] = 0xFF;
            for (k = 6; k < SANA2_MAX_ADDR_BYTES; k++)
                io->ios2_DstAddr[k] = 0;
        }
        do_write(unit, io0, io, FALSE);
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
        if (!(io->ios2_DstAddr[0] & 0x01)) {
            io0->io_Error = S2ERR_BAD_ARGUMENT;
            io->ios2_WireError = S2WERR_BAD_MULTICAST;
            break;
        }
        do_write(unit, io0, io, FALSE);
        break;

    case CMD_READ: {
        /* Typed: only a packet whose ep_PacketType matches what the
         * caller set in ios2_PacketType before submitting satisfies this
         * read (per SANA-II convention) -- S2_READORPHAN below is the
         * catch-anything counterpart. */
        struct EchoPacket *pkt = NULL, *scan, *next;

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
        struct EchoPacket *pkt = (struct EchoPacket *)safe_rem_head(&unit->su_PendingData);
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
        break;
    }

    case CMD_RESET:
    case CMD_START:
    case CMD_STOP:
    case CMD_CLEAR:
        break; /* nothing unit-specific to do yet -- succeed */

    case S2_DEVICEQUERY: {
        struct Sana2DeviceQuery *dq = (struct Sana2DeviceQuery *)io->ios2_StatData;
        ULONG mine, given;

        if (!dq) {
            io0->io_Error = S2ERR_BAD_ARGUMENT;
            io->ios2_WireError = S2WERR_NULL_POINTER;
            break;
        }
        mine = sizeof(struct Sana2DeviceQuery);
        given = dq->SizeAvailable;

        dq->DevQueryFormat = 0;
        dq->DeviceLevel    = 0;
        dq->AddrFieldSize  = 48;
        dq->MTU            = unit->su_MTU;
        dq->BPS            = 10000000;
        dq->HardwareType   = S2WireType_Ethernet;
        dq->SizeSupplied   = (given < mine) ? given : mine;
        dq->SizeAvailable  = mine;
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
            io->ios2_SrcAddr[k] = (k < 6) ? unit->su_Station[k] : 0;
            io->ios2_DstAddr[k] = (k < 6) ? unit->su_FactoryStation[k] : 0;
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
            unit->su_Station[4] = io->ios2_SrcAddr[4];
            unit->su_Station[5] = io->ios2_SrcAddr[5];
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
        unit->su_Online = TRUE;
        fire_event(unit, S2EVENT_ONLINE);
        break;

    case S2_OFFLINE:
        unit->su_Online = FALSE;
        fire_event(unit, S2EVENT_OFFLINE);
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
        /* Zero-record: this device doesn't track any special (per-driver,
         * non-standard) stats yet. RecordCountMax is the caller's own
         * buffer capacity (left as they set it); we only report how many
         * we actually supplied. */
        struct Sana2SpecialStatHeader *hdr =
            (struct Sana2SpecialStatHeader *)io->ios2_StatData;
        if (!hdr) {
            io0->io_Error = S2ERR_BAD_ARGUMENT;
            io->ios2_WireError = S2WERR_NULL_POINTER;
            break;
        }
        hdr->RecordCountSupplied = 0;
        break;
    }

    /* S2_ADD/DELMULTICASTADDRESSES (plural -- the Rev 7 batch-list
     * variants, 0xc000/0xc001) have no consumer needing them yet, same
     * treatment as the Rev 7 buffer-management hook variants (M1) --
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
