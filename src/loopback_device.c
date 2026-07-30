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
/* Fixed queued-packet buffer size. Per-unit configurable MTU (from
 * ENV:sana2loop/unitN.config) is a later phase; for now this is also the
 * hard cap enforced against ios2_DataLength. */
#define SANA2LOOP_MAX_PACKET  1500

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
    UBYTE       su_Station[6];        /* fake ethernet station address */
    ULONG       su_MTU;
    BOOL        su_Online;
    struct List su_PendingReads;      /* queued CMD_READ IORequests awaiting data */
    struct List su_PendingData;       /* echoed packets awaiting a CMD_READ */
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

/* Aborts any request still queued (reads or event waits) under a cookie
 * that's about to be freed -- a well-behaved consumer closes with nothing
 * outstanding, but a test device should survive one that doesn't rather
 * than hand a freed cookie's hooks to a later packet delivery. */
static void abort_requests_for_open(struct Sana2LoopUnit *unit, struct Sana2LoopOpen *slo)
{
    struct List *lists[2];
    int li;

    lists[0] = &unit->su_PendingReads;
    lists[1] = &unit->su_EventReqs;
    for (li = 0; li < 2; li++) {
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
    return slo->slo_CopyToBuff(io->ios2_Data, pkt->ep_Data, pkt->ep_Length);
}

/* Complete any queued S2_ONEVENT request whose mask matches `event`.
 * Edge-triggered by design: a request queued while already "true" (e.g.
 * S2_ONEVENT for S2EVENT_ONLINE sent to an already-online unit) waits for
 * the *next* transition, not the current state -- matching how a caller
 * uses it as a wait-for-next-change primitive rather than a status poll
 * (S2_DEVICEQUERY / a future S2_GETGLOBALSTATS is the status-poll path). */
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

    while ((io = (struct IOSana2Req *)safe_rem_head(&unit->su_PendingReads)) != NULL) {
        io->ios2_Req.io_Error = IOERR_ABORTED;
        ReplyMsg(&io->ios2_Req.io_Message);
    }
    while ((io = (struct IOSana2Req *)safe_rem_head(&unit->su_EventReqs)) != NULL) {
        io->ios2_Req.io_Error = IOERR_ABORTED;
        ReplyMsg(&io->ios2_Req.io_Message);
    }
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
        u->su_MTU    = SANA2LOOP_DEFAULT_MTU;
        u->su_Online = FALSE;
        /* Locally-administered (bit set in the first octet) fake address,
         * spelling "S2L" in the middle bytes so it's recognisable in a
         * packet dump; last byte is the unit number. */
        u->su_Station[0] = 0x02;
        u->su_Station[1] = 0x53;
        u->su_Station[2] = 0x32;
        u->su_Station[3] = 0x4C;
        u->su_Station[4] = 0x00;
        u->su_Station[5] = (UBYTE)i;

        list_init(&u->su_PendingReads);
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
    struct Sana2LoopOpen *slo;

    io->io_Error = S2ERR_BAD_ARGUMENT;
    io->io_Message.mn_Node.ln_Type = NT_REPLYMSG;

    if (unitnum >= SANA2LOOP_NUM_UNITS)
        return;

    slo = AllocMem(sizeof(struct Sana2LoopOpen), MEMF_CLEAR);
    if (!slo) {
        io->io_Error = S2ERR_NO_RESOURCES;
        return;
    }
    slo->slo_Unit  = &sld->sld_Units[unitnum];
    slo->slo_Magic = SANA2LOOP_OPEN_MAGIC;
    /* ios2_BufferManagement arrives pointing at the caller's tag list (or
     * NULL); apply_buffer_mgmt_tags() reads it before we overwrite the
     * field below with our own cookie. */
    apply_buffer_mgmt_tags(slo, (struct TagItem *)io2->ios2_BufferManagement);

    io->io_Device = (struct Device *)dev;
    io->io_Unit   = &sld->sld_Units[unitnum].su_Unit;
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

/* device dependent beginio function */
static void __attribute__((used))
begin_io(struct Library *dev asm("a6"), struct IORequest *io0 asm("a1"))
{
    struct IOSana2Req *io = (struct IOSana2Req *)io0;
    struct Sana2LoopUnit *unit = (struct Sana2LoopUnit *)io0->io_Unit;
    (void)dev;

    io0->io_Error = 0;

    switch (io0->io_Command) {

    case CMD_WRITE: {
        struct EchoPacket *pkt;
        struct IOSana2Req *waiting;
        struct Sana2LoopOpen *writer_slo = (struct Sana2LoopOpen *)io->ios2_BufferManagement;

        if (!unit->su_Online) {
            io0->io_Error = S2ERR_OUTOFSERVICE;
            break;
        }
        if (io->ios2_DataLength > unit->su_MTU || io->ios2_DataLength > SANA2LOOP_MAX_PACKET) {
            io0->io_Error = S2ERR_MTU_EXCEEDED;
            break;
        }

        pkt = AllocMem(sizeof(struct EchoPacket), MEMF_CLEAR);
        if (!pkt) {
            io0->io_Error = S2ERR_NO_RESOURCES;
            break;
        }
        pkt->ep_Length     = io->ios2_DataLength;
        pkt->ep_PacketType = io->ios2_PacketType;
        /* Echo semantics: the address we were asked to send TO becomes the
         * src the reader sees; the reader's dst is our own station address
         * -- "src/dst addresses swapped", per the proposal. */
        CopyMem(io->ios2_DstAddr, pkt->ep_SrcAddr, SANA2_MAX_ADDR_BYTES);
        {
            int k;
            for (k = 0; k < SANA2_MAX_ADDR_BYTES; k++)
                pkt->ep_DstAddr[k] = (k < 6) ? unit->su_Station[k] : 0;
        }
        if (!writer_slo->slo_CopyFromBuff(pkt->ep_Data, io->ios2_Data, pkt->ep_Length)) {
            FreeMem(pkt, sizeof(struct EchoPacket));
            io0->io_Error = S2ERR_NO_RESOURCES;
            io->ios2_WireError = S2WERR_BUFF_ERROR;
            break;
        }

        waiting = (struct IOSana2Req *)safe_rem_head(&unit->su_PendingReads);
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
        } else {
            safe_add_tail(&unit->su_PendingData, (struct Node *)pkt);
        }
        break;
    }

    case CMD_READ: {
        struct EchoPacket *pkt = (struct EchoPacket *)safe_rem_head(&unit->su_PendingData);
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

    case CMD_FLUSH: {
        struct IOSana2Req *pending;
        while ((pending = (struct IOSana2Req *)safe_rem_head(&unit->su_PendingReads)) != NULL) {
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
        ULONG mine = sizeof(struct Sana2DeviceQuery);
        ULONG given = dq->SizeAvailable;

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
        int k;
        for (k = 0; k < SANA2_MAX_ADDR_BYTES; k++) {
            io->ios2_SrcAddr[k] = (k < 6) ? unit->su_Station[k] : 0;
            io->ios2_DstAddr[k] = (k < 6) ? unit->su_Station[k] : 0;
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

    case S2_ONEVENT:
        io0->io_Flags &= ~IOF_QUICK;
        safe_add_tail(&unit->su_EventReqs, (struct Node *)io);
        return; /* completed later by fire_event() */

    /* Later phases: S2_CONFIGINTERFACE, S2_ADD/DELMULTICASTADDRESS,
     * S2_MULTICAST/BROADCAST send flags, S2_TRACKTYPE/UNTRACKTYPE,
     * S2_GETTYPESTATS/GETSPECIALSTATS/GETGLOBALSTATS, S2_READORPHAN. */
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
