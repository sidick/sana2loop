/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Simon Dick
 */

/* sanadump.c -- SanaDump, a Shell tool that captures whatever a
 * loopback.device unit actually receives into a .pcap file.
 *
 * A normal AmigaOS 2.0+/clib2 CLI program, like SanaInfo -- not bound to
 * the device's own Kickstart 1.3 floor (ReadArgs()/CreateMsgPort()/
 * CreateIORequest() are V36+). Unlike SanaInfo, this one actually reads
 * packet data, so it goes through sana2open.c's "model citizen" open
 * helper (real S2_CopyToBuff/S2_CopyFromBuff hooks) rather than a bare
 * OpenDevice().
 *
 * Usage: SanaDump UNIT/A/N,FILE/A,TYPE/K,PROM/S,COUNT/K/N
 *   UNIT   - unit number to open.
 *   FILE   - output .pcap path.
 *   TYPE   - hex packet type (e.g. "0800") to capture only that type via
 *            a typed CMD_READ; omitted = S2_READORPHAN (any type).
 *   PROM   - open promiscuous (SANA2OPF_PROM) -- meaningful for a
 *            crossover peer's own destination filtering, a no-op on an
 *            echo unit (see userdocs/Echo-and-Crossover.md).
 *   COUNT  - stop after this many packets; omitted = run until Ctrl-C.
 *
 * Forces the unit online at startup (a capture tool that isn't online
 * captures nothing) -- see userdocs/SanaDump-and-SanaSend.md.
 */

#include <exec/types.h>
#include <exec/io.h>
#include <exec/errors.h>
#include <devices/sana2.h>
#include <dos/rdargs.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "sana2open.h"
#include "pcapio.h"

#define STR(s)  #s
#define XSTR(s) STR(s)

#ifndef SANADUMP_VERSION
#define SANADUMP_VERSION 1
#endif
#ifndef SANADUMP_REVISION
#define SANADUMP_REVISION 0
#endif

/* See sanainfo.c's matching comment for why `volatile`, not `const`, and
 * the numeric dd.mm.yyyy date. */
static volatile char version[] =
    "$VER: SanaDump " XSTR(SANADUMP_VERSION) "." XSTR(SANADUMP_REVISION) " (30.07.2026)";

/* See sanasend.c's matching comment: a plain Shell CLI process's default
 * stack is a mere 4 KB, and this program's own frame buffer alone (see
 * MAX_FRAME below, also moved to static storage for the same reason) is
 * over a third of that -- a real, silent-failure risk this project's own
 * on-target test for these tools caught the hard way, not a theoretical
 * concern. */
static volatile char stack_cookie[] = "$STACK: 16384";

#define TEMPLATE "UNIT/A/N,FILE/A,TYPE/K,PROM/S,COUNT/K/N"
enum { ARG_UNIT, ARG_FILE, ARG_TYPE, ARG_PROM, ARG_COUNT, ARG_COUNT_TOTAL };

/* SANA2LOOP_MAX_PACKET-equivalent from the device's own header isn't
 * shared with tools (that header is device-internal) -- 1514 is the
 * largest PAYLOAD this project's own loopback.device can ever deliver:
 * the standard Ethernet MTU (1500, its own default -- see
 * userdocs/Configuration-Reference.md) plus the 14 bytes a SANA2IOF_RAW
 * writer's relaxed MTU check allows on top of that. Note that's payload
 * alone, addresses/type excluded -- see frame[]'s own comment below. */
#define MAX_FRAME 1514
#define ETH_HDR_SIZE 14

/* Parses a bare-hex packet type ("0800", any case -- exactly the form the
 * usage text and userdocs/SanaDump-and-SanaSend.md document; no "0x"
 * prefix, since none is documented) into a 16-bit value. FALSE on
 * anything else. A bare strtoul() here would silently turn garbage into
 * type 0 and wave through values past 0xFFFF -- and a wrong TYPE is
 * exactly as fatal as a malformed pcap record elsewhere in this project
 * (fail-fast, not a silent best-effort parse). Kept byte-for-byte
 * identical to sanasend.c's own copy. */
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
    struct PcapWriter writer;
    ULONG unit;
    ULONG want_count = 0; /* 0 = unlimited */
    ULONG type = 0;
    BOOL typed = FALSE;
    ULONG flags = 0;
    ULONG captured = 0;
    /* Not a stack local -- see stack_cookie's own comment. Sized
     * MAX_FRAME + ETH_HDR_SIZE, not MAX_FRAME: the PAYLOAD alone can be a
     * full MAX_FRAME bytes (a SANA2IOF_RAW writer gets the MTU check
     * relaxed by 14 -- see docs/sana2-notes.md's M4 section), and the
     * hook writes it at frame + ETH_HDR_SIZE, so the header room must be
     * ON TOP of the maximum payload, not carved out of it. */
    static UBYTE frame[MAX_FRAME + ETH_HDR_SIZE];
    int rc = 0;

    (void)argc;
    (void)argv;

    memset(args, 0, sizeof(args));
    rda = ReadArgs((STRPTR)TEMPLATE, args, NULL);
    if (!rda) {
        PrintFault(IoErr(), (STRPTR)"SanaDump");
        return 20;
    }

    unit = (ULONG)*(LONG *)args[ARG_UNIT];
    if (args[ARG_TYPE]) {
        if (!parse_type((char *)args[ARG_TYPE], &type)) {
            fprintf(stderr, "SanaDump: bad TYPE %s (want bare hex 0000-ffff, e.g. 0800)\n",
                    (char *)args[ARG_TYPE]);
            FreeArgs(rda);
            return 20;
        }
        typed = TRUE;
    }
    if (args[ARG_PROM])
        flags |= SANA2OPF_PROM;
    if (args[ARG_COUNT])
        want_count = (ULONG)*(LONG *)args[ARG_COUNT];

    if (!sana2_open(&so, "loopback.device", (LONG)unit, flags)) {
        fprintf(stderr, "SanaDump: can't open loopback.device unit %lu\n", (unsigned long)unit);
        FreeArgs(rda);
        return 20;
    }

    /* A capture tool that isn't online captures nothing -- see this
     * file's own header comment. */
    so.io->ios2_Req.io_Command = S2_ONLINE;
    DoIO((struct IORequest *)so.io);

    /* Snaplen advertises the largest record this file may contain -- a
     * full MAX_FRAME payload plus the 14-byte header this tool prepends,
     * matching frame[]'s own size above. */
    if (!pcapio_open_write(&writer, (char *)args[ARG_FILE], ETH_HDR_SIZE + MAX_FRAME, PCAP_LINKTYPE_ETHERNET)) {
        fprintf(stderr, "SanaDump: can't create %s\n", (char *)args[ARG_FILE]);
        sana2_close(&so);
        FreeArgs(rda);
        return 20;
    }

    printf("SanaDump: capturing unit %lu to %s (Ctrl-C to stop)\n",
           (unsigned long)unit, (char *)args[ARG_FILE]);

    for (;;) {
        ULONG sigs;

        if (want_count && captured >= want_count)
            break;

        /* ios2_Data holds only the packet's PAYLOAD -- SANA-II keeps
         * addresses/type in their own separate ios2_SrcAddr/ios2_DstAddr/
         * ios2_PacketType fields, never inline with the data (see
         * userdocs/Configuration-Reference.md and loopback_device.c's own
         * do_write()/fill_read_from_packet()). A .pcap record, though,
         * needs a real 14-byte Ethernet header (dst+src+type) ahead of
         * the payload -- that's what every reader of this file (this
         * device's own load_replay_file(), tcpdump, Wireshark, SanaSend
         * itself) expects a "packet" to be. `frame + ETH_HDR_SIZE` is
         * where the hook writes the payload, leaving room to fill in
         * the header fields afterward from the read's own address/type
         * fields, once they're known. */
        so.io->ios2_Req.io_Command = typed ? CMD_READ : S2_READORPHAN;
        so.io->ios2_PacketType     = type;
        so.io->ios2_Data           = frame + ETH_HDR_SIZE;
        so.io->ios2_Req.io_Flags   = 0;
        SendIO((struct IORequest *)so.io);

        sigs = Wait((1UL << so.port->mp_SigBit) | SIGBREAKF_CTRL_C);
        if (sigs & SIGBREAKF_CTRL_C) {
            AbortIO((struct IORequest *)so.io);
            WaitIO((struct IORequest *)so.io);
            printf("SanaDump: interrupted\n");
            break;
        }
        WaitIO((struct IORequest *)so.io);

        if (so.io->ios2_Req.io_Error != 0)
            continue; /* aborted or failed read -- not a capturable packet */

        {
            int k;
            for (k = 0; k < 6; k++) {
                frame[k]     = so.io->ios2_DstAddr[k];
                frame[6 + k] = so.io->ios2_SrcAddr[k];
            }
            frame[12] = (UBYTE)(so.io->ios2_PacketType >> 8);
            frame[13] = (UBYTE)so.io->ios2_PacketType;
        }

        /* Real wall-clock time, unlike this device's own tick-relative
         * record= timestamps (see userdocs/Replay-and-Record.md) -- a
         * capture of REAL traffic (from a real stack talking through
         * this device, not the device's own deterministic replay) should
         * carry a real timestamp, matching what any other pcap-producing
         * tool would write. */
        if (!pcapio_write_packet(&writer, (ULONG)time(NULL), 0, frame,
                                 ETH_HDR_SIZE + so.io->ios2_DataLength)) {
            fprintf(stderr, "SanaDump: write to %s failed\n", (char *)args[ARG_FILE]);
            rc = 10;
            break;
        }
        captured++;
    }

    /* Close() is where a filesystem's deferred flush errors finally
     * surface (see pcapio_close_write()'s own comment) -- a capture tool
     * that reports success over a file the filesystem never finished
     * writing is worse than useless. */
    if (!pcapio_close_write(&writer)) {
        fprintf(stderr, "SanaDump: closing %s failed -- capture may be incomplete\n",
                (char *)args[ARG_FILE]);
        rc = 10;
    }
    sana2_close(&so);
    FreeArgs(rda);

    printf("SanaDump: captured %lu packet%s\n", (unsigned long)captured, captured == 1 ? "" : "s");
    return rc;
}
