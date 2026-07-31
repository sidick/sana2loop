/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Simon Dick
 */

/* sanasend.c -- SanaSend, a Shell tool that writes every record in a
 * .pcap file to a loopback.device unit as a real CMD_WRITE.
 *
 * Normal AmigaOS 2.0+/clib2, like SanaDump -- see that file's own header
 * comment for why (and for sana2open.c's "model citizen" open helper).
 *
 * Usage: SanaSend UNIT/A/N,FILE/A,DEST/K,TYPE/K,DELAY/K/N
 *   UNIT   - unit number to open.
 *   FILE   - input .pcap path.
 *   DEST   - override destination address, "aa:bb:cc:dd:ee:ff"; omitted =
 *            use each record's own captured destination.
 *   TYPE   - override packet type in hex (e.g. "0800"); omitted = use
 *            each record's own captured type.
 *   DELAY  - dos.library Delay() ticks between writes; omitted = 0 (as
 *            fast as the device accepts them).
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

#define TEMPLATE "UNIT/A/N,FILE/A,DEST/K,TYPE/K,DELAY/K/N"
enum { ARG_UNIT, ARG_FILE, ARG_DEST, ARG_TYPE, ARG_DELAY, ARG_COUNT_TOTAL };

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

int main(int argc, char **argv)
{
    LONG args[ARG_COUNT_TOTAL];
    struct RDArgs *rda;
    struct Sana2Open so;
    struct PcapReader reader;
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
        type = strtoul((char *)args[ARG_TYPE], NULL, 16);
        type_override = TRUE;
    }
    if (args[ARG_DELAY])
        delay_ticks = (ULONG)*(LONG *)args[ARG_DELAY];

    if (!pcapio_open_read(&reader, (char *)args[ARG_FILE])) {
        fprintf(stderr, "SanaSend: can't open %s as a pcap capture\n", (char *)args[ARG_FILE]);
        FreeArgs(rda);
        return 20;
    }

    if (!sana2_open(&so, "loopback.device", (LONG)unit, 0)) {
        fprintf(stderr, "SanaSend: can't open loopback.device unit %lu\n", (unsigned long)unit);
        pcapio_close_read(&reader);
        FreeArgs(rda);
        return 20;
    }

    so.io->ios2_Req.io_Command = S2_ONLINE;
    DoIO((struct IORequest *)so.io);

    printf("SanaSend: sending %s to unit %lu\n", (char *)args[ARG_FILE], (unsigned long)unit);

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

    sana2_close(&so);
    pcapio_close_read(&reader);
    FreeArgs(rda);

    printf("SanaSend: sent %lu packet%s\n", (unsigned long)sent, sent == 1 ? "" : "s");
    if (skipped)
        printf("SanaSend: skipped %lu record%s shorter than an Ethernet header\n",
               (unsigned long)skipped, skipped == 1 ? "" : "s");
    return rc;
}
