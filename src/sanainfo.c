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
 * Usage: SanaInfo [unit] [ONLINE|OFFLINE]
 *   No arguments: opens unit 0, prints S2_DEVICEQUERY + station address.
 *   ONLINE/OFFLINE: issues S2_ONLINE or S2_OFFLINE first, then queries.
 */

#include <exec/types.h>
#include <exec/io.h>
#include <exec/errors.h>
#include <devices/sana2.h>
#include <proto/exec.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    int i;
    int rc = 0;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "ONLINE") || !strcmp(argv[i], "OFFLINE"))
            mode = argv[i];
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

    if (OpenDevice((STRPTR)"loopback.device", unit, (struct IORequest *)io, 0)) {
        fprintf(stderr, "SanaInfo: can't open loopback.device unit %d\n", (int)unit);
        DeleteIORequest((struct IORequest *)io);
        DeleteMsgPort(port);
        return 20;
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
