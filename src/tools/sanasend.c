/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Simon Dick
 */

/* sanasend.c -- SanaSend, a Shell tool that writes every record in a
 * .pcap file to a loopback.device unit as a real CMD_WRITE.
 *
 * Normal AmigaOS 2.0+/clib2, like SanaDump -- see that file's own header
 * comment for why (and for sana2open.c's "model citizen" open helper).
 *
 * Usage: SanaSend UNIT/A/N,FILE/A,DEST/K,TYPE/K,DELAY/K/N,DEVICE/K,
 *                 CONFIG/S,ONLINE/S
 *   UNIT   - unit number to open.
 *   FILE   - input .pcap path.
 *   DEST   - override destination address, "aa:bb:cc:dd:ee:ff"; omitted =
 *            use each record's own captured destination.
 *   TYPE   - override packet type in hex (e.g. "0800"); omitted = use
 *            each record's own captured type.
 *   DELAY  - dos.library Delay() ticks between writes; omitted = 0 (as
 *            fast as the device accepts them).
 *   DEVICE - SANA-II device to open; omitted = loopback.device. Not
 *            limited to this project's own device -- see
 *            userdocs/SanaDump-and-SanaSend.md's "Other drivers" section.
 *   CONFIG - issue S2_CONFIGINTERFACE first, using whatever address
 *            S2_GETSTATIONADDRESS reports as the driver's own factory
 *            default -- see sanadump.c's matching CONFIG comment for why
 *            and when this matters (a real driver with no stack already
 *            running; sana2loop's own units never need it).
 *   ONLINE - bring the unit online before sending and back offline on
 *            exit -- own the whole online lifecycle for this session.
 *            Omitted (the default) assumes a protocol stack already
 *            manages online state and doesn't touch it -- see
 *            sanadump.c's matching ONLINE comment for the full reasoning.
 *
 * A real constraint worth knowing before reaching for this tool: SANA-II
 * (and this device) never lets CMD_WRITE fake an arbitrary SOURCE address
 * -- the delivered frame's source is always this unit's own real station
 * address (or, for echo mode specifically, the destination you wrote,
 * per that mode's own swapped-echo semantics -- see
 * userdocs/Echo-and-Crossover.md). SanaSend replays a capture's
 * destination, type, and payload faithfully; the resulting frame's
 * SOURCE is whatever the device itself assigns, never the original
 * capture's source byte-for-byte. This models a real driver correctly
 * (you can't spoof your own MAC on real hardware either), it just means
 * "replaying a capture" isn't a byte-for-byte wire replay of the
 * original frame.
 */

#include <exec/types.h>
#include <exec/io.h>
#include <exec/errors.h>
#include <devices/sana2.h>
#include <dos/rdargs.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sana2open.h"
#include "pcapio.h"

#define STR(s)  #s
#define XSTR(s) STR(s)

#ifndef SANASEND_VERSION
#define SANASEND_VERSION 1
#endif
#ifndef SANASEND_REVISION
#define SANASEND_REVISION 0
#endif

static volatile char version[] =
    "$VER: SanaSend " XSTR(SANASEND_VERSION) "." XSTR(SANASEND_REVISION) " (30.07.2026)";

/* AmigaDOS's own "$STACK:" cookie (RKRM: DOS, "The Stack Cookie") --
 * scanned by the Shell/Workbench at load time and used to RAISE (never
 * lower) the process's stack if the caller's own Stack/WBStartup
 * tooltype setting is smaller. A plain Shell CLI process's default stack
 * is a mere 4 KB, and this program's own main() alone (a full
 * MAX_FRAME-sized frame buffer, a Sana2Open, a PcapReader, ReadArgs'
 * result array) already eats a real fraction of that BEFORE accounting
 * for clib2's own startup/library overhead or any call depth into
 * ReadArgs()/etc. 16 KB is comfortably generous, not just "enough". */
static volatile char stack_cookie[] = "$STACK: 16384";

#define TEMPLATE "UNIT/A/N,FILE/A,DEST/K,TYPE/K,DELAY/K/N,DEVICE/K,CONFIG/S,ONLINE/S"
enum { ARG_UNIT, ARG_FILE, ARG_DEST, ARG_TYPE, ARG_DELAY, ARG_DEVICE, ARG_CONFIG,
       ARG_ONLINE, ARG_COUNT_TOTAL };

#define MAX_FRAME 1514
#define ETH_HDR_SIZE 14

/* Parses "aa:bb:cc:dd:ee:ff" (colon-separated hex bytes, any case) into
 * 6 bytes. FALSE on anything else -- a malformed DEST is exactly as
 * fatal as a malformed pcap record elsewhere in this project (fail-fast,
 * not a silent best-effort partial parse). */
static BOOL parse_mac(const char *s, UBYTE *out)
{
    int i;
    for (i = 0; i < 6; i++) {
        char *end;
        unsigned long v = strtoul(s, &end, 16);
        if (end == s || v > 0xFF)
            return FALSE;
        out[i] = (UBYTE)v;
        s = end;
        if (i < 5) {
            if (*s != ':')
                return FALSE;
            s++;
        }
    }
    return *s == '\0';
}

/* Parses a bare-hex packet type ("0800", any case -- exactly the form the
 * usage text and userdocs/SanaDump-and-SanaSend.md document; no "0x"
 * prefix, since none is documented) into a 16-bit value. FALSE on
 * anything else. A bare strtoul() here would silently turn garbage into
 * type 0 and wave through values past 0xFFFF -- and a wrong TYPE is
 * exactly as fatal as a malformed pcap record elsewhere in this project
 * (fail-fast, not a silent best-effort parse). Kept byte-for-byte
 * identical to sanadump.c's own copy. */
static BOOL parse_type(const char *s, ULONG *out)
{
    char *end;
    unsigned long v;
    const char *p;

    if (*s == '\0')
        return FALSE;
    for (p = s; *p != '\0'; p++) {
        if (!((*p >= '0' && *p <= '9') ||
              (*p >= 'a' && *p <= 'f') ||
              (*p >= 'A' && *p <= 'F')))
            return FALSE;
    }
    v = strtoul(s, &end, 16);
    if (*end != '\0' || v > 0xFFFFUL)
        return FALSE;
    *out = (ULONG)v;
    return TRUE;
}

int main(int argc, char **argv)
{
    LONG args[ARG_COUNT_TOTAL];
    struct RDArgs *rda;
    struct Sana2Open so;
    struct PcapReader reader;
    const char *device = "loopback.device";
    ULONG unit;
    ULONG delay_ticks = 0;
    BOOL dest_override = FALSE, type_override = FALSE;
    UBYTE dest[6];
    ULONG type = 0;
    ULONG sent = 0, skipped = 0;
    /* static, not a stack local -- see stack_cookie's own comment: this
     * alone is over a third of a plain Shell CLI process's default 4 KB
     * stack. */
    static UBYTE frame[MAX_FRAME];
    int rc = 0;

    (void)argc;
    (void)argv;

    memset(args, 0, sizeof(args));
    rda = ReadArgs((STRPTR)TEMPLATE, args, NULL);
    if (!rda) {
        PrintFault(IoErr(), (STRPTR)"SanaSend");
        return 20;
    }

    unit = (ULONG)*(LONG *)args[ARG_UNIT];
    if (args[ARG_DEST]) {
        if (!parse_mac((char *)args[ARG_DEST], dest)) {
            fprintf(stderr, "SanaSend: bad DEST %s (want aa:bb:cc:dd:ee:ff)\n",
                    (char *)args[ARG_DEST]);
            FreeArgs(rda);
            return 20;
        }
        dest_override = TRUE;
    }
    if (args[ARG_TYPE]) {
        if (!parse_type((char *)args[ARG_TYPE], &type)) {
            fprintf(stderr, "SanaSend: bad TYPE %s (want bare hex 0000-ffff, e.g. 0800)\n",
                    (char *)args[ARG_TYPE]);
            FreeArgs(rda);
            return 20;
        }
        type_override = TRUE;
    }
    if (args[ARG_DELAY]) {
        /* ReadArgs /N parses a SIGNED LONG -- a negative DELAY cast
         * straight into a ULONG would hand Delay() a count in the
         * billions of ticks (years of wall-clock hang, uninterruptible
         * short of a reboot), so reject it here, same fail-fast stance
         * as a malformed DEST above. */
        LONG d = *(LONG *)args[ARG_DELAY];
        if (d < 0) {
            fprintf(stderr, "SanaSend: bad DELAY %ld (must be >= 0)\n", (long)d);
            FreeArgs(rda);
            return 20;
        }
        delay_ticks = (ULONG)d;
    }

    if (args[ARG_DEVICE])
        device = (char *)args[ARG_DEVICE];

    if (!pcapio_open_read(&reader, (char *)args[ARG_FILE])) {
        fprintf(stderr, "SanaSend: can't open %s as a pcap capture\n", (char *)args[ARG_FILE]);
        FreeArgs(rda);
        return 20;
    }

    if (!sana2_open(&so, device, (LONG)unit, 0)) {
        fprintf(stderr, "SanaSend: can't open %s unit %lu\n", device, (unsigned long)unit);
        pcapio_close_read(&reader);
        FreeArgs(rda);
        return 20;
    }

    /* CONFIG/ONLINE (opt-in, #35/#36) -- see sanadump.c's matching
     * comments for the full reasoning; kept byte-for-byte equivalent
     * here. */
    if (args[ARG_CONFIG]) {
        so.io->ios2_Req.io_Command = S2_GETSTATIONADDRESS;
        DoIO((struct IORequest *)so.io);
        if (so.io->ios2_Req.io_Error != 0) {
            fprintf(stderr, "SanaSend: S2_GETSTATIONADDRESS failed, io_Error=%d\n",
                    so.io->ios2_Req.io_Error);
        } else {
            memcpy(so.io->ios2_SrcAddr, so.io->ios2_DstAddr, SANA2_MAX_ADDR_BYTES);
            so.io->ios2_Req.io_Command = S2_CONFIGINTERFACE;
            DoIO((struct IORequest *)so.io);
            if (so.io->ios2_Req.io_Error == 0)
                printf("SanaSend: configured with the driver's own factory address\n");
            else if (so.io->ios2_WireError == S2WERR_IS_CONFIGURED)
                printf("SanaSend: already configured -- leaving as-is\n");
            else
                fprintf(stderr, "SanaSend: S2_CONFIGINTERFACE failed, io_Error=%d\n",
                        so.io->ios2_Req.io_Error);
        }
    }

    if (args[ARG_ONLINE]) {
        so.io->ios2_Req.io_Command = S2_ONLINE;
        DoIO((struct IORequest *)so.io);
    }

    printf("SanaSend: sending %s to unit %lu on %s\n", (char *)args[ARG_FILE],
           (unsigned long)unit, device);

    for (;;) {
        struct pcap_pkthdr hdr;
        BOOL eof;
        int k;

        if (!pcapio_read_packet(&reader, &hdr, frame, sizeof(frame), &eof)) {
            if (!eof) {
                fprintf(stderr, "SanaSend: %s: read error or record too large\n",
                        (char *)args[ARG_FILE]);
                rc = 10;
            }
            break;
        }

        if (hdr.incl_len < ETH_HDR_SIZE) {
            skipped++; /* shorter than a real Ethernet header -- not a
                          frame this device (or any real one) could ever
                          have sent; skip rather than fail the whole run */
            continue;
        }

        so.io->ios2_Req.io_Command = CMD_WRITE;
        for (k = 0; k < SANA2_MAX_ADDR_BYTES; k++)
            so.io->ios2_DstAddr[k] = (k < 6) ? (dest_override ? dest[k] : frame[k]) : 0;
        so.io->ios2_PacketType = type_override ? type
                                : (((ULONG)frame[12] << 8) | (ULONG)frame[13]);
        so.io->ios2_Data       = frame + ETH_HDR_SIZE;
        so.io->ios2_DataLength = hdr.incl_len - ETH_HDR_SIZE;
        so.io->ios2_Req.io_Flags = 0;
        DoIO((struct IORequest *)so.io);

        if (so.io->ios2_Req.io_Error != 0) {
            fprintf(stderr, "SanaSend: write failed, io_Error=%d\n",
                    so.io->ios2_Req.io_Error);
            rc = 10;
            break;
        }
        sent++;

        if (delay_ticks)
            Delay((LONG)delay_ticks);
    }

    if (args[ARG_ONLINE]) {
        so.io->ios2_Req.io_Command = S2_OFFLINE;
        DoIO((struct IORequest *)so.io);
    }

    sana2_close(&so);
    pcapio_close_read(&reader);
    FreeArgs(rda);

    printf("SanaSend: sent %lu packet%s\n", (unsigned long)sent, sent == 1 ? "" : "s");
    if (skipped)
        printf("SanaSend: skipped %lu record%s shorter than an Ethernet header\n",
               (unsigned long)skipped, skipped == 1 ? "" : "s");
    return rc;
}
