/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Simon Dick
 */

/* sanainfo.c -- SanaInfo, the Phase 1 validation tool for loopback.device.
 * A normal Shell CLI program (not the device itself), so unlike
 * loopback.device it is free to require Kickstart 2.0+ (CreateMsgPort()/
 * CreateIORequest() are V36+) -- per the project proposal, companion tools
 * may be "documented as 2.0+ conveniences" even though the device stays
 * 1.3-compatible.
 *
 * Usage: SanaInfo [unit] [ONLINE|OFFLINE] [CONFIG] [DEVICE=<name>]
 *   No arguments: opens unit 0 of loopback.device, prints
 *     S2_DEVICEQUERY + station address.
 *   ONLINE/OFFLINE: issues S2_ONLINE or S2_OFFLINE first, then queries.
 *   CONFIG: issues S2_CONFIGINTERFACE first, using whatever address
 *     S2_GETSTATIONADDRESS reports as the driver's own factory default
 *     -- for a real driver with no protocol stack already running to
 *     configure it (sana2loop's own units never need this). A no-op
 *     (reported, not fatal) if already configured. Independent of
 *     ONLINE/OFFLINE -- combine freely, e.g. "SanaInfo 0 CONFIG ONLINE".
 *   DEVICE=<name>: SANA-II device to open; omitted = loopback.device.
 *     Not limited to this project's own device.
 */

#include <exec/types.h>
#include <exec/io.h>
#include <exec/errors.h>
#include <devices/sana2.h>
#include <proto/exec.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STR(s)  #s
#define XSTR(s) STR(s)

#ifndef SANAINFO_VERSION
#define SANAINFO_VERSION 1
#endif
#ifndef SANAINFO_REVISION
#define SANAINFO_REVISION 0
#endif

/* Standard AmigaDOS "$VER:" version cookie (RKRM: DOS, "The Version
 * Cookie") -- what the Shell's Version command scans a compiled
 * executable's segments for. `static volatile` (not `static const`) so
 * -O2 can't conclude the never-read array is dead and drop it -- the
 * whole point is that ITS BYTES are read by an external tool, not by
 * this program. dd.mm.yyyy: the cookie's date field is three decimal
 * numbers, not a textual month. */
static volatile char version[] =
    "$VER: SanaInfo " XSTR(SANAINFO_VERSION) "." XSTR(SANAINFO_REVISION) " (04.08.2026)";

static void print_addr(const char *label, const UBYTE *addr)
{
    printf("%s: %02x:%02x:%02x:%02x:%02x:%02x\n", label,
           addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
}

int main(int argc, char **argv)
{
    struct MsgPort *port;
    struct IOSana2Req *io;
    LONG unit = 0;
    const char *mode = NULL;
    const char *device = "loopback.device";
    BOOL do_config = FALSE;
    int i;
    int rc = 0;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "ONLINE") || !strcmp(argv[i], "OFFLINE"))
            mode = argv[i];
        else if (!strcmp(argv[i], "CONFIG"))
            do_config = TRUE;
        else if (!strncmp(argv[i], "DEVICE=", 7))
            device = argv[i] + 7;
        else
            unit = atol(argv[i]);
    }

    port = CreateMsgPort();
    if (!port) {
        fprintf(stderr, "SanaInfo: can't create message port\n");
        return 20;
    }

    io = (struct IOSana2Req *)CreateIORequest(port, sizeof(struct IOSana2Req));
    if (!io) {
        DeleteMsgPort(port);
        fprintf(stderr, "SanaInfo: can't create IO request\n");
        return 20;
    }

    if (OpenDevice((STRPTR)device, unit, (struct IORequest *)io, 0)) {
        fprintf(stderr, "SanaInfo: can't open %s unit %d\n", device, (int)unit);
        DeleteIORequest((struct IORequest *)io);
        DeleteMsgPort(port);
        return 20;
    }

    /* CONFIG: most real drivers have no usable station address until a
     * stack calls S2_CONFIGINTERFACE -- sana2loop's own units never need
     * this. Uses whatever S2_GETSTATIONADDRESS already reports as the
     * FACTORY default (ios2_DstAddr), matching sanamon's own documented
     * "configure interface with its default address" behavior.
     * S2WERR_IS_CONFIGURED (already configured, e.g. by a real stack) is
     * reported, not fatal -- S2_CONFIGINTERFACE's own one-shot contract
     * makes that a normal outcome. Done before the mode/query steps
     * below so they reflect any address just configured here. */
    if (do_config) {
        io->ios2_Req.io_Command = S2_GETSTATIONADDRESS;
        DoIO((struct IORequest *)io);
        if (io->ios2_Req.io_Error != 0) {
            fprintf(stderr, "SanaInfo: S2_GETSTATIONADDRESS failed, io_Error=%d\n",
                    io->ios2_Req.io_Error);
        } else {
            memcpy(io->ios2_SrcAddr, io->ios2_DstAddr, SANA2_MAX_ADDR_BYTES);
            io->ios2_Req.io_Command = S2_CONFIGINTERFACE;
            DoIO((struct IORequest *)io);
            if (io->ios2_Req.io_Error == 0)
                printf("SanaInfo: configured with the driver's own factory address\n");
            else if (io->ios2_WireError == S2WERR_IS_CONFIGURED)
                printf("SanaInfo: already configured -- leaving as-is\n");
            else
                fprintf(stderr, "SanaInfo: S2_CONFIGINTERFACE failed, io_Error=%d\n",
                        io->ios2_Req.io_Error);
        }
    }

    if (mode) {
        io->ios2_Req.io_Command = !strcmp(mode, "ONLINE") ? S2_ONLINE : S2_OFFLINE;
        DoIO((struct IORequest *)io);
        printf("SanaInfo: unit %d set %s (io_Error=%d)\n", (int)unit, mode, io->ios2_Req.io_Error);
    }

    {
        struct Sana2DeviceQuery dq;
        dq.SizeAvailable = sizeof(dq);
        io->ios2_Req.io_Command = S2_DEVICEQUERY;
        io->ios2_StatData = &dq;
        DoIO((struct IORequest *)io);
        if (io->ios2_Req.io_Error) {
            fprintf(stderr, "SanaInfo: S2_DEVICEQUERY failed, io_Error=%d\n", io->ios2_Req.io_Error);
            rc = 10;
        } else {
            printf("MTU: %lu\n", (unsigned long)dq.MTU);
            printf("BPS: %lu\n", (unsigned long)dq.BPS);
            printf("HardwareType: %lu (1=Ethernet)\n", (unsigned long)dq.HardwareType);
            printf("AddrFieldSize: %u bits\n", dq.AddrFieldSize);
        }
    }

    io->ios2_Req.io_Command = S2_GETSTATIONADDRESS;
    DoIO((struct IORequest *)io);
    if (io->ios2_Req.io_Error) {
        fprintf(stderr, "SanaInfo: S2_GETSTATIONADDRESS failed, io_Error=%d\n", io->ios2_Req.io_Error);
        rc = 10;
    } else {
        print_addr("Station address", io->ios2_SrcAddr);
    }

    CloseDevice((struct IORequest *)io);
    DeleteIORequest((struct IORequest *)io);
    DeleteMsgPort(port);
    return rc;
}
