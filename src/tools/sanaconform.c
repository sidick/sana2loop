/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Simon Dick
 */

/* sanaconform.c -- SanaConform, a SANA-II Rev 2/3/4/7 conformance/quirk
 * probe for loopback.device (docs/PLAN.md's M13).
 *
 * A normal AmigaOS 2.0+/clib2 CLI program, like SanaInfo/SanaDump/SanaSend
 * -- not bound to the device's own Kickstart 1.3 floor. Unlike those
 * tools, this one deliberately does its OWN manual OpenDevice() with a
 * richer tag list than sana2open.c's shared 3-tag helper (S2_CopyToBuff/
 * CopyFromBuff/PacketFilter only): SanaConform also needs to negotiate
 * the Rev 2/3 16/32-bit hooks and Rev 7 DMA hooks sana2open.c was never
 * meant to carry, so extending its fixed-size array for one tool's
 * specialized probing isn't worth complicating SanaDump/SanaSend's own
 * simpler, stable contract.
 *
 * Probes both conformance directions docs/PLAN.md's M13 section calls
 * for:
 *   - "driver advertising support": does OpenDevice() accept every
 *     Rev 2/3/7 tag without erroring, and does S2_DEVICEQUERY report
 *     Rev 4's RawMTU field?
 *   - "stack actually invoking it": which negotiated buffer-hook width
 *     (base/16/32) does a real self-echo round-trip actually get
 *     completed through -- instrumented counting hooks answer this
 *     directly, the same technique tests/copperline/sana2test.c's own
 *     "buffhooks" sub-test uses on-target.
 * Every Rev 4/7 addition is probed with GRACEFUL FALLBACK in mind: an
 * older driver (or a differently-configured unit -- e.g. crossover-mode,
 * which never self-delivers) that doesn't behave like a fresh echo unit
 * is reported as such (informational), not treated as a probe failure.
 * This tool is meant to run against loopback.device in whatever
 * config/mode a given unit is actually in, not just this project's own
 * on-target test fixtures.
 *
 * Usage: SanaConform UNIT/N
 *   UNIT - unit number to probe (default 0).
 */

#include <exec/types.h>
#include <exec/io.h>
#include <exec/errors.h>
#include <devices/sana2.h>
#include <utility/tagitem.h>
#include <dos/rdargs.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STR(s)  #s
#define XSTR(s) STR(s)

#ifndef SANACONFORM_VERSION
#define SANACONFORM_VERSION 1
#endif
#ifndef SANACONFORM_REVISION
#define SANACONFORM_REVISION 0
#endif

/* See sanainfo.c's matching comment for why `volatile`, not `const`, and
 * the numeric dd.mm.yyyy date. */
static volatile char version[] =
    "$VER: SanaConform " XSTR(SANACONFORM_VERSION) "." XSTR(SANACONFORM_REVISION) " (31.07.2026)";

/* See sanadump.c's matching comment: a plain Shell CLI process's default
 * stack is a mere 4 KB. */
static volatile char stack_cookie[] = "$STACK: 16384";

#define TEMPLATE "UNIT/N"
enum { ARG_UNIT, ARG_COUNT_TOTAL };

/* M13: this toolchain's <devices/sana2.h> has no Rev 4/7 additions at all
 * -- same gap, same local #defines as loopback_device.c's own (see that
 * file's comment for where these numbers come from: wiki.amigaos.net,
 * cross-checked against the header's existing 0xc000/0xc001 sequence for
 * the two command values; the 64-bit DMA tags are this project's own
 * inferred continuation of S2_Dummy+4..+9, harmless if wrong since this
 * probe only ever counts whether ITS OWN stub got called -- it never
 * assumes a specific driver behavior from the tag value alone). */
#define S2_GETPEERADDRESS    0xC002
#define S2_GETDNSADDRESS     0xC003
#define S2_DMACopyToBuff64   (S2_Dummy + 10)
#define S2_DMACopyFromBuff64 (S2_Dummy + 11)

/* Rev 4's RawMTU extension to Sana2DeviceQuery -- same local copy, same
 * reasoning as loopback_device.c's own struct Sana2LoopDeviceQuery. */
struct Sana2LoopDeviceQuery {
    ULONG SizeAvailable;
    ULONG SizeSupplied;
    ULONG DevQueryFormat;
    ULONG DeviceLevel;
    UWORD AddrFieldSize;
    ULONG MTU;
    ULONG BPS;
    ULONG HardwareType;
    ULONG RawMTU;
};

/* This project's own S2_GETSPECIALSTATS record type for DMA-hook
 * negotiation (loopback_device.c's SANA2LOOP_SPECIALSTAT_DMA_HOOKS_OFFERED)
 * -- NOT a spec value, sana2loop-specific, checked only as a best-effort
 * "quirk" extra (see this file's own header comment on scope); a
 * different SANA-II driver simply won't have this stat type at all,
 * which is fine -- see the RecordCountSupplied==0 handling below. */
#define SANA2LOOP_SPECIALSTAT_DMA_HOOKS_OFFERED 2

/* Instrumented buffer-management hooks -- real CopyMem-backed hooks (so a
 * self-echo round trip's data still arrives correctly through whichever
 * one the driver picks), each incrementing its own counter so this probe
 * can report EXACTLY which negotiated width actually got invoked. */
static ULONG g_ToBase, g_FromBase, g_To16, g_From16, g_To32, g_From32, g_DMACalls;

static BOOL hook_to_base(APTR to asm("a0"), APTR from asm("a1"), ULONG size asm("d0"))
{
    CopyMem(from, to, size);
    g_ToBase++;
    return TRUE;
}
static BOOL hook_from_base(APTR to asm("a0"), APTR from asm("a1"), ULONG size asm("d0"))
{
    CopyMem(from, to, size);
    g_FromBase++;
    return TRUE;
}
static BOOL hook_to16(APTR to asm("a0"), APTR from asm("a1"), ULONG size asm("d0"))
{
    CopyMem(from, to, size);
    g_To16++;
    return TRUE;
}
static BOOL hook_from16(APTR to asm("a0"), APTR from asm("a1"), ULONG size asm("d0"))
{
    CopyMem(from, to, size);
    g_From16++;
    return TRUE;
}
static BOOL hook_to32(APTR to asm("a0"), APTR from asm("a1"), ULONG size asm("d0"))
{
    CopyMem(from, to, size);
    g_To32++;
    return TRUE;
}
static BOOL hook_from32(APTR to asm("a0"), APTR from asm("a1"), ULONG size asm("d0"))
{
    CopyMem(from, to, size);
    g_From32++;
    return TRUE;
}

/* Registered as S2_DMACopyToBuff32/S2_DMACopyFromBuff32 -- the real DMA
 * hook calling convention doesn't matter here (see loopback_device.c's
 * own comment): this stub only needs to be a distinguishable address that
 * counts whether it was EVER called, since "never called" (a driver
 * gracefully declining DMA) is itself the expected, spec-legal result
 * this probe is checking for. */
static APTR hook_dma(APTR data asm("a0"))
{
    (void)data;
    g_DMACalls++;
    return NULL;
}

static void print_addr(const char *label, const UBYTE *addr)
{
    printf("%s: %02x:%02x:%02x:%02x:%02x:%02x\n", label,
           addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
}

int main(int argc, char **argv)
{
    LONG args[ARG_COUNT_TOTAL];
    struct RDArgs *rda;
    struct MsgPort *port;
    struct IOSana2Req *io;
    struct MsgPort *wport = NULL;
    struct IOSana2Req *wio = NULL;
    struct TagItem tags[9];
    ULONG unit = 0;
    UBYTE station[SANA2_MAX_ADDR_BYTES];
    int rc = 0;

    (void)argc;
    (void)argv;

    memset(args, 0, sizeof(args));
    rda = ReadArgs((STRPTR)TEMPLATE, args, NULL);
    if (!rda) {
        PrintFault(IoErr(), (STRPTR)"SanaConform");
        return 20;
    }
    if (args[ARG_UNIT])
        unit = (ULONG)*(LONG *)args[ARG_UNIT];

    port = CreateMsgPort();
    if (!port) {
        fprintf(stderr, "SanaConform: can't create message port\n");
        FreeArgs(rda);
        return 20;
    }
    io = (struct IOSana2Req *)CreateIORequest(port, sizeof(struct IOSana2Req));
    if (!io) {
        fprintf(stderr, "SanaConform: can't create IO request\n");
        DeleteMsgPort(port);
        FreeArgs(rda);
        return 20;
    }

    tags[0].ti_Tag = S2_CopyToBuff;         tags[0].ti_Data = (ULONG)hook_to_base;
    tags[1].ti_Tag = S2_CopyFromBuff;       tags[1].ti_Data = (ULONG)hook_from_base;
    tags[2].ti_Tag = S2_CopyToBuff16;       tags[2].ti_Data = (ULONG)hook_to16;
    tags[3].ti_Tag = S2_CopyFromBuff16;     tags[3].ti_Data = (ULONG)hook_from16;
    tags[4].ti_Tag = S2_CopyToBuff32;       tags[4].ti_Data = (ULONG)hook_to32;
    tags[5].ti_Tag = S2_CopyFromBuff32;     tags[5].ti_Data = (ULONG)hook_from32;
    tags[6].ti_Tag = S2_DMACopyToBuff32;    tags[6].ti_Data = (ULONG)hook_dma;
    tags[7].ti_Tag = S2_DMACopyFromBuff32;  tags[7].ti_Data = (ULONG)hook_dma;
    tags[8].ti_Tag = TAG_DONE;              tags[8].ti_Data = 0;
    io->ios2_BufferManagement = tags;

    printf("SanaConform: probing loopback.device unit %lu\n", (unsigned long)unit);

    if (OpenDevice((STRPTR)"loopback.device", (LONG)unit, (struct IORequest *)io, 0)) {
        fprintf(stderr, "SanaConform: can't open loopback.device unit %lu\n", (unsigned long)unit);
        DeleteIORequest((struct IORequest *)io);
        DeleteMsgPort(port);
        FreeArgs(rda);
        return 20;
    }
    printf("OpenDevice: OK (Rev 2/3/7 buffer/DMA hooks all accepted without error --\n"
           "  advisory tags are legal to offer even to a driver that ignores them)\n");

    io->ios2_Req.io_Command = S2_ONLINE;
    DoIO((struct IORequest *)io);
    printf("S2_ONLINE: %s\n", io->ios2_Req.io_Error ? "FAILED" : "OK");

    /* --- Rev 4: RawMTU via the extended Sana2DeviceQuery -- graceful
     * fallback: SizeSupplied less than the extended struct's size means
     * the driver only knows about the pre-Rev-4 fields, not an error. --- */
    {
        struct Sana2LoopDeviceQuery dq;
        dq.SizeAvailable = sizeof(dq);
        io->ios2_Req.io_Command = S2_DEVICEQUERY;
        io->ios2_StatData = &dq;
        DoIO((struct IORequest *)io);
        if (io->ios2_Req.io_Error) {
            printf("S2_DEVICEQUERY: FAILED (io_Error=%d)\n", io->ios2_Req.io_Error);
        } else {
            printf("S2_DEVICEQUERY: MTU=%lu BPS=%lu HardwareType=%lu\n",
                   (unsigned long)dq.MTU, (unsigned long)dq.BPS, (unsigned long)dq.HardwareType);
            if (dq.SizeSupplied >= sizeof(dq))
                printf("  Rev 4 RawMTU: %lu (supported)\n", (unsigned long)dq.RawMTU);
            else
                printf("  Rev 4 RawMTU: not supported (SizeSupplied=%lu bytes, pre-Rev-4 driver)\n",
                       (unsigned long)dq.SizeSupplied);
        }
    }

    io->ios2_Req.io_Command = S2_GETSTATIONADDRESS;
    DoIO((struct IORequest *)io);
    if (io->ios2_Req.io_Error == 0) {
        print_addr("Station address", io->ios2_SrcAddr);
        memcpy(station, io->ios2_SrcAddr, SANA2_MAX_ADDR_BYTES);
    } else {
        memset(station, 0, sizeof(station));
    }

    /* --- Rev 4: S2_GETPEERADDRESS/S2_GETDNSADDRESS. Graceful fallback
     * either way is a PASS for this probe: IOERR_NOCMD means the driver
     * simply predates Rev 4, S2ERR_NOT_SUPPORTED means it knows the
     * command but there's genuinely no peer/DNS concept for this unit's
     * current mode (e.g. echo has no real peer), and success means
     * both are meaningful here. --- */
    io->ios2_Req.io_Command = S2_GETPEERADDRESS;
    DoIO((struct IORequest *)io);
    if (io->ios2_Req.io_Error == 0) {
        print_addr("S2_GETPEERADDRESS local", io->ios2_SrcAddr);
        print_addr("S2_GETPEERADDRESS peer ", io->ios2_DstAddr);
    } else if (io->ios2_Req.io_Error == IOERR_NOCMD) {
        printf("S2_GETPEERADDRESS: not implemented (pre-Rev-4 driver)\n");
    } else if (io->ios2_Req.io_Error == S2ERR_NOT_SUPPORTED) {
        printf("S2_GETPEERADDRESS: not supported for this unit's mode (no real peer)\n");
    } else {
        printf("S2_GETPEERADDRESS: io_Error=%d\n", io->ios2_Req.io_Error);
    }

    io->ios2_Req.io_Command = S2_GETDNSADDRESS;
    DoIO((struct IORequest *)io);
    if (io->ios2_Req.io_Error == 0) {
        print_addr("S2_GETDNSADDRESS primary  ", io->ios2_SrcAddr);
        print_addr("S2_GETDNSADDRESS secondary", io->ios2_DstAddr);
    } else if (io->ios2_Req.io_Error == IOERR_NOCMD) {
        printf("S2_GETDNSADDRESS: not implemented (pre-Rev-4 driver)\n");
    } else if (io->ios2_Req.io_Error == S2ERR_NOT_SUPPORTED) {
        printf("S2_GETDNSADDRESS: not supported for this unit\n");
    } else {
        printf("S2_GETDNSADDRESS: io_Error=%d\n", io->ios2_Req.io_Error);
    }

    /* --- Rev 2/3/7: which negotiated buffer-hook width does a real
     * round-trip actually invoke? Needs a SECOND open on the same unit as
     * a write partner -- a lone unit can't both queue a typed read and
     * issue the matching write on one IORequest at once. Bounded wait
     * (Delay()+CheckIO(), not a blocking WaitIO()): a crossover-mode (or
     * otherwise non-self-delivering) unit will never satisfy this read at
     * all, and that is itself a meaningful, gracefully-reported result,
     * not a probe failure. --- */
    wport = CreateMsgPort();
    wio = wport ? (struct IOSana2Req *)CreateIORequest(wport, sizeof(struct IOSana2Req)) : NULL;
    /* Same tag list as `io`'s own open above: without this, `wio` would
     * negotiate NO hooks at all and the device would silently install its
     * own uninstrumented internal default for the write side, making the
     * "from" (CopyFromBuff) counters below structurally always read zero
     * regardless of what the driver actually does. */
    if (wio)
        wio->ios2_BufferManagement = tags;
    if (wio && OpenDevice((STRPTR)"loopback.device", (LONG)unit, (struct IORequest *)wio, 0) == 0) {
        static UBYTE readbuf[64];
        static char payload[] = "SanaConform-probe-payload";
        int waited;

        io->ios2_Req.io_Command = CMD_READ;
        io->ios2_Data = readbuf;
        io->ios2_PacketType = 0x53C0;
        io->ios2_Req.io_Flags = 0;
        io->ios2_Req.io_Message.mn_Node.ln_Type = NT_MESSAGE;
        SendIO((struct IORequest *)io);

        wio->ios2_Req.io_Command = CMD_WRITE;
        memcpy(wio->ios2_DstAddr, station, SANA2_MAX_ADDR_BYTES);
        wio->ios2_PacketType = 0x53C0;
        wio->ios2_DataLength = sizeof(payload) - 1;
        wio->ios2_Data = (APTR)payload;
        wio->ios2_Req.io_Message.mn_Node.ln_Type = NT_MESSAGE;
        DoIO((struct IORequest *)wio);

        for (waited = 0; waited < 50 && !CheckIO((struct IORequest *)io); waited++)
            Delay(1); /* ~1 tick (1/50s) per iteration, ~1s total ceiling */

        if (CheckIO((struct IORequest *)io)) {
            WaitIO((struct IORequest *)io);
            printf("Self-echo round trip: %s\n",
                   (io->ios2_Req.io_Error == 0 && io->ios2_DataLength == sizeof(payload) - 1)
                       ? "OK" : "completed with an error");
            printf("Buffer hooks actually invoked: ");
            if (g_To32 || g_From32)
                printf("32-bit (to=%lu from=%lu)\n", (unsigned long)g_To32, (unsigned long)g_From32);
            else if (g_To16 || g_From16)
                printf("16-bit (to=%lu from=%lu)\n", (unsigned long)g_To16, (unsigned long)g_From16);
            else if (g_ToBase || g_FromBase)
                printf("base 8-bit (to=%lu from=%lu)\n", (unsigned long)g_ToBase, (unsigned long)g_FromBase);
            else
                printf("none of the negotiated hooks (driver used an internal default)\n");
        } else {
            AbortIO((struct IORequest *)io);
            WaitIO((struct IORequest *)io);
            printf("Self-echo round trip: no response within ~1s -- not an echo-mode\n"
                   "  unit (or this driver doesn't self-deliver); hook-invocation check\n"
                   "  skipped, not a failure\n");
        }
        printf("DMA hooks invoked: %lu (0 expected -- DMA is always advisory, a driver\n"
               "  may legitimately decline every buffer and fall back to CPU copies)\n",
               (unsigned long)g_DMACalls);

        CloseDevice((struct IORequest *)wio);
    } else {
        printf("Self-echo round trip: skipped (couldn't open a second handle on unit %lu)\n",
               (unsigned long)unit);
    }
    if (wio)
        FreeMem(wio, sizeof(struct IOSana2Req));
    if (wport)
        DeleteMsgPort(wport);

    /* --- Quirk extra: this project's own DMA-hook-negotiation stat (see
     * this file's own header comment on scope -- sana2loop-specific, not
     * a SANA-II spec value). Expected to show at least 1 here: THIS
     * probe's own OpenDevice() above already offered S2_DMACopyToBuff32/
     * FromBuff32 (see the `tags[]` list), so this unit's own
     * su_DMAHooksOffered counter should already be >= 1 by the time this
     * query runs. RecordCountSupplied==0 is the graceful, expected result
     * against any OTHER SANA-II driver, which won't have this stat type
     * at all. --- */
    {
        struct {
            struct Sana2SpecialStatHeader hdr;
            struct Sana2SpecialStatRecord recs[4];
        } stats;
        int i;
        BOOL found = FALSE;

        stats.hdr.RecordCountMax = 4;
        io->ios2_Req.io_Command = S2_GETSPECIALSTATS;
        io->ios2_StatData = &stats;
        DoIO((struct IORequest *)io);
        if (io->ios2_Req.io_Error == 0) {
            for (i = 0; i < (int)stats.hdr.RecordCountSupplied; i++)
                if (stats.recs[i].Type == SANA2LOOP_SPECIALSTAT_DMA_HOOKS_OFFERED) {
                    printf("sana2loop quirk stat: %lu open(s) on this unit have offered a DMA hook\n",
                           (unsigned long)stats.recs[i].Count);
                    found = TRUE;
                }
            if (!found)
                printf("sana2loop quirk stat: no DMA-hook-offered record (unexpected -- this "
                       "probe's own open above should have counted as one)\n");
        }
    }

    CloseDevice((struct IORequest *)io);
    DeleteIORequest((struct IORequest *)io);
    DeleteMsgPort(port);
    FreeArgs(rda);
    return rc;
}
