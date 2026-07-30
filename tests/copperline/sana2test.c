/* sana2test.c -- on-target (m68k/AmigaOS) smoke test for loopback.device.
 *
 * Opens loopback.device unit 0 twice (two independent IORequests/ports,
 * modeling two concurrent consumers of the same unit -- exactly the
 * scenario echo/crossover state-sharing exists for), then runs a growing
 * set of named sub-tests against it, each emitting a `SUB=<name>=PASS` or
 * `SUB=<name>=FAIL` serial line. run.sh asserts each SUB= line plus the
 * final aggregate RESULT=. This file grows one sub-test per milestone
 * (see docs/PLAN.md); the SUB= protocol exists so each milestone's
 * regression is individually visible in the serial capture rather than
 * buried in one pass/fail bit.
 *
 * Emits over serial via exec/RawPutChar -- the ROM debug path, which needs
 * no serial.device handler, no Mount, and no Workbench files, so it works
 * in the most minimal boot. Copperline's `--serial stdout` captures it.
 *
 * RawPutChar technique lifted from the sibling amiauth project's
 * tests/copperline/serialtest.c. Unlike that program, this one is
 * freestanding (-nostdlib -nostartfiles, no crt at all) -- empirically,
 * under Copperline with a REAL Kickstart 1.3 ROM (not AROS), a normal
 * linked `-noixemul` program either failed to launch ("utility.library
 * failed to load" -- its crt forces a V37 auto-open regardless of
 * __oslibversion) or crashed outright (Guru "task held") even doing
 * nothing but calling RawPutChar. This harness exists specifically to
 * validate loopback.device's own 1.3 claims, so it can't be tripped up by
 * its OWN crt's V37 assumptions -- hence the same no-crt, hand-rolled-entry
 * approach already used by src/loopback_device.c, including manual
 * MsgPort/IORequest construction instead of amiga.lib's CreatePort()/
 * CreateExtIO() (untried whether those are crt-independent here; not worth
 * finding out when doing it by hand is a dozen lines).
 */

#include <exec/types.h>
#include <exec/nodes.h>
#include <exec/ports.h>
#include <exec/io.h>
#include <exec/errors.h>
#include <exec/memory.h>
#include <exec/execbase.h>
#include <devices/sana2.h>
#include <utility/tagitem.h>
#include <proto/exec.h>

struct ExecBase *SysBase;

/* Invocation counters for the bufferhooks sub-test's registered hooks.
 * Plain globals: safe here for the same reason as SysBase above (this is
 * -nostdlib, no -fbaserel small-data model to violate). */
static ULONG g_HookToCount;
static ULONG g_HookFromCount;

/* Every message as a NAMED array, never a raw string literal passed
 * directly to raw_str(). Root cause of an on-target "task held" Guru that
 * reproduced on every run regardless of Kickstart version or floppy image
 * (found by disassembling the built binary -- far faster than attaching
 * gdb to a live Copperline session, which was too slow under breakpoint/
 * single-step overhead to be usable here): this freestanding build's
 * backend pools EVERY string literal used anywhere in the translation unit
 * into a single block at the very front of .text, before any function
 * code, regardless of which function references them or their source
 * order. AmigaDOS always jumps to hunk offset 0, so that pooled string
 * data -- not _start -- ended up there, and execution began by literally
 * running string bytes as instructions. A *named* array like
 * `static char MSG_X[] = "...";` is ordinary initialized data and
 * lands in .data (a separate hunk), sidestepping the .text pool entirely.
 * This matches why loopback_device.c never hit this: it only ever uses
 * named char-array globals (DEVICE_NAME et al.), never an inline literal
 * passed straight to a function. */
static char MSG_BEGIN[]        = "BEGIN\r\n";
static char MSG_FAIL_PORT[]    = "FAIL_PORT\r\n";
static char MSG_FAIL_SIGNAL[]  = "FAIL_SIGNAL\r\n";
static char MSG_FAIL_IOREQ[]   = "FAIL_IOREQ\r\n";
static char MSG_OPEN_FAIL[]    = "OPEN=FAIL\r\n";
static char MSG_OPEN_OK[]      = "OPEN=OK\r\n";
static char MSG_MTU[]          = "MTU=";
static char MSG_HWTYPE[]       = "HWTYPE=";
static char MSG_ADDR[]         = "ADDR=";
static char MSG_CRLF[]         = "\r\n";
static char MSG_RESULT_PASS[]  = "RESULT=PASS\r\n";
static char MSG_RESULT_FAIL[]  = "RESULT=FAIL\r\n";
static char MSG_END[]          = "END\r\n";
static char MSG_SUB[]          = "SUB=";
static char MSG_EQ_PASS[]      = "=PASS\r\n";
static char MSG_EQ_FAIL[]      = "=FAIL\r\n";
static char HEX_DIGITS[]       = "0123456789abcdef";
static char PAYLOAD[]          = "sana2loop-echo-test";
static char DEVICE_NAME[]      = "loopback.device";

static char NAME_ONLINE[]         = "online";
static char NAME_DEVICEQUERY[]    = "devicequery";
static char NAME_STATIONADDR[]    = "stationaddress";
static char NAME_ECHO[]           = "echo";
static char NAME_OFFLINE[]        = "offline";
static char NAME_ONEVENT[]        = "onevent";
static char NAME_ABORTIO[]        = "abortio";
static char NAME_CMDFLUSH[]       = "cmdflush";
static char NAME_BUFFERHOOKS[]    = "bufferhooks";
static char NAME_ONEVENTNOW[]     = "oneventnow";
static char NAME_DELIVERY[]       = "delivery";
static char NAME_CONFIGINTERFACE[] = "configinterface";
static char NAME_TRACKTYPE[]      = "tracktype";
static char DBG_CHECKIO_NULL[]    = "DBG_CHECKIO_NULL=";

/* _start must be the very first thing in the file and contain no data of
 * its own -- it only calls test_main(), which holds all the real logic and
 * can live anywhere since it's reached by an ordinary call, never by
 * implicit position (see loopback_device.c's own _start for the
 * device-side equivalent of the "must be first" convention; an earlier
 * version of this file also tried defining the raw_* helpers before
 * _start, which hit the same class of bug from the other direction --
 * __attribute__((no_reorder)) only stops the compiler moving _start under
 * optimization, it does not override plain source order). */
static int test_main(void);
static void raw_put(char c);
static void raw_str(const char *s);
static void raw_u16(unsigned short v, int width);
static void raw_hex_byte(unsigned char b);
static void report(const char *name, BOOL ok, int *fails);
static struct MsgPort *make_port(void);
static struct IOSana2Req *make_io(struct MsgPort *port);
static BOOL counting_copy_to(APTR to asm("a0"), APTR from asm("a1"), ULONG size asm("d0"));
static BOOL counting_copy_from(APTR to asm("a0"), APTR from asm("a1"), ULONG size asm("d0"));
static BOOL open_unit0(struct IOSana2Req *io);
static void send_io(struct IOSana2Req *io);
static void do_io(struct IOSana2Req *io);

int __attribute__((no_reorder)) _start(void)
{
    return test_main();
}

static int test_main(void)
{
    struct MsgPort *port1, *port2;
    struct IOSana2Req *io1, *io2;
    static UBYTE readbuf[64];
    static UBYTE readbuf2[64];
    int fails = 0;

    SysBase = *(struct ExecBase **)4UL;

    raw_str(MSG_BEGIN);

    port1 = make_port();
    port2 = make_port();
    if (!port1 || !port2) {
        raw_str(MSG_END);
        return 1;
    }
    io1 = make_io(port1);
    io2 = make_io(port2);
    if (!io1 || !io2) {
        raw_str(MSG_END);
        return 1;
    }
    if (!open_unit0(io1) || !open_unit0(io2)) {
        raw_str(MSG_END);
        return 1;
    }
    raw_str(MSG_OPEN_OK);

    /* --- online --- */
    io1->ios2_Req.io_Command = S2_ONLINE;
    do_io(io1);
    report(NAME_ONLINE, io1->ios2_Req.io_Error == 0, &fails);

    /* --- devicequery --- */
    {
        struct Sana2DeviceQuery dq;
        BOOL ok;
        dq.SizeAvailable = sizeof(dq);
        io1->ios2_Req.io_Command = S2_DEVICEQUERY;
        io1->ios2_StatData = &dq;
        do_io(io1);
        ok = (io1->ios2_Req.io_Error == 0) && dq.MTU == 1500 &&
             dq.HardwareType == S2WireType_Ethernet;
        if (io1->ios2_Req.io_Error == 0) {
            raw_str(MSG_MTU);
            raw_u16(dq.MTU, 1);
            raw_str(MSG_CRLF);
            raw_str(MSG_HWTYPE);
            raw_u16(dq.HardwareType, 1);
            raw_str(MSG_CRLF);
        }
        report(NAME_DEVICEQUERY, ok, &fails);
    }

    /* --- stationaddress --- */
    {
        BOOL ok;
        io1->ios2_Req.io_Command = S2_GETSTATIONADDRESS;
        do_io(io1);
        ok = (io1->ios2_Req.io_Error == 0);
        if (ok) {
            int i;
            raw_str(MSG_ADDR);
            for (i = 0; i < 6; i++)
                raw_hex_byte(io1->ios2_SrcAddr[i]);
            raw_str(MSG_CRLF);
        }
        report(NAME_STATIONADDR, ok, &fails);
    }

    /* --- echo: pre-queue the read (SendIO) before the write, so this
     * already matches the consumer pattern strict SANA-II receive
     * semantics require (a read must be queued before a packet arrives, or
     * it is dropped, not buffered) -- see docs/PLAN.md M2. Uses io1 for the
     * read and io2 for the write: DoIO on io1 while it's SendIO-pending
     * would deadlock (same request, same task), so a second IORequest/port
     * is the correct tool, not a workaround. */
    {
        int i;
        BOOL addr_ok = TRUE, data_ok = TRUE;

        io1->ios2_Req.io_Command = CMD_READ;
        io1->ios2_Data = readbuf;
        io1->ios2_PacketType = 0x1234; /* CMD_READ is typed: must match
                                           what the write below sends, or
                                           M2's strict delivery drops it */
        io1->ios2_Req.io_Flags = 0;
        send_io(io1);

        io2->ios2_Req.io_Command = CMD_WRITE;
        for (i = 0; i < SANA2_MAX_ADDR_BYTES; i++)
            io2->ios2_DstAddr[i] = (i < 6) ? (UBYTE)(0xAA + i) : 0;
        io2->ios2_PacketType = 0x1234;
        io2->ios2_DataLength = sizeof(PAYLOAD) - 1;
        io2->ios2_Data = (APTR)PAYLOAD;
        do_io(io2);

        WaitIO((struct IORequest *)io1);

        if (io1->ios2_Req.io_Error || io2->ios2_Req.io_Error) {
            addr_ok = data_ok = FALSE;
        } else {
            for (i = 0; i < 6; i++)
                if (io1->ios2_SrcAddr[i] != (UBYTE)(0xAA + i))
                    addr_ok = FALSE;
            if (io1->ios2_DataLength != sizeof(PAYLOAD) - 1)
                data_ok = FALSE;
            else
                for (i = 0; i < (int)io1->ios2_DataLength; i++)
                    if (readbuf[i] != (UBYTE)PAYLOAD[i])
                        data_ok = FALSE;
        }
        report(NAME_ECHO, addr_ok && data_ok, &fails);
    }

    /* --- offline: a write while offline must fail S2ERR_OUTOFSERVICE --- */
    {
        BOOL ok;
        io1->ios2_Req.io_Command = S2_OFFLINE;
        do_io(io1);

        io1->ios2_Req.io_Command = CMD_WRITE;
        io1->ios2_DataLength = sizeof(PAYLOAD) - 1;
        io1->ios2_Data = (APTR)PAYLOAD;
        do_io(io1);
        ok = (io1->ios2_Req.io_Error == S2ERR_OUTOFSERVICE);

        io1->ios2_Req.io_Command = S2_ONLINE; /* restore for later sub-tests */
        do_io(io1);

        report(NAME_OFFLINE, ok, &fails);
    }

    /* --- onevent: edge-triggered -- a queued S2_ONEVENT for S2EVENT_OFFLINE
     * must complete only once S2_OFFLINE actually fires, with
     * ios2_WireError reporting the mask.
     *
     * CheckIO() right after SendIO() is logged (DBG_CHECKIO_NULL) as a
     * "did this NOT complete synchronously" diagnostic, but deliberately
     * not gated on: confirmed on real Kickstart 1.3 it correctly reports
     * not-yet-complete (1) here, but under Copperline's bundled AROS it
     * reports complete (0) at this same point even though the device
     * hasn't replied yet -- an AROS CheckIO compatibility quirk, not a
     * device or harness bug (the actual delivered values below are correct
     * on both). Consistent with this project treating the AROS CI lane as
     * a gross-regression check, not a stand-in for real 1.3 -- see
     * CLAUDE.md. */
    {
        BOOL ok;
        io2->ios2_Req.io_Command = S2_ONEVENT;
        io2->ios2_WireError = S2EVENT_OFFLINE;
        io2->ios2_Req.io_Flags = 0;
        send_io(io2);

        raw_str(DBG_CHECKIO_NULL);
        raw_u16((unsigned short)(CheckIO((struct IORequest *)io2) == NULL), 1);
        raw_str(MSG_CRLF);

        io1->ios2_Req.io_Command = S2_OFFLINE;
        do_io(io1);

        WaitIO((struct IORequest *)io2);
        ok = (io2->ios2_Req.io_Error == 0) &&
             (io2->ios2_WireError == S2EVENT_OFFLINE);

        io1->ios2_Req.io_Command = S2_ONLINE; /* restore */
        do_io(io1);

        report(NAME_ONEVENT, ok, &fails);
    }

    /* --- oneventnow: S2_ONEVENT for a condition ALREADY true at
     * submission time (unit is online now; request asks for
     * S2EVENT_ONLINE) must complete immediately, not wait for some future
     * transition. An earlier version of this device queued unconditionally
     * and called that edge-triggered by design -- wrong, and it would have
     * hung a caller doing the single most common thing with S2_ONEVENT
     * ("tell me once the link is up"). Caught by cross-checking a real
     * shipped SANA-II driver (plipbox) rather than by assumption; see
     * fire_event()'s comment in the device source. */
    {
        BOOL ok;
        io2->ios2_Req.io_Command = S2_ONEVENT;
        io2->ios2_WireError = S2EVENT_ONLINE;
        do_io(io2);
        ok = (io2->ios2_Req.io_Error == 0) && (io2->ios2_WireError == S2EVENT_ONLINE);
        report(NAME_ONEVENTNOW, ok, &fails);
    }

    /* --- abortio: a queued CMD_READ with nothing to satisfy it must abort
     * cleanly with IOERR_ABORTED, not hang. --- */
    {
        BOOL ok;
        io2->ios2_Req.io_Command = CMD_READ;
        io2->ios2_Data = readbuf;
        io2->ios2_Req.io_Flags = 0;
        send_io(io2);

        AbortIO((struct IORequest *)io2);
        WaitIO((struct IORequest *)io2);
        ok = (io2->ios2_Req.io_Error == IOERR_ABORTED);

        report(NAME_ABORTIO, ok, &fails);
    }

    /* --- cmdflush: CMD_FLUSH (issued on io1) must abort a read queued by a
     * DIFFERENT opener (io2) on the same unit -- flush is a unit-wide
     * operation, not per-opener. --- */
    {
        BOOL ok;
        io2->ios2_Req.io_Command = CMD_READ;
        io2->ios2_Data = readbuf;
        io2->ios2_Req.io_Flags = 0;
        send_io(io2);

        io1->ios2_Req.io_Command = CMD_FLUSH;
        do_io(io1);

        WaitIO((struct IORequest *)io2);
        ok = (io1->ios2_Req.io_Error == 0) &&
             (io2->ios2_Req.io_Error == IOERR_ABORTED);

        report(NAME_CMDFLUSH, ok, &fails);
    }

    /* --- bufferhooks: a real S2_CopyToBuff/S2_CopyFromBuff tag pair
     * (chained via TAG_MORE, to exercise that control tag specifically)
     * must actually be invoked, with the data still arriving correctly
     * through them -- proving the hooks are wired into the real data
     * path, not just recorded. A separate third open (io3/port3), since
     * io1/io2 already have default hooks installed from OpenDevice and
     * hooks are fixed for the life of an open.
     *
     * Two independent exchanges with io1/io2 (which have default hooks),
     * rather than one round trip on io3 alone: under M2's strict rxqueue=0
     * drop semantics a write with no read already queued is dropped, not
     * buffered, so each direction needs its own pre-queued reader --
     * io3 as reader (exercises CopyToBuff) and io3 as writer (exercises
     * CopyFromBuff) are two separate, independently-queued exchanges. */
    {
        struct MsgPort *port3 = make_port();
        struct IOSana2Req *io3 = port3 ? make_io(port3) : NULL;
        BOOL ok = FALSE;

        if (io3) {
            struct TagItem tags2[2];
            struct TagItem tags1[2];

            tags2[0].ti_Tag  = S2_CopyFromBuff;
            tags2[0].ti_Data = (ULONG)counting_copy_from;
            tags2[1].ti_Tag  = TAG_DONE;
            tags2[1].ti_Data = 0;
            tags1[0].ti_Tag  = S2_CopyToBuff;
            tags1[0].ti_Data = (ULONG)counting_copy_to;
            tags1[1].ti_Tag  = TAG_MORE;
            tags1[1].ti_Data = (ULONG)tags2;

            io3->ios2_BufferManagement = tags1;
            if (open_unit0(io3)) {
                int i;
                BOOL to_ok = FALSE, from_ok = FALSE;
                g_HookToCount = 0;
                g_HookFromCount = 0;

                /* Exchange A: io3 reads (CopyToBuff), io2 writes (default). */
                io3->ios2_Req.io_Command = CMD_READ;
                io3->ios2_Data = readbuf;
                io3->ios2_PacketType = 0x1234;
                io3->ios2_Req.io_Flags = 0;
                send_io(io3);

                io2->ios2_Req.io_Command = CMD_WRITE;
                for (i = 0; i < SANA2_MAX_ADDR_BYTES; i++)
                    io2->ios2_DstAddr[i] = (i < 6) ? (UBYTE)(0xBB + i) : 0;
                io2->ios2_PacketType = 0x1234;
                io2->ios2_DataLength = sizeof(PAYLOAD) - 1;
                io2->ios2_Data = (APTR)PAYLOAD;
                do_io(io2);

                WaitIO((struct IORequest *)io3);
                to_ok = (io3->ios2_Req.io_Error == 0) && (io2->ios2_Req.io_Error == 0) &&
                        g_HookToCount >= 1 &&
                        io3->ios2_DataLength == sizeof(PAYLOAD) - 1;
                if (to_ok)
                    for (i = 0; i < (int)io3->ios2_DataLength; i++)
                        if (readbuf[i] != (UBYTE)PAYLOAD[i])
                            to_ok = FALSE;

                /* Exchange B: io1 reads (default), io3 writes (CopyFromBuff). */
                io1->ios2_Req.io_Command = CMD_READ;
                io1->ios2_Data = readbuf;
                io1->ios2_PacketType = 0x5678;
                io1->ios2_Req.io_Flags = 0;
                send_io(io1);

                io3->ios2_Req.io_Command = CMD_WRITE;
                for (i = 0; i < SANA2_MAX_ADDR_BYTES; i++)
                    io3->ios2_DstAddr[i] = (i < 6) ? (UBYTE)(0xCC + i) : 0;
                io3->ios2_PacketType = 0x5678;
                io3->ios2_DataLength = sizeof(PAYLOAD) - 1;
                io3->ios2_Data = (APTR)PAYLOAD;
                do_io(io3);

                WaitIO((struct IORequest *)io1);
                from_ok = (io1->ios2_Req.io_Error == 0) && (io3->ios2_Req.io_Error == 0) &&
                          g_HookFromCount >= 1 &&
                          io1->ios2_DataLength == sizeof(PAYLOAD) - 1;
                if (from_ok)
                    for (i = 0; i < (int)io1->ios2_DataLength; i++)
                        if (readbuf[i] != (UBYTE)PAYLOAD[i])
                            from_ok = FALSE;

                ok = to_ok && from_ok;
                CloseDevice((struct IORequest *)io3);
            }
            FreeMem(io3, sizeof(struct IOSana2Req));
        }
        if (port3) {
            FreeSignal(port3->mp_SigBit);
            FreeMem(port3, sizeof(struct MsgPort));
        }

        report(NAME_BUFFERHOOKS, ok, &fails);
    }

    /* --- delivery: SANA-II precedence and strict-drop semantics.
     * (a) With rxqueue=0 (the default), a packet with no read of any kind
     *     queued for it is dropped, not buffered -- and UnknownTypesReceived
     *     counts it, per the proposal's "drivers hold no internal buffers"
     *     teaching claim.
     * (b) A pending typed CMD_READ takes priority over a pending
     *     S2_READORPHAN for the very same packet -- the orphan read must
     *     stay untouched (verified by aborting it afterward: if it had
     *     wrongly consumed the packet, AbortIO would be a no-op on an
     *     already-completed request instead of actually aborting it). */
    {
        BOOL ok = TRUE;
        struct Sana2DeviceStats before, after;

        io1->ios2_Req.io_Command = S2_GETGLOBALSTATS;
        io1->ios2_StatData = &before;
        do_io(io1);

        io2->ios2_Req.io_Command = CMD_WRITE;
        {
            int i;
            for (i = 0; i < SANA2_MAX_ADDR_BYTES; i++)
                io2->ios2_DstAddr[i] = (i < 6) ? (UBYTE)(0xDD + i) : 0;
        }
        io2->ios2_PacketType = 0x7777;
        io2->ios2_DataLength = sizeof(PAYLOAD) - 1;
        io2->ios2_Data = (APTR)PAYLOAD;
        do_io(io2);
        ok = ok && (io1->ios2_Req.io_Error == 0) && (io2->ios2_Req.io_Error == 0);

        io1->ios2_Req.io_Command = S2_GETGLOBALSTATS;
        io1->ios2_StatData = &after;
        do_io(io1);
        ok = ok && (io1->ios2_Req.io_Error == 0) &&
             (after.UnknownTypesReceived == before.UnknownTypesReceived + 1);

        {
            struct MsgPort *port3 = make_port();
            struct IOSana2Req *io3 = port3 ? make_io(port3) : NULL;
            BOOL precedence_ok = FALSE;

            if (io3 && open_unit0(io3)) {
                int i;

                io1->ios2_Req.io_Command = S2_READORPHAN;
                io1->ios2_Data = readbuf;
                io1->ios2_Req.io_Flags = 0;
                send_io(io1);

                io2->ios2_Req.io_Command = CMD_READ;
                io2->ios2_Data = readbuf2;
                io2->ios2_PacketType = 0x8888;
                io2->ios2_Req.io_Flags = 0;
                send_io(io2);

                io3->ios2_Req.io_Command = CMD_WRITE;
                for (i = 0; i < SANA2_MAX_ADDR_BYTES; i++)
                    io3->ios2_DstAddr[i] = (i < 6) ? (UBYTE)(0xEE + i) : 0;
                io3->ios2_PacketType = 0x8888;
                io3->ios2_DataLength = sizeof(PAYLOAD) - 1;
                io3->ios2_Data = (APTR)PAYLOAD;
                do_io(io3);

                WaitIO((struct IORequest *)io2);
                precedence_ok = (io2->ios2_Req.io_Error == 0) && (io3->ios2_Req.io_Error == 0);

                /* io1 (orphan) must still be pending -- prove it by
                 * aborting it: a no-op AbortIO would mean it had already
                 * (wrongly) completed. */
                AbortIO((struct IORequest *)io1);
                WaitIO((struct IORequest *)io1);
                precedence_ok = precedence_ok && (io1->ios2_Req.io_Error == IOERR_ABORTED);

                CloseDevice((struct IORequest *)io3);
            }
            if (io3)
                FreeMem(io3, sizeof(struct IOSana2Req));
            if (port3) {
                FreeSignal(port3->mp_SigBit);
                FreeMem(port3, sizeof(struct MsgPort));
            }

            ok = ok && precedence_ok;
        }

        report(NAME_DELIVERY, ok, &fails);
    }

    /* --- configinterface: first S2_CONFIGINTERFACE adopts a new CURRENT
     * station address (S2_GETSTATIONADDRESS's ios2_SrcAddr) while the
     * FACTORY address (ios2_DstAddr) stays put; a second attempt on the
     * same (now-configured) unit must fail
     * S2ERR_BAD_STATE/S2WERR_IS_CONFIGURED. --- */
    {
        BOOL ok = TRUE;
        UBYTE factory_before[6];
        int i;

        io1->ios2_Req.io_Command = S2_GETSTATIONADDRESS;
        do_io(io1);
        for (i = 0; i < 6; i++)
            factory_before[i] = io1->ios2_DstAddr[i];

        io1->ios2_Req.io_Command = S2_CONFIGINTERFACE;
        for (i = 0; i < SANA2_MAX_ADDR_BYTES; i++)
            io1->ios2_SrcAddr[i] = (i < 6) ? (UBYTE)(0x10 + i) : 0;
        do_io(io1);
        ok = ok && (io1->ios2_Req.io_Error == 0);

        io1->ios2_Req.io_Command = S2_GETSTATIONADDRESS;
        do_io(io1);
        ok = ok && (io1->ios2_Req.io_Error == 0);
        for (i = 0; i < 6; i++) {
            if (io1->ios2_SrcAddr[i] != (UBYTE)(0x10 + i))
                ok = FALSE;
            if (io1->ios2_DstAddr[i] != factory_before[i])
                ok = FALSE;
        }

        io1->ios2_Req.io_Command = S2_CONFIGINTERFACE; /* second attempt -- must fail */
        do_io(io1);
        ok = ok && (io1->ios2_Req.io_Error == S2ERR_BAD_STATE) &&
             (io1->ios2_WireError == S2WERR_IS_CONFIGURED);

        report(NAME_CONFIGINTERFACE, ok, &fails);
    }

    /* --- tracktype: S2_TRACKTYPE/S2_GETTYPESTATS/S2_UNTRACKTYPE against a
     * packet type untouched by any earlier sub-test -- proves the fixed
     * 8-slot table actually accumulates real traffic counts (not just
     * recorded and ignored), and that the duplicate/not-tracked error
     * paths work. --- */
    {
        BOOL ok = TRUE;
        struct Sana2PacketTypeStats stats;
        int i;

        io1->ios2_Req.io_Command = S2_TRACKTYPE;
        io1->ios2_PacketType = 0x9999;
        do_io(io1);
        ok = ok && (io1->ios2_Req.io_Error == 0);

        io1->ios2_Req.io_Command = S2_TRACKTYPE; /* duplicate -- must fail */
        do_io(io1);
        ok = ok && (io1->ios2_Req.io_Error == S2ERR_BAD_STATE) &&
             (io1->ios2_WireError == S2WERR_ALREADY_TRACKED);

        /* One echo exchange of the tracked type: io1 reads, io2 writes. */
        io1->ios2_Req.io_Command = CMD_READ;
        io1->ios2_Data = readbuf;
        io1->ios2_PacketType = 0x9999;
        io1->ios2_Req.io_Flags = 0;
        send_io(io1);

        io2->ios2_Req.io_Command = CMD_WRITE;
        for (i = 0; i < SANA2_MAX_ADDR_BYTES; i++)
            io2->ios2_DstAddr[i] = (i < 6) ? (UBYTE)(0xFF - i) : 0;
        io2->ios2_PacketType = 0x9999;
        io2->ios2_DataLength = sizeof(PAYLOAD) - 1;
        io2->ios2_Data = (APTR)PAYLOAD;
        do_io(io2);

        WaitIO((struct IORequest *)io1);
        ok = ok && (io1->ios2_Req.io_Error == 0) && (io2->ios2_Req.io_Error == 0);

        io1->ios2_Req.io_Command = S2_GETTYPESTATS;
        io1->ios2_PacketType = 0x9999;
        io1->ios2_StatData = &stats;
        do_io(io1);
        ok = ok && (io1->ios2_Req.io_Error == 0) &&
             stats.PacketsSent == 1 && stats.PacketsReceived == 1 &&
             stats.BytesSent == sizeof(PAYLOAD) - 1 &&
             stats.BytesReceived == sizeof(PAYLOAD) - 1 &&
             stats.PacketsDropped == 0;

        io1->ios2_Req.io_Command = S2_UNTRACKTYPE;
        io1->ios2_PacketType = 0x9999;
        do_io(io1);
        ok = ok && (io1->ios2_Req.io_Error == 0);

        io1->ios2_Req.io_Command = S2_GETTYPESTATS; /* no longer tracked -- must fail */
        io1->ios2_StatData = &stats;
        do_io(io1);
        ok = ok && (io1->ios2_Req.io_Error == S2ERR_BAD_STATE) &&
             (io1->ios2_WireError == S2WERR_NOT_TRACKED);

        report(NAME_TRACKTYPE, ok, &fails);
    }

    CloseDevice((struct IORequest *)io1);
    CloseDevice((struct IORequest *)io2);
    FreeMem(io1, sizeof(struct IOSana2Req));
    FreeMem(io2, sizeof(struct IOSana2Req));
    FreeSignal(port1->mp_SigBit);
    FreeSignal(port2->mp_SigBit);
    FreeMem(port1, sizeof(struct MsgPort));
    FreeMem(port2, sizeof(struct MsgPort));

    raw_str(fails ? MSG_RESULT_FAIL : MSG_RESULT_PASS);
    raw_str(MSG_END);
    return fails ? 1 : 0;
}

static struct MsgPort *make_port(void)
{
    struct MsgPort *port;
    BYTE sigbit;

    port = AllocMem(sizeof(struct MsgPort), MEMF_PUBLIC | MEMF_CLEAR);
    if (!port) {
        raw_str(MSG_FAIL_PORT);
        return NULL;
    }
    sigbit = AllocSignal(-1);
    if (sigbit == -1) {
        raw_str(MSG_FAIL_SIGNAL);
        FreeMem(port, sizeof(struct MsgPort));
        return NULL;
    }
    port->mp_Node.ln_Type = NT_MSGPORT;
    port->mp_Flags        = PA_SIGNAL;
    port->mp_SigBit       = sigbit;
    port->mp_SigTask      = FindTask(NULL);
    port->mp_MsgList.lh_Head     = (struct Node *)&port->mp_MsgList.lh_Tail;
    port->mp_MsgList.lh_Tail     = NULL;
    port->mp_MsgList.lh_TailPred = (struct Node *)&port->mp_MsgList.lh_Head;
    return port;
}

static struct IOSana2Req *make_io(struct MsgPort *port)
{
    struct IOSana2Req *io;

    io = AllocMem(sizeof(struct IOSana2Req), MEMF_PUBLIC | MEMF_CLEAR);
    if (!io) {
        raw_str(MSG_FAIL_IOREQ);
        return NULL;
    }
    io->ios2_Req.io_Message.mn_Node.ln_Type = NT_MESSAGE;
    io->ios2_Req.io_Message.mn_ReplyPort    = port;
    io->ios2_Req.io_Message.mn_Length       = sizeof(struct IOSana2Req);
    return io;
}

static BOOL open_unit0(struct IOSana2Req *io)
{
    if (OpenDevice((STRPTR)DEVICE_NAME, 0, (struct IORequest *)io, 0)) {
        raw_str(MSG_OPEN_FAIL);
        return FALSE;
    }
    return TRUE;
}

/* Registered as S2_CopyToBuff/S2_CopyFromBuff for the bufferhooks
 * sub-test. Real hooks, not no-ops -- they do the actual copy (so
 * correctness proves the device's data path really runs through them, not
 * just that they get called) and count invocations. Signature matches the
 * SANA-II hook ABI exactly (a0=to, a1=from, d0=size) so the device can
 * call them directly, the same way it calls its own default_copy(). */
static BOOL counting_copy_to(APTR to asm("a0"), APTR from asm("a1"), ULONG size asm("d0"))
{
    CopyMem(from, to, size);
    g_HookToCount++;
    return TRUE;
}

static BOOL counting_copy_from(APTR to asm("a0"), APTR from asm("a1"), ULONG size asm("d0"))
{
    CopyMem(from, to, size);
    g_HookFromCount++;
    return TRUE;
}

/* Every request in this harness is reused across many commands (two
 * IORequests carry the whole sub-test suite, on purpose -- see the header
 * comment). ReplyMsg() marks a completed message's node NT_REPLYMSG; that
 * mark does not reset itself, so resubmitting a just-completed IORequest
 * without restoring NT_MESSAGE first makes CheckIO() report it as already
 * complete before the device has even seen the new command. Real code
 * normally uses a fresh IORequest per command (or amiga.lib's
 * CreateExtIO(), which sets this once) specifically to avoid ever needing
 * to think about this; reusing two by hand means these wrappers must. */
static void send_io(struct IOSana2Req *io)
{
    io->ios2_Req.io_Message.mn_Node.ln_Type = NT_MESSAGE;
    SendIO((struct IORequest *)io);
}

static void do_io(struct IOSana2Req *io)
{
    io->ios2_Req.io_Message.mn_Node.ln_Type = NT_MESSAGE;
    DoIO((struct IORequest *)io);
}

static void report(const char *name, BOOL ok, int *fails)
{
    raw_str(MSG_SUB);
    raw_str(name);
    raw_str(ok ? MSG_EQ_PASS : MSG_EQ_FAIL);
    if (!ok)
        (*fails)++;
}

/* exec RawPutChar: char in d0, SysBase in a6, LVO -516. */
static void raw_put(char c)
{
    register long d0 __asm__("d0") = (unsigned char)c;
    register void *a6 __asm__("a6") = SysBase;
    __asm__ volatile("jsr -516(%%a6)" : : "r"(d0), "r"(a6)
                     : "d1", "a0", "a1", "cc", "memory");
}

static void raw_str(const char *s)
{
    while (*s)
        raw_put(*s++);
}

/* 16-bit, not 32: -nostdlib has no libgcc, and 32-bit / and % on this
 * -m68000 target compile to __udivsi3/__umodsi3 calls that don't exist
 * without it. Every value this harness prints (io_Error, MTU, HardwareType)
 * fits comfortably in 16 bits, where the 68000's native divu.w suffices. */
static void raw_u16(unsigned short v, int width)
{
    char buf[6];
    int n = 0;
    if (v == 0)
        buf[n++] = '0';
    while (v) {
        buf[n++] = (char)('0' + (v % 10));
        v = (unsigned short)(v / 10);
    }
    while (n < width)
        buf[n++] = '0';
    while (n)
        raw_put(buf[--n]);
}

static void raw_hex_byte(unsigned char b)
{
    raw_put(HEX_DIGITS[b >> 4]);
    raw_put(HEX_DIGITS[b & 0x0f]);
}
