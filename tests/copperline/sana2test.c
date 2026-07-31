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
/* volatile: g_HookToCount is written from real VBlank interrupt context
 * (via counting_copy_to(), called through deliver_packet() from
 * vblank_server()) and spin-polled from ordinary task context in
 * reap_interrupt_io() -- without volatile, GCC would be free to hoist
 * that loop's read out entirely, since nothing in this translation unit
 * visibly writes it from the task's own point of view. */
static volatile ULONG g_HookToCount;
static ULONG g_HookFromCount;

/* M13: separate counters for the buffhooks sub-test's 32-bit hooks (not
 * reused g_HookToCount/g_HookFromCount above, since that pair also gets
 * reused within the SAME sub-test to prove the base 8-bit hook is NOT
 * invoked while the 32-bit ones ARE) and for the dmahooks sub-test's DMA
 * hook stub (which must never be called at all -- see never_call_dma()). */
static ULONG g_Hook32ToCount;
static ULONG g_Hook32FromCount;
static ULONG g_DMAHookCalls;

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
static char NAME_BROADCAST[]      = "broadcast";
static char NAME_MULTICAST[]      = "multicast";
static char NAME_MULTICASTADDR[]  = "multicastaddress";
static char NAME_RAW[]            = "raw";
static char NAME_CONFIG[]         = "config";
static char NAME_CROSSOVER[]      = "crossover";
static char NAME_FAULTDROP[]       = "faultdrop";
static char NAME_FAULTDUP[]        = "faultdup";
static char NAME_FAULTREORDER[]    = "faultreorder";
static char NAME_FAULTOVERSILENT[] = "faultoversilent";
static char NAME_FAULTFLAP[]       = "faultflap";
static char NAME_REPLAY[]          = "replay";
static char NAME_SCHEDULED[]       = "scheduled";
static char NAME_RECORD[]          = "record";
static char NAME_RAWMTU[]          = "rawmtu";
static char NAME_PEERADDRESS[]     = "peeraddress";
static char NAME_DNSADDRESS[]      = "dnsaddress";
static char NAME_BUFFHOOKS[]       = "buffhooks";
static char NAME_DMAHOOKS[]        = "dmahooks";
static char NAME_REPLAYCAP[]       = "replaycap";
static char DBG_CHECKIO_NULL[]    = "DBG_CHECKIO_NULL=";

/* M13: this toolchain's <devices/sana2.h> lacks Rev 4's RawMTU field --
 * same gap, same fix as loopback_device.c's own struct Sana2LoopDeviceQuery
 * (see that file's comment for the full story and why the field order/
 * types here match the header's existing struct exactly). */
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

/* M13: this toolchain's <devices/sana2.h> also lacks the Rev 4/7 tag/
 * command values themselves -- same local #defines as loopback_device.c's
 * own (see that file's comment for where these numbers come from). */
#define S2_GETPEERADDRESS    0xC002
#define S2_GETDNSADDRESS     0xC003
#define S2_DMACopyToBuff64   (S2_Dummy + 10)
#define S2_DMACopyFromBuff64 (S2_Dummy + 11)

/* M13: this device's own S2_GETSPECIALSTATS record type for DMA-hook
 * negotiation (loopback_device.c's SANA2LOOP_SPECIALSTAT_DMA_HOOKS_OFFERED
 * -- not a spec value at all, SANA-II leaves "special" stat Type values
 * entirely driver-defined, so the dmahooks sub-test just needs to agree
 * with the device on this one number). */
#define SANA2LOOP_SPECIALSTAT_DMA_HOOKS_OFFERED 2

/* M8 replay fixture data -- must exactly match
 * tests/copperline/fixtures/replay.pcap's 3 hand-crafted records (see
 * run.sh's own comment for how that file gets onto the ADF). Every array
 * here is a NAMED global, never an inline literal -- see this file's own
 * header comment for why that matters even for plain data, not just
 * strings passed to a function. */
static UBYTE REPLAY_DST0[] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
static UBYTE REPLAY_SRC0[] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x02};
static char  REPLAY_PAY0[] = "REPLAY-A";
static UBYTE REPLAY_DST1[] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x03};
static UBYTE REPLAY_SRC1[] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x04};
static char  REPLAY_PAY1[] = "REPLAY-BB";
static UBYTE REPLAY_DST2[] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x05};
static UBYTE REPLAY_SRC2[] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x06};
static char  REPLAY_PAY2[] = "REPLAY-CCC";
static UBYTE *REPLAY_DST[3] = {REPLAY_DST0, REPLAY_DST1, REPLAY_DST2};
static UBYTE *REPLAY_SRC[3] = {REPLAY_SRC0, REPLAY_SRC1, REPLAY_SRC2};
static char  *REPLAY_PAY[3] = {REPLAY_PAY0, REPLAY_PAY1, REPLAY_PAY2};
static ULONG  REPLAY_TYPE[3] = {0x0800, 0x0806, 0x86DD};
static ULONG  REPLAY_LEN[3]  = {8, 9, 10};

/* M9 scheduled-replay fixture data -- must exactly match
 * tests/copperline/fixtures/scheduled.pcap's 2 records: ts_sec=100 (due
 * tick 0 -- delivers on the first VBlank after S2_ONLINE) and ts_sec=101
 * (1 real second later -- due tick 50 at SANA2LOOP_VBLANK_HZ). */
static UBYTE SCHED_DST0[] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xF1};
static UBYTE SCHED_SRC0[] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xF2};
static char  SCHED_PAY0[] = "SCHED-A";
static UBYTE SCHED_DST1[] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xF3};
static UBYTE SCHED_SRC1[] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xF4};
static char  SCHED_PAY1[] = "SCHED-BB";

/* M9 record fixture data -- the 3 known writes unit 11's record test
 * makes, so run.sh's post-run pcap extraction can byte-compare against
 * them. dst is deliberately the SAME for all 3 (an arbitrary
 * "wrote-to" address); echo mode swaps addressing on a plain CMD_WRITE
 * (see do_write()'s own comment), so the RECORDED src is this constant
 * address and the recorded dst is unit 11's own (unconfigured, factory)
 * station address -- 02:53:32:4C:00:0B, per reset_unit()'s deterministic
 * factory-address scheme (0x0B = unit 11). */
static UBYTE RECORD_WROTE_TO[] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
static char  RECORD_PAY0[] = "RECORD-ONE";
static char  RECORD_PAY1[] = "RECORD-TWO";
static char  RECORD_PAY2[] = "RECORD-THREE";
static char  *RECORD_PAY[3] = {RECORD_PAY0, RECORD_PAY1, RECORD_PAY2};
static ULONG  RECORD_TYPE[3] = {0x1000, 0x1001, 0x1002};
static ULONG  RECORD_LEN[3]  = {10, 10, 12};

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
static BOOL counting_copy_to32(APTR to asm("a0"), APTR from asm("a1"), ULONG size asm("d0"));
static BOOL counting_copy_from32(APTR to asm("a0"), APTR from asm("a1"), ULONG size asm("d0"));
static APTR never_call_dma(APTR data asm("a0"));
static BOOL open_unit0(struct IOSana2Req *io);
static void send_io(struct IOSana2Req *io);
static void do_io(struct IOSana2Req *io);
static void reap_interrupt_io(struct IOSana2Req *io, volatile ULONG *evidence, ULONG want);
static void flagged_begin_io(struct IOSana2Req *io);

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

    /* --- broadcast: S2_BROADCAST forces the destination to the all-1s
     * Ethernet broadcast address (confirmed against a real driver,
     * plipbox) and the reader sees SANA2IOF_BCAST set with that same
     * destination -- unlike a plain CMD_WRITE, whose delivered dst is
     * always this unit's own station address (see the device's
     * fill_read_from_packet() comment). --- */
    {
        BOOL ok;
        int i;

        io1->ios2_Req.io_Command = CMD_READ;
        io1->ios2_Data = readbuf;
        io1->ios2_PacketType = 0xBCBC;
        io1->ios2_Req.io_Flags = 0;
        send_io(io1);

        io2->ios2_Req.io_Command = S2_BROADCAST;
        io2->ios2_PacketType = 0xBCBC;
        io2->ios2_DataLength = sizeof(PAYLOAD) - 1;
        io2->ios2_Data = (APTR)PAYLOAD;
        for (i = 0; i < SANA2_MAX_ADDR_BYTES; i++) /* ignored -- forced to all-1s */
            io2->ios2_DstAddr[i] = (i < 6) ? (UBYTE)(0x30 + i) : 0;
        do_io(io2);

        WaitIO((struct IORequest *)io1);
        ok = (io1->ios2_Req.io_Error == 0) && (io2->ios2_Req.io_Error == 0);
        ok = ok && (io1->ios2_Req.io_Flags & SANA2IOF_BCAST) != 0;
        for (i = 0; i < 6; i++)
            if (io1->ios2_DstAddr[i] != 0xFF)
                ok = FALSE;

        report(NAME_BROADCAST, ok, &fails);
    }

    /* --- multicast: S2_MULTICAST validates the caller-chosen destination
     * actually has the Ethernet group bit set (low bit of the first
     * octet), delivers to it as given (unlike S2_BROADCAST, which
     * overrides it), and the reader sees SANA2IOF_MCAST set with that same
     * destination. A non-multicast destination must fail
     * S2ERR_BAD_ARGUMENT/S2WERR_BAD_MULTICAST before ever reaching
     * delivery. --- */
    {
        BOOL ok;
        int i;

        io2->ios2_Req.io_Command = S2_MULTICAST;
        io2->ios2_PacketType = 0xCACA;
        io2->ios2_DataLength = sizeof(PAYLOAD) - 1;
        io2->ios2_Data = (APTR)PAYLOAD;
        for (i = 0; i < SANA2_MAX_ADDR_BYTES; i++) /* 0x40 & 1 == 0 -- invalid */
            io2->ios2_DstAddr[i] = (i < 6) ? (UBYTE)(0x40 + i) : 0;
        do_io(io2);
        ok = (io2->ios2_Req.io_Error == S2ERR_BAD_ARGUMENT) &&
             (io2->ios2_WireError == S2WERR_BAD_MULTICAST);

        io1->ios2_Req.io_Command = CMD_READ;
        io1->ios2_Data = readbuf;
        io1->ios2_PacketType = 0xCACA;
        io1->ios2_Req.io_Flags = 0;
        send_io(io1);

        io2->ios2_Req.io_Command = S2_MULTICAST;
        for (i = 0; i < SANA2_MAX_ADDR_BYTES; i++) /* 0x41 & 1 == 1 -- valid */
            io2->ios2_DstAddr[i] = (i < 6) ? (UBYTE)(0x41 + i) : 0;
        do_io(io2);

        WaitIO((struct IORequest *)io1);
        ok = ok && (io1->ios2_Req.io_Error == 0) && (io2->ios2_Req.io_Error == 0);
        ok = ok && (io1->ios2_Req.io_Flags & SANA2IOF_MCAST) != 0;
        for (i = 0; i < 6; i++)
            if (io1->ios2_DstAddr[i] != (UBYTE)(0x41 + i))
                ok = FALSE;

        report(NAME_MULTICAST, ok, &fails);
    }

    /* --- multicastaddress: S2_ADD/DELMULTICASTADDRESS, refcounted --
     * adding the same group twice (as two independent opens, io1 and io2,
     * both joining) must not actually leave the group until BOTH remove
     * it; a third removal (nothing left to remove) must fail
     * S2ERR_BAD_STATE/S2WERR_BAD_MULTICAST. An invalid (non-multicast)
     * address must fail S2ERR_BAD_ARGUMENT/S2WERR_BAD_MULTICAST at add
     * time, same validation S2_MULTICAST uses. --- */
    {
        BOOL ok;
        int i;

        io1->ios2_Req.io_Command = S2_ADDMULTICASTADDRESS;
        for (i = 0; i < 6; i++) /* 0x50 & 1 == 0 -- invalid */
            io1->ios2_SrcAddr[i] = (UBYTE)(0x50 + i);
        do_io(io1);
        ok = (io1->ios2_Req.io_Error == S2ERR_BAD_ARGUMENT) &&
             (io1->ios2_WireError == S2WERR_BAD_MULTICAST);

        for (i = 0; i < 6; i++) /* 0x51 & 1 == 1 -- valid */
            io1->ios2_SrcAddr[i] = (UBYTE)(0x51 + i);
        do_io(io1);
        ok = ok && (io1->ios2_Req.io_Error == 0);

        io2->ios2_Req.io_Command = S2_ADDMULTICASTADDRESS;
        for (i = 0; i < 6; i++)
            io2->ios2_SrcAddr[i] = (UBYTE)(0x51 + i);
        do_io(io2);
        ok = ok && (io2->ios2_Req.io_Error == 0);

        io1->ios2_Req.io_Command = S2_DELMULTICASTADDRESS;
        do_io(io1); /* refcount 2 -> 1 */
        ok = ok && (io1->ios2_Req.io_Error == 0);

        io2->ios2_Req.io_Command = S2_DELMULTICASTADDRESS;
        do_io(io2); /* refcount 1 -> 0, actually gone now */
        ok = ok && (io2->ios2_Req.io_Error == 0);

        io1->ios2_Req.io_Command = S2_DELMULTICASTADDRESS;
        do_io(io1); /* nothing left -- must fail */
        ok = ok && (io1->ios2_Req.io_Error == S2ERR_BAD_STATE) &&
             (io1->ios2_WireError == S2WERR_BAD_MULTICAST);

        report(NAME_MULTICASTADDR, ok, &fails);
    }

    /* --- raw: SANA2IOF_RAW relaxes the MTU-exceeded check by 14 bytes
     * (the Ethernet header this device doesn't otherwise reparse -- see
     * the device's own comment on why that's a deliberately narrow scope
     * for a loopback device with no real wire framing to parse a header
     * out of). Confirms: (a) a write between MTU and MTU+14 fails WITHOUT
     * the flag, (b) succeeds WITH the flag and the reader gets the exact
     * same bytes back, (c) a write beyond MTU+14 fails even WITH the
     * flag.
     *
     * (b) and (c) must use flagged_begin_io(), NOT send_io()/do_io():
     * confirmed via a very confusing false failure (identical on bundled
     * AROS and real Kickstart 1.3, so this is documented AmigaOS behavior,
     * not a compatibility quirk) that BOTH SendIO()'s and DoIO()'s own
     * autodocs spell out plainly -- SendIO() "will be set to zero before
     * the request is sent," DoIO() "is always set to IOF_QUICK (0x01)
     * before the internal device call." Neither preserves a caller-set
     * io_Flags bit like SANA2IOF_RAW; only amiga.lib's BeginIO() does
     * ("equivalent to SendIO(), except that io_Flags is not cleared"),
     * hence flagged_begin_io()'s direct vector call. (a) doesn't need this
     * care since the ABSENCE of the RAW flag is exactly what SendIO()/
     * DoIO() force anyway. See docs/sana2-notes.md and
     * flagged_begin_io()'s own comment. --- */
    {
        static UBYTE rawbuf[1520];
        static UBYTE rawreadbuf[1520];
        BOOL ok;
        int i;

        for (i = 0; i < 1520; i++)
            rawbuf[i] = (UBYTE)(i & 0xFF);

        io1->ios2_Req.io_Command = CMD_WRITE;
        io1->ios2_Req.io_Flags = 0;
        io1->ios2_DataLength = 1510;
        io1->ios2_Data = rawbuf;
        io1->ios2_PacketType = 0xAAAA;
        for (i = 0; i < SANA2_MAX_ADDR_BYTES; i++)
            io1->ios2_DstAddr[i] = (i < 6) ? (UBYTE)(0x20 + i) : 0;
        do_io(io1);
        ok = (io1->ios2_Req.io_Error == S2ERR_MTU_EXCEEDED);

        io1->ios2_Req.io_Flags = SANA2IOF_RAW;
        io1->ios2_DataLength = 1520;
        flagged_begin_io(io1);
        WaitIO((struct IORequest *)io1);
        ok = ok && (io1->ios2_Req.io_Error == S2ERR_MTU_EXCEEDED);

        io2->ios2_Req.io_Command = CMD_READ;
        io2->ios2_Data = rawreadbuf;
        io2->ios2_PacketType = 0xAAAA;
        io2->ios2_Req.io_Flags = 0;
        send_io(io2);

        io1->ios2_Req.io_Command = CMD_WRITE;
        io1->ios2_Req.io_Flags = SANA2IOF_RAW;
        io1->ios2_DataLength = 1510;
        flagged_begin_io(io1);
        WaitIO((struct IORequest *)io1);

        WaitIO((struct IORequest *)io2);
        ok = ok && (io1->ios2_Req.io_Error == 0) && (io2->ios2_Req.io_Error == 0) &&
             (io2->ios2_DataLength == 1510);
        if (ok)
            for (i = 0; i < 1510; i++)
                if (rawreadbuf[i] != (UBYTE)(i & 0xFF)) {
                    ok = FALSE;
                    break;
                }

        report(NAME_RAW, ok, &fails);
    }

    /* --- config: M5's config-file infrastructure. Units 1 and 4 are
     * fresh here (no earlier sub-test ever opens them) and each get their
     * own s/sana2loop/unitN.config on the boot ADF (see run.sh) -- unit 0
     * (used by every earlier sub-test) deliberately has NO config file at
     * all, so its own devicequery sub-test already covers "no config file
     * -> compiled-in defaults" implicitly. Both fixtures live under S:,
     * not ENV: -- this minimal boot ADF has no ENV: assign at all, so
     * every lookup here naturally exercises the S: fallback path, not
     * just the ENV: primary one. Unit 4, not 2 or 3: this fixture makes
     * OpenDevice() permanently fail on whichever unit hosts it (every
     * open of a never-successfully-opened unit re-triggers the same
     * failing config read), and M6's crossover sub-test needs units 2
     * and 3 to both open cleanly as the default crossover pair.
     * (a) unit 1's config sets mtu=1400 -- S2_DEVICEQUERY must report it.
     * (b) unit 4's config has an unrecognized key -- OpenDevice() itself
     *     must fail S2ERR_BAD_ARGUMENT (fail-fast, not silently
     *     ignored). --- */
    {
        BOOL ok = TRUE;
        struct MsgPort *port3 = make_port();
        struct IOSana2Req *io3 = port3 ? make_io(port3) : NULL;

        if (io3 && OpenDevice((STRPTR)DEVICE_NAME, 1, (struct IORequest *)io3, 0) == 0) {
            struct Sana2DeviceQuery dq3;
            dq3.SizeAvailable = sizeof(dq3);
            io3->ios2_Req.io_Command = S2_DEVICEQUERY;
            io3->ios2_StatData = &dq3;
            do_io(io3);
            ok = (io3->ios2_Req.io_Error == 0) && (dq3.MTU == 1400);
            CloseDevice((struct IORequest *)io3);
        } else {
            ok = FALSE;
        }
        if (io3)
            FreeMem(io3, sizeof(struct IOSana2Req));
        if (port3) {
            FreeSignal(port3->mp_SigBit);
            FreeMem(port3, sizeof(struct MsgPort));
        }

        {
            struct MsgPort *port4 = make_port();
            struct IOSana2Req *io4 = port4 ? make_io(port4) : NULL;
            BOOL bad_ok = FALSE;

            if (io4) {
                LONG err = OpenDevice((STRPTR)DEVICE_NAME, 4, (struct IORequest *)io4, 0);
                bad_ok = (err != 0) && (io4->ios2_Req.io_Error == S2ERR_BAD_ARGUMENT);
                if (err == 0) /* shouldn't happen, but don't leak if it does */
                    CloseDevice((struct IORequest *)io4);
                FreeMem(io4, sizeof(struct IOSana2Req));
            }
            if (port4) {
                FreeSignal(port4->mp_SigBit);
                FreeMem(port4, sizeof(struct MsgPort));
            }
            ok = ok && bad_ok;
        }

        report(NAME_CONFIG, ok, &fails);
    }

    /* --- crossover: units 2 and 3 default-pair with each other (M6,
     * docs/PLAN.md) with no config file needed -- a real, separately-
     * addressed link, unlike echo mode's self-reflection. Confirms:
     * (a) a write on unit 2 to an address that is NOT unit 3's station
     *     (and not broadcast/multicast) is NOT delivered to a queued
     *     typed read on unit 3 -- crossover's destination-address
     *     filtering (dest_matches_unit()) actually gates delivery, unlike
     *     echo mode's unconditional self-delivery.
     * (b) the SAME write, re-addressed to unit 3's real station address,
     *     IS delivered, with REAL (unswapped) addressing: the unit-3
     *     reader sees ios2_SrcAddr == unit 2's own actual station address
     *     (not echo's fake swap) and ios2_DstAddr exactly what was
     *     written.
     * (c) a promiscuous (SANA2OPF_PROM) open on unit 3 accepts a
     *     wrong-destination write anyway -- the whole point of
     *     promiscuous mode. --- */
    {
        BOOL ok = TRUE;
        struct MsgPort *port2c = make_port();
        struct IOSana2Req *io2c = port2c ? make_io(port2c) : NULL; /* unit 2: writer */
        struct MsgPort *port3c = make_port();
        struct IOSana2Req *io3c = port3c ? make_io(port3c) : NULL; /* unit 3: reader */

        if (io2c && io3c &&
            OpenDevice((STRPTR)DEVICE_NAME, 2, (struct IORequest *)io2c, 0) == 0 &&
            OpenDevice((STRPTR)DEVICE_NAME, 3, (struct IORequest *)io3c, 0) == 0) {
            int i;
            UBYTE unit2_addr[SANA2_MAX_ADDR_BYTES];
            UBYTE unit3_addr[SANA2_MAX_ADDR_BYTES];

            io2c->ios2_Req.io_Command = S2_ONLINE;
            do_io(io2c);
            io3c->ios2_Req.io_Command = S2_ONLINE;
            do_io(io3c);
            ok = ok && (io2c->ios2_Req.io_Error == 0) && (io3c->ios2_Req.io_Error == 0);

            io2c->ios2_Req.io_Command = S2_GETSTATIONADDRESS;
            do_io(io2c);
            for (i = 0; i < SANA2_MAX_ADDR_BYTES; i++)
                unit2_addr[i] = io2c->ios2_SrcAddr[i];

            io3c->ios2_Req.io_Command = S2_GETSTATIONADDRESS;
            do_io(io3c);
            for (i = 0; i < SANA2_MAX_ADDR_BYTES; i++)
                unit3_addr[i] = io3c->ios2_SrcAddr[i];

            /* (a) wrong destination -- must NOT be delivered. */
            io3c->ios2_Req.io_Command = CMD_READ;
            io3c->ios2_Data = readbuf;
            io3c->ios2_PacketType = 0xC0C0;
            io3c->ios2_Req.io_Flags = 0;
            send_io(io3c);

            io2c->ios2_Req.io_Command = CMD_WRITE;
            io2c->ios2_PacketType = 0xC0C0;
            io2c->ios2_DataLength = sizeof(PAYLOAD) - 1;
            io2c->ios2_Data = (APTR)PAYLOAD;
            for (i = 0; i < SANA2_MAX_ADDR_BYTES; i++) /* deliberately NOT unit 3's address */
                io2c->ios2_DstAddr[i] = (i < 6) ? (UBYTE)(0x60 + i) : 0;
            do_io(io2c);
            ok = ok && (io2c->ios2_Req.io_Error == 0);

            AbortIO((struct IORequest *)io3c);
            WaitIO((struct IORequest *)io3c);
            ok = ok && (io3c->ios2_Req.io_Error == IOERR_ABORTED);

            /* (b) correct destination -- must be delivered, unswapped. */
            io3c->ios2_Req.io_Command = CMD_READ;
            io3c->ios2_Data = readbuf;
            io3c->ios2_PacketType = 0xC0C0;
            io3c->ios2_Req.io_Flags = 0;
            send_io(io3c);

            io2c->ios2_Req.io_Command = CMD_WRITE;
            io2c->ios2_PacketType = 0xC0C0;
            io2c->ios2_DataLength = sizeof(PAYLOAD) - 1;
            io2c->ios2_Data = (APTR)PAYLOAD;
            for (i = 0; i < SANA2_MAX_ADDR_BYTES; i++)
                io2c->ios2_DstAddr[i] = unit3_addr[i];
            do_io(io2c);

            WaitIO((struct IORequest *)io3c);
            ok = ok && (io2c->ios2_Req.io_Error == 0) && (io3c->ios2_Req.io_Error == 0);
            for (i = 0; i < 6; i++) {
                if (io3c->ios2_SrcAddr[i] != unit2_addr[i]) /* real, unswapped src */
                    ok = FALSE;
                if (io3c->ios2_DstAddr[i] != unit3_addr[i])
                    ok = FALSE;
            }

            /* (c) promiscuous open on unit 3 accepts a wrong-destination
             * write anyway. */
            {
                struct MsgPort *portp = make_port();
                struct IOSana2Req *iop = portp ? make_io(portp) : NULL;
                BOOL prom_ok = FALSE;

                if (iop && OpenDevice((STRPTR)DEVICE_NAME, 3,
                                       (struct IORequest *)iop, SANA2OPF_PROM) == 0) {
                    iop->ios2_Req.io_Command = CMD_READ;
                    iop->ios2_Data = readbuf2;
                    iop->ios2_PacketType = 0xC1C1;
                    iop->ios2_Req.io_Flags = 0;
                    send_io(iop);

                    io2c->ios2_Req.io_Command = CMD_WRITE;
                    io2c->ios2_PacketType = 0xC1C1;
                    io2c->ios2_DataLength = sizeof(PAYLOAD) - 1;
                    io2c->ios2_Data = (APTR)PAYLOAD;
                    for (i = 0; i < SANA2_MAX_ADDR_BYTES; i++) /* wrong dest again */
                        io2c->ios2_DstAddr[i] = (i < 6) ? (UBYTE)(0x70 + i) : 0;
                    do_io(io2c);

                    WaitIO((struct IORequest *)iop);
                    prom_ok = (io2c->ios2_Req.io_Error == 0) && (iop->ios2_Req.io_Error == 0);
                    CloseDevice((struct IORequest *)iop);
                }
                if (iop)
                    FreeMem(iop, sizeof(struct IOSana2Req));
                if (portp) {
                    FreeSignal(portp->mp_SigBit);
                    FreeMem(portp, sizeof(struct MsgPort));
                }
                ok = ok && prom_ok;
            }

            CloseDevice((struct IORequest *)io2c);
            CloseDevice((struct IORequest *)io3c);
        } else {
            ok = FALSE;
        }

        if (io2c)
            FreeMem(io2c, sizeof(struct IOSana2Req));
        if (io3c)
            FreeMem(io3c, sizeof(struct IOSana2Req));
        if (port2c) {
            FreeSignal(port2c->mp_SigBit);
            FreeMem(port2c, sizeof(struct MsgPort));
        }
        if (port3c) {
            FreeSignal(port3c->mp_SigBit);
            FreeMem(port3c, sizeof(struct MsgPort));
        }

        report(NAME_CROSSOVER, ok, &fails);
    }

    /* --- faultdrop: unit 5's config (droppct=100) makes every write
     * silently lost -- the write itself still "succeeds" (io_Error==0,
     * same as real best-effort Ethernet has no way to tell a sender a
     * frame got lost), but a queued read for it never completes. --- */
    {
        BOOL ok = TRUE;
        struct MsgPort *port5 = make_port();
        struct IOSana2Req *io5 = port5 ? make_io(port5) : NULL;

        if (io5 && OpenDevice((STRPTR)DEVICE_NAME, 5, (struct IORequest *)io5, 0) == 0) {
            io5->ios2_Req.io_Command = S2_ONLINE;
            do_io(io5);
            ok = (io5->ios2_Req.io_Error == 0);

            io5->ios2_Req.io_Command = CMD_READ;
            io5->ios2_Data = readbuf;
            io5->ios2_PacketType = 0xD100;
            io5->ios2_Req.io_Flags = 0;
            send_io(io5);

            /* A separate writer open -- do_io on io5 while it's
             * SendIO-pending would deadlock (same request, same task). */
            {
                struct MsgPort *port5w = make_port();
                struct IOSana2Req *io5w = port5w ? make_io(port5w) : NULL;

                if (io5w && OpenDevice((STRPTR)DEVICE_NAME, 5, (struct IORequest *)io5w, 0) == 0) {
                    int i;
                    io5w->ios2_Req.io_Command = CMD_WRITE;
                    io5w->ios2_PacketType = 0xD100;
                    io5w->ios2_DataLength = sizeof(PAYLOAD) - 1;
                    io5w->ios2_Data = (APTR)PAYLOAD;
                    for (i = 0; i < SANA2_MAX_ADDR_BYTES; i++)
                        io5w->ios2_DstAddr[i] = (i < 6) ? (UBYTE)(0x90 + i) : 0;
                    do_io(io5w);
                    ok = ok && (io5w->ios2_Req.io_Error == 0);
                    CloseDevice((struct IORequest *)io5w);
                } else {
                    ok = FALSE;
                }
                if (io5w)
                    FreeMem(io5w, sizeof(struct IOSana2Req));
                if (port5w) {
                    FreeSignal(port5w->mp_SigBit);
                    FreeMem(port5w, sizeof(struct MsgPort));
                }
            }

            AbortIO((struct IORequest *)io5); /* nothing ever arrived to satisfy it */
            WaitIO((struct IORequest *)io5);
            ok = ok && (io5->ios2_Req.io_Error == IOERR_ABORTED);

            CloseDevice((struct IORequest *)io5);
        } else {
            ok = FALSE;
        }
        if (io5)
            FreeMem(io5, sizeof(struct IOSana2Req));
        if (port5) {
            FreeSignal(port5->mp_SigBit);
            FreeMem(port5, sizeof(struct MsgPort));
        }

        report(NAME_FAULTDROP, ok, &fails);
    }

    /* --- faultdup: unit 6's config (duppct=100) makes every write
     * deliver TWICE -- two independently queued typed reads for the same
     * packet type must BOTH complete from just one write, proving real
     * duplication (a second, separately delivered packet), not the same
     * buffer referenced twice. --- */
    {
        BOOL ok = TRUE;
        struct MsgPort *port6a = make_port();
        struct IOSana2Req *io6a = port6a ? make_io(port6a) : NULL;
        struct MsgPort *port6b = make_port();
        struct IOSana2Req *io6b = port6b ? make_io(port6b) : NULL;
        struct MsgPort *port6w = make_port();
        struct IOSana2Req *io6w = port6w ? make_io(port6w) : NULL;

        if (io6a && io6b && io6w &&
            OpenDevice((STRPTR)DEVICE_NAME, 6, (struct IORequest *)io6a, 0) == 0 &&
            OpenDevice((STRPTR)DEVICE_NAME, 6, (struct IORequest *)io6b, 0) == 0 &&
            OpenDevice((STRPTR)DEVICE_NAME, 6, (struct IORequest *)io6w, 0) == 0) {
            int i;

            io6w->ios2_Req.io_Command = S2_ONLINE;
            do_io(io6w);
            ok = (io6w->ios2_Req.io_Error == 0);

            io6a->ios2_Req.io_Command = CMD_READ;
            io6a->ios2_Data = readbuf;
            io6a->ios2_PacketType = 0xD200;
            io6a->ios2_Req.io_Flags = 0;
            send_io(io6a);

            io6b->ios2_Req.io_Command = CMD_READ;
            io6b->ios2_Data = readbuf2;
            io6b->ios2_PacketType = 0xD200;
            io6b->ios2_Req.io_Flags = 0;
            send_io(io6b);

            io6w->ios2_Req.io_Command = CMD_WRITE;
            io6w->ios2_PacketType = 0xD200;
            io6w->ios2_DataLength = sizeof(PAYLOAD) - 1;
            io6w->ios2_Data = (APTR)PAYLOAD;
            for (i = 0; i < SANA2_MAX_ADDR_BYTES; i++)
                io6w->ios2_DstAddr[i] = (i < 6) ? (UBYTE)(0xA0 + i) : 0;
            do_io(io6w);
            ok = ok && (io6w->ios2_Req.io_Error == 0);

            WaitIO((struct IORequest *)io6a);
            WaitIO((struct IORequest *)io6b);
            ok = ok && (io6a->ios2_Req.io_Error == 0) && (io6b->ios2_Req.io_Error == 0);

            CloseDevice((struct IORequest *)io6a);
            CloseDevice((struct IORequest *)io6b);
            CloseDevice((struct IORequest *)io6w);
        } else {
            ok = FALSE;
        }
        if (io6a)
            FreeMem(io6a, sizeof(struct IOSana2Req));
        if (io6b)
            FreeMem(io6b, sizeof(struct IOSana2Req));
        if (io6w)
            FreeMem(io6w, sizeof(struct IOSana2Req));
        if (port6a) {
            FreeSignal(port6a->mp_SigBit);
            FreeMem(port6a, sizeof(struct MsgPort));
        }
        if (port6b) {
            FreeSignal(port6b->mp_SigBit);
            FreeMem(port6b, sizeof(struct MsgPort));
        }
        if (port6w) {
            FreeSignal(port6w->mp_SigBit);
            FreeMem(port6w, sizeof(struct MsgPort));
        }

        report(NAME_FAULTDUP, ok, &fails);
    }

    /* --- faultreorder: unit 7's config (seed=7, reorder=3) produces an
     * EXACT, precomputable delivery permutation for 6 packets sent in
     * order 0..5 -- precomputed on the host with a Python replica of this
     * device's xorshift32()+prng_index() (see fixtures/unit7.config's own
     * comment): writes at (0-indexed) positions 2, 3, 4, 5 each trigger
     * one IMMEDIATE delivery (the reorder buffer reaching its window of
     * 3), in the order [2, 3, 0, 5]; the two packets still buffered
     * afterward (1 and 4) are delivered by a final S2_OFFLINE flush, in
     * their original arrival order. Each write's ios2_PacketType
     * (0x9000+i) identifies which original packet arrived -- simpler than
     * needing distinct payload content on top of distinct types, since
     * S2_READORPHAN (untyped) is what's used to receive. --- */
    {
        BOOL ok = TRUE;
        struct MsgPort *porta = make_port();
        struct IOSana2Req *ioa = porta ? make_io(porta) : NULL; /* reader */
        struct MsgPort *portb = make_port();
        struct IOSana2Req *iob = portb ? make_io(portb) : NULL; /* writer */

        if (ioa && iob &&
            OpenDevice((STRPTR)DEVICE_NAME, 7, (struct IORequest *)ioa, 0) == 0 &&
            OpenDevice((STRPTR)DEVICE_NAME, 7, (struct IORequest *)iob, 0) == 0) {
            static const UWORD expect_immediate[4] = {2, 3, 0, 5};
            static const UWORD expect_flush[2]     = {1, 4};
            int i;

            ioa->ios2_Req.io_Command = S2_ONLINE;
            do_io(ioa);
            ok = ok && (ioa->ios2_Req.io_Error == 0);

            for (i = 0; i < 6; i++) {
                BOOL expect_delivery = (i >= 2); /* writes 2..5 each evict one */

                if (expect_delivery) {
                    ioa->ios2_Req.io_Command = S2_READORPHAN;
                    ioa->ios2_Data = readbuf;
                    ioa->ios2_Req.io_Flags = 0;
                    send_io(ioa);
                }

                iob->ios2_Req.io_Command = CMD_WRITE;
                iob->ios2_PacketType = (ULONG)(0x9000 + i);
                iob->ios2_DataLength = sizeof(PAYLOAD) - 1;
                iob->ios2_Data = (APTR)PAYLOAD;
                {
                    int k;
                    for (k = 0; k < SANA2_MAX_ADDR_BYTES; k++)
                        iob->ios2_DstAddr[k] = (k < 6) ? (UBYTE)(0x80 + k) : 0;
                }
                do_io(iob);
                ok = ok && (iob->ios2_Req.io_Error == 0);

                if (expect_delivery) {
                    WaitIO((struct IORequest *)ioa);
                    ok = ok && (ioa->ios2_Req.io_Error == 0) &&
                         (ioa->ios2_PacketType == (ULONG)(0x9000 + expect_immediate[i - 2]));
                }
            }

            /* Final flush: packets 1 and 4 remain buffered -- S2_OFFLINE
             * delivers both within that one synchronous call. Queue two
             * orphan reads first, in the order they must be satisfied
             * (FIFO orphan queue, oldest-queued first). */
            {
                struct MsgPort *portc = make_port();
                struct IOSana2Req *ioc = portc ? make_io(portc) : NULL;

                if (ioc && OpenDevice((STRPTR)DEVICE_NAME, 7, (struct IORequest *)ioc, 0) == 0) {
                    ioa->ios2_Req.io_Command = S2_READORPHAN;
                    ioa->ios2_Data = readbuf;
                    ioa->ios2_Req.io_Flags = 0;
                    send_io(ioa);

                    ioc->ios2_Req.io_Command = S2_READORPHAN;
                    ioc->ios2_Data = readbuf2;
                    ioc->ios2_Req.io_Flags = 0;
                    send_io(ioc);

                    iob->ios2_Req.io_Command = S2_OFFLINE;
                    do_io(iob);
                    ok = ok && (iob->ios2_Req.io_Error == 0);

                    WaitIO((struct IORequest *)ioa);
                    WaitIO((struct IORequest *)ioc);
                    ok = ok && (ioa->ios2_Req.io_Error == 0) && (ioc->ios2_Req.io_Error == 0) &&
                         (ioa->ios2_PacketType == (ULONG)(0x9000 + expect_flush[0])) &&
                         (ioc->ios2_PacketType == (ULONG)(0x9000 + expect_flush[1]));

                    CloseDevice((struct IORequest *)ioc);
                } else {
                    ok = FALSE;
                }
                if (ioc)
                    FreeMem(ioc, sizeof(struct IOSana2Req));
                if (portc) {
                    FreeSignal(portc->mp_SigBit);
                    FreeMem(portc, sizeof(struct MsgPort));
                }
            }

            CloseDevice((struct IORequest *)ioa);
            CloseDevice((struct IORequest *)iob);
        } else {
            ok = FALSE;
        }
        if (ioa)
            FreeMem(ioa, sizeof(struct IOSana2Req));
        if (iob)
            FreeMem(iob, sizeof(struct IOSana2Req));
        if (porta) {
            FreeSignal(porta->mp_SigBit);
            FreeMem(porta, sizeof(struct MsgPort));
        }
        if (portb) {
            FreeSignal(portb->mp_SigBit);
            FreeMem(portb, sizeof(struct MsgPort));
        }

        report(NAME_FAULTREORDER, ok, &fails);
    }

    /* --- faultoversilent: units 2/3 (M6's default crossover pair) get an
     * extra config knob each for this sub-test alone: unit 2's
     * oversilent=1, unit 3's mtu=500 (both harmless to M6's own test,
     * which only ever writes tiny 20-byte payloads). A write from unit 2
     * within its OWN (default 1500) MTU but over unit 3's must succeed
     * (io_Error==0 -- the caller is never told) yet never actually arrive
     * -- a real PMTUD black hole, the pathology PMTUD itself exists to
     * detect and route around. --- */
    {
        BOOL ok = TRUE;
        struct MsgPort *port2o = make_port();
        struct IOSana2Req *io2o = port2o ? make_io(port2o) : NULL; /* unit 2, writer */
        struct MsgPort *port3o = make_port();
        struct IOSana2Req *io3o = port3o ? make_io(port3o) : NULL; /* unit 3, reader */

        if (io2o && io3o &&
            OpenDevice((STRPTR)DEVICE_NAME, 2, (struct IORequest *)io2o, 0) == 0 &&
            OpenDevice((STRPTR)DEVICE_NAME, 3, (struct IORequest *)io3o, 0) == 0) {
            static UBYTE bigbuf[600];
            int i;
            UBYTE unit3_addr[SANA2_MAX_ADDR_BYTES];

            for (i = 0; i < 600; i++)
                bigbuf[i] = (UBYTE)i;

            io2o->ios2_Req.io_Command = S2_ONLINE;
            do_io(io2o);
            io3o->ios2_Req.io_Command = S2_ONLINE;
            do_io(io3o);
            ok = ok && (io2o->ios2_Req.io_Error == 0) && (io3o->ios2_Req.io_Error == 0);

            io3o->ios2_Req.io_Command = S2_GETSTATIONADDRESS;
            do_io(io3o);
            for (i = 0; i < SANA2_MAX_ADDR_BYTES; i++)
                unit3_addr[i] = io3o->ios2_SrcAddr[i];

            io3o->ios2_Req.io_Command = CMD_READ;
            io3o->ios2_Data = readbuf;
            io3o->ios2_PacketType = 0xD300;
            io3o->ios2_Req.io_Flags = 0;
            send_io(io3o);

            io2o->ios2_Req.io_Command = CMD_WRITE;
            io2o->ios2_PacketType = 0xD300;
            io2o->ios2_DataLength = 600; /* within unit 2's 1500 MTU, over unit 3's 500 */
            io2o->ios2_Data = bigbuf;
            for (i = 0; i < SANA2_MAX_ADDR_BYTES; i++)
                io2o->ios2_DstAddr[i] = unit3_addr[i];
            do_io(io2o);
            ok = ok && (io2o->ios2_Req.io_Error == 0); /* accepted... */

            AbortIO((struct IORequest *)io3o);
            WaitIO((struct IORequest *)io3o);
            ok = ok && (io3o->ios2_Req.io_Error == IOERR_ABORTED); /* ...but vanished */

            CloseDevice((struct IORequest *)io2o);
            CloseDevice((struct IORequest *)io3o);
        } else {
            ok = FALSE;
        }
        if (io2o)
            FreeMem(io2o, sizeof(struct IOSana2Req));
        if (io3o)
            FreeMem(io3o, sizeof(struct IOSana2Req));
        if (port2o) {
            FreeSignal(port2o->mp_SigBit);
            FreeMem(port2o, sizeof(struct MsgPort));
        }
        if (port3o) {
            FreeSignal(port3o->mp_SigBit);
            FreeMem(port3o, sizeof(struct MsgPort));
        }

        report(NAME_FAULTOVERSILENT, ok, &fails);
    }

    /* --- faultflap: unit 8's config (offlineafter=2, onlineafter=2)
     * auto-flaps online/offline based on write-attempt counts, simulating
     * a flaky link that recovers on its own. Writes 1-2 succeed; write 3
     * (crossing offlineafter's threshold from write 2) finds the unit
     * already auto-offline; writes 3-4 both fail S2ERR_OUTOFSERVICE
     * (failed-attempt count reaching onlineafter's threshold on write 4);
     * write 5 succeeds again, the unit having auto-recovered. --- */
    {
        BOOL ok = TRUE;
        struct MsgPort *port8 = make_port();
        struct IOSana2Req *io8 = port8 ? make_io(port8) : NULL;

        if (io8 && OpenDevice((STRPTR)DEVICE_NAME, 8, (struct IORequest *)io8, 0) == 0) {
            int i;

            io8->ios2_Req.io_Command = S2_ONLINE;
            do_io(io8);
            ok = (io8->ios2_Req.io_Error == 0);

            for (i = 0; i < 2; i++) {
                int k;
                io8->ios2_Req.io_Command = CMD_WRITE;
                io8->ios2_PacketType = 0xD400;
                io8->ios2_DataLength = sizeof(PAYLOAD) - 1;
                io8->ios2_Data = (APTR)PAYLOAD;
                for (k = 0; k < SANA2_MAX_ADDR_BYTES; k++)
                    io8->ios2_DstAddr[k] = (k < 6) ? (UBYTE)(0xB0 + k) : 0;
                do_io(io8);
                ok = ok && (io8->ios2_Req.io_Error == 0);
            }

            /* write 3: unit auto-went offline after write 2 above. */
            do_io(io8);
            ok = ok && (io8->ios2_Req.io_Error == S2ERR_OUTOFSERVICE);

            /* write 4: 2nd failed attempt -- triggers auto-recovery for
             * the NEXT attempt, but this one still fails. */
            do_io(io8);
            ok = ok && (io8->ios2_Req.io_Error == S2ERR_OUTOFSERVICE);

            /* write 5: unit auto-recovered -- succeeds again. */
            do_io(io8);
            ok = ok && (io8->ios2_Req.io_Error == 0);

            CloseDevice((struct IORequest *)io8);
        } else {
            ok = FALSE;
        }
        if (io8)
            FreeMem(io8, sizeof(struct IOSana2Req));
        if (port8) {
            FreeSignal(port8->mp_SigBit);
            FreeMem(port8, sizeof(struct MsgPort));
        }

        report(NAME_FAULTFLAP, ok, &fails);
    }

    /* --- replay: unit 9's config (mode=replay,
     * replay=S:sana2loop/replay.pcap) plays back a 3-record fixture
     * capture in order via CMD_READ -- deliberately submitted with a
     * wrong, non-matching ios2_PacketType each time, proving "consume
     * mode" ignores typing entirely (M9's real tick-scheduled replay is
     * the only thing this milestone leaves undone; this is a plain
     * in-order drain). Once exhausted, a further CMD_READ just queues
     * like any other unit with nothing waiting -- reclaimed via AbortIO,
     * same pattern the abortio/faultoversilent sub-tests already use --
     * and CMD_WRITE fails S2ERR_NOT_SUPPORTED, since a canned trace has
     * nowhere to send anything. --- */
    {
        BOOL ok = TRUE;
        struct MsgPort *port9 = make_port();
        struct IOSana2Req *io9 = port9 ? make_io(port9) : NULL;

        if (io9 && OpenDevice((STRPTR)DEVICE_NAME, 9, (struct IORequest *)io9, 0) == 0) {
            int i;

            for (i = 0; i < 3; i++) {
                int k;

                io9->ios2_Req.io_Command = CMD_READ;
                io9->ios2_PacketType = 0xFFFF; /* deliberately wrong */
                io9->ios2_Data = readbuf;
                io9->ios2_Req.io_Flags = 0;
                do_io(io9);

                ok = ok && (io9->ios2_Req.io_Error == 0) &&
                     (io9->ios2_PacketType == REPLAY_TYPE[i]) &&
                     (io9->ios2_DataLength == REPLAY_LEN[i]);
                for (k = 0; k < 6; k++) {
                    if (io9->ios2_DstAddr[k] != REPLAY_DST[i][k])
                        ok = FALSE;
                    if (io9->ios2_SrcAddr[k] != REPLAY_SRC[i][k])
                        ok = FALSE;
                }
                for (k = 0; k < (int)REPLAY_LEN[i]; k++)
                    if (readbuf[k] != (UBYTE)REPLAY_PAY[i][k])
                        ok = FALSE;
            }

            /* capture exhausted -- next read just queues, doesn't complete */
            io9->ios2_Req.io_Command = CMD_READ;
            io9->ios2_PacketType = 0;
            io9->ios2_Data = readbuf;
            io9->ios2_Req.io_Flags = 0;
            send_io(io9);
            AbortIO((struct IORequest *)io9);
            WaitIO((struct IORequest *)io9);
            ok = ok && (io9->ios2_Req.io_Error == IOERR_ABORTED);

            /* replay-mode CMD_WRITE: unsupported */
            {
                int k;
                io9->ios2_Req.io_Command = CMD_WRITE;
                io9->ios2_PacketType = 0x0900;
                io9->ios2_DataLength = sizeof(PAYLOAD) - 1;
                io9->ios2_Data = (APTR)PAYLOAD;
                for (k = 0; k < SANA2_MAX_ADDR_BYTES; k++)
                    io9->ios2_DstAddr[k] = (k < 6) ? (UBYTE)(0xC0 + k) : 0;
            }
            do_io(io9);
            ok = ok && (io9->ios2_Req.io_Error == S2ERR_NOT_SUPPORTED);

            CloseDevice((struct IORequest *)io9);
        } else {
            ok = FALSE;
        }
        if (io9)
            FreeMem(io9, sizeof(struct IOSana2Req));
        if (port9) {
            FreeSignal(port9->mp_SigBit);
            FreeMem(port9, sizeof(struct MsgPort));
        }

        report(NAME_REPLAY, ok, &fails);
    }

    /* --- scheduled: unit 10 (mode=replay, replaymode=scheduled) delivers
     * its 2-record fixture on real VBlank-driven timing rather than
     * on-demand -- the actual point of M9, unlike M8's consume mode. A
     * queued CMD_READ submitted before a record's due tick genuinely
     * WAITS, completing only once vblank_server() delivers it through the
     * normal deliver_packet() precedence. A real S2_CopyToBuff hook
     * (matching the bufferhooks sub-test's own pattern) is registered on
     * the reading open and its invocation count checked, proving it gets
     * called correctly from ACTUAL VBlank interrupt context -- every
     * other sub-test in this file only ever exercises hooks from ordinary
     * task context. --- */
    {
        BOOL ok = TRUE;
        struct MsgPort *port10a = make_port();
        struct IOSana2Req *io10a = port10a ? make_io(port10a) : NULL; /* reader, with hook */
        struct MsgPort *port10b = make_port();
        struct IOSana2Req *io10b = port10b ? make_io(port10b) : NULL; /* S2_ONLINE control */
        struct TagItem tags10[2];

        if (io10a) {
            tags10[0].ti_Tag  = S2_CopyToBuff;
            tags10[0].ti_Data = (ULONG)counting_copy_to;
            tags10[1].ti_Tag  = TAG_DONE;
            tags10[1].ti_Data = 0;
            io10a->ios2_BufferManagement = tags10;
        }

        if (io10a && io10b &&
            OpenDevice((STRPTR)DEVICE_NAME, 10, (struct IORequest *)io10a, 0) == 0 &&
            OpenDevice((STRPTR)DEVICE_NAME, 10, (struct IORequest *)io10b, 0) == 0) {
            int k;
            g_HookToCount = 0;

            /* Record 0: due tick 0 -- delivers on the very first VBlank
             * after going online. Read submitted BEFORE S2_ONLINE, same
             * as this file's other pre-queue-then-trigger tests.
             * S2_READORPHAN (untyped), not CMD_READ: unlike M8's consume
             * mode, SCHEDULED delivery goes through the REAL
             * deliver_packet() precedence, which DOES respect CMD_READ's
             * typing -- S2_READORPHAN is the correct "give me whatever's
             * next" tool here, not a workaround. */
            io10a->ios2_Req.io_Command = S2_READORPHAN;
            io10a->ios2_Data = readbuf;
            io10a->ios2_Req.io_Flags = 0;
            send_io(io10a);

            io10b->ios2_Req.io_Command = S2_ONLINE;
            do_io(io10b);

            /* See reap_interrupt_io()'s own comment: this is where a
             * plain WaitIO() (or a WaitIO() gated on CheckIO()) would
             * hang on bundled AROS specifically. */
            reap_interrupt_io(io10a, &g_HookToCount, 1);
            ok = ok && (io10a->ios2_Req.io_Error == 0) &&
                 (io10a->ios2_PacketType == 0x0800) &&
                 (io10a->ios2_DataLength == sizeof(SCHED_PAY0) - 1) &&
                 g_HookToCount >= 1;
            for (k = 0; k < 6; k++) {
                if (io10a->ios2_DstAddr[k] != SCHED_DST0[k])
                    ok = FALSE;
                if (io10a->ios2_SrcAddr[k] != SCHED_SRC0[k])
                    ok = FALSE;
            }
            for (k = 0; k < (int)(sizeof(SCHED_PAY0) - 1); k++)
                if (readbuf[k] != (UBYTE)SCHED_PAY0[k])
                    ok = FALSE;

            /* Record 1: due tick 50 (1 real second later at
             * SANA2LOOP_VBLANK_HZ). Same reap_interrupt_io() treatment as
             * record 0 above -- this is the same interrupt-driven
             * completion path, so it hits the identical bundled-AROS
             * quirk. */
            io10a->ios2_Req.io_Command = S2_READORPHAN; /* untyped, see record 0's comment above */
            io10a->ios2_Data = readbuf2;
            io10a->ios2_Req.io_Flags = 0;
            send_io(io10a);

            reap_interrupt_io(io10a, &g_HookToCount, 2);
            ok = ok && (io10a->ios2_Req.io_Error == 0) &&
                 (io10a->ios2_PacketType == 0x0801) &&
                 (io10a->ios2_DataLength == sizeof(SCHED_PAY1) - 1) &&
                 g_HookToCount >= 2;
            for (k = 0; k < 6; k++) {
                if (io10a->ios2_DstAddr[k] != SCHED_DST1[k])
                    ok = FALSE;
                if (io10a->ios2_SrcAddr[k] != SCHED_SRC1[k])
                    ok = FALSE;
            }
            for (k = 0; k < (int)(sizeof(SCHED_PAY1) - 1); k++)
                if (readbuf2[k] != (UBYTE)SCHED_PAY1[k])
                    ok = FALSE;

            CloseDevice((struct IORequest *)io10a);
            CloseDevice((struct IORequest *)io10b);
        } else {
            ok = FALSE;
        }
        if (io10a)
            FreeMem(io10a, sizeof(struct IOSana2Req));
        if (io10b)
            FreeMem(io10b, sizeof(struct IOSana2Req));
        if (port10a) {
            FreeSignal(port10a->mp_SigBit);
            FreeMem(port10a, sizeof(struct MsgPort));
        }
        if (port10b) {
            FreeSignal(port10b->mp_SigBit);
            FreeMem(port10b, sizeof(struct MsgPort));
        }

        report(NAME_SCHEDULED, ok, &fails);
    }

    /* --- rawmtu: Rev 4's Sana2DeviceQuery.RawMTU, queried via the local
     * struct Sana2LoopDeviceQuery (this toolchain's <devices/sana2.h> has
     * no RawMTU field at all -- see that struct's own comment above).
     * Echo mode (unit 0, io1): RawMTU == MTU + the same 14-byte
     * SANA2IOF_RAW allowance M4 already gives raw writes (docs/PLAN.md's
     * "buffer -> MTU+14"). Replay mode (unit 9, reopened fresh -- M8's
     * own test already closed it): RawMTU == 0, since a replay unit never
     * accepts ANY write, raw or otherwise (do_write() rejects it
     * outright). --- */
    {
        struct Sana2LoopDeviceQuery dq;
        BOOL echo_ok, replay_ok = FALSE;
        struct MsgPort *port9r = make_port();
        struct IOSana2Req *io9r = port9r ? make_io(port9r) : NULL;

        dq.SizeAvailable = sizeof(dq);
        io1->ios2_Req.io_Command = S2_DEVICEQUERY;
        io1->ios2_StatData = &dq;
        do_io(io1);
        echo_ok = (io1->ios2_Req.io_Error == 0) &&
                  dq.SizeSupplied == sizeof(dq) &&
                  dq.RawMTU == dq.MTU + 14;

        if (io9r && OpenDevice((STRPTR)DEVICE_NAME, 9, (struct IORequest *)io9r, 0) == 0) {
            struct Sana2LoopDeviceQuery dq9;
            dq9.SizeAvailable = sizeof(dq9);
            io9r->ios2_Req.io_Command = S2_DEVICEQUERY;
            io9r->ios2_StatData = &dq9;
            do_io(io9r);
            replay_ok = (io9r->ios2_Req.io_Error == 0) &&
                        dq9.SizeSupplied == sizeof(dq9) &&
                        dq9.RawMTU == 0;
            CloseDevice((struct IORequest *)io9r);
        }
        if (io9r)
            FreeMem(io9r, sizeof(struct IOSana2Req));
        if (port9r) {
            FreeSignal(port9r->mp_SigBit);
            FreeMem(port9r, sizeof(struct MsgPort));
        }

        report(NAME_RAWMTU, echo_ok && replay_ok, &fails);
    }

    /* --- peeraddress: Rev 4's S2_GETPEERADDRESS. Echo mode (unit 0, io1)
     * has no real peer -- S2ERR_NOT_SUPPORTED. Crossover mode (units 2/3,
     * reopened fresh -- M6/M7's own tests already closed them): each side
     * reports the OTHER's real station address as ios2_DstAddr and its
     * own as ios2_SrcAddr, the same current/factory reuse convention
     * S2_GETSTATIONADDRESS already established for ios2_SrcAddr/DstAddr,
     * applied here to local/peer instead. --- */
    {
        BOOL echo_ok, cross_ok = FALSE;
        struct MsgPort *port2p = make_port();
        struct IOSana2Req *io2p = port2p ? make_io(port2p) : NULL;
        struct MsgPort *port3p = make_port();
        struct IOSana2Req *io3p = port3p ? make_io(port3p) : NULL;

        io1->ios2_Req.io_Command = S2_GETPEERADDRESS;
        do_io(io1);
        echo_ok = (io1->ios2_Req.io_Error == S2ERR_NOT_SUPPORTED);

        if (io2p && io3p &&
            OpenDevice((STRPTR)DEVICE_NAME, 2, (struct IORequest *)io2p, 0) == 0 &&
            OpenDevice((STRPTR)DEVICE_NAME, 3, (struct IORequest *)io3p, 0) == 0) {
            int i;
            UBYTE addr2[SANA2_MAX_ADDR_BYTES], addr3[SANA2_MAX_ADDR_BYTES];

            io2p->ios2_Req.io_Command = S2_GETSTATIONADDRESS;
            do_io(io2p);
            for (i = 0; i < SANA2_MAX_ADDR_BYTES; i++)
                addr2[i] = io2p->ios2_SrcAddr[i];
            io3p->ios2_Req.io_Command = S2_GETSTATIONADDRESS;
            do_io(io3p);
            for (i = 0; i < SANA2_MAX_ADDR_BYTES; i++)
                addr3[i] = io3p->ios2_SrcAddr[i];

            io2p->ios2_Req.io_Command = S2_GETPEERADDRESS;
            do_io(io2p);
            io3p->ios2_Req.io_Command = S2_GETPEERADDRESS;
            do_io(io3p);

            cross_ok = (io2p->ios2_Req.io_Error == 0) && (io3p->ios2_Req.io_Error == 0);
            for (i = 0; cross_ok && i < SANA2_MAX_ADDR_BYTES; i++) {
                if (io2p->ios2_SrcAddr[i] != addr2[i] || io2p->ios2_DstAddr[i] != addr3[i])
                    cross_ok = FALSE;
                if (io3p->ios2_SrcAddr[i] != addr3[i] || io3p->ios2_DstAddr[i] != addr2[i])
                    cross_ok = FALSE;
            }

            CloseDevice((struct IORequest *)io2p);
            CloseDevice((struct IORequest *)io3p);
        }
        if (io2p)
            FreeMem(io2p, sizeof(struct IOSana2Req));
        if (io3p)
            FreeMem(io3p, sizeof(struct IOSana2Req));
        if (port2p) {
            FreeSignal(port2p->mp_SigBit);
            FreeMem(port2p, sizeof(struct MsgPort));
        }
        if (port3p) {
            FreeSignal(port3p->mp_SigBit);
            FreeMem(port3p, sizeof(struct MsgPort));
        }

        report(NAME_PEERADDRESS, echo_ok && cross_ok, &fails);
    }

    /* --- dnsaddress: Rev 4's S2_GETDNSADDRESS -- this device has no real
     * DNS concept on any unit, so every unit succeeds with both addresses
     * zero-filled (docs/PLAN.md's own M13 section: "zero-fill per spec
     * when N/A") -- deliberately a DIFFERENT not-applicable shape from
     * peeraddress's hard S2ERR_NOT_SUPPORTED above: a real stack has to
     * tolerate both. --- */
    {
        BOOL ok;
        int i;
        io1->ios2_Req.io_Command = S2_GETDNSADDRESS;
        do_io(io1);
        ok = (io1->ios2_Req.io_Error == 0);
        for (i = 0; ok && i < SANA2_MAX_ADDR_BYTES; i++)
            if (io1->ios2_SrcAddr[i] != 0 || io1->ios2_DstAddr[i] != 0)
                ok = FALSE;
        report(NAME_DNSADDRESS, ok, &fails);
    }

    /* --- buffhooks: Rev 2/3 hook-width negotiation, actually INVOKED not
     * just accepted -- unit 13's fixture config (buffhooks=32) makes this
     * unit prefer S2_CopyToBuff32/S2_CopyFromBuff32 for whichever opener
     * actually negotiated them (pick_copy_to_buff()/pick_copy_from_buff()
     * in loopback_device.c), falling back to an opener's own base hook
     * when it didn't. io13a negotiates ONLY the 32-bit hooks; io13b
     * negotiates ONLY the base 8-bit ones -- two independent opens on the
     * same echo unit, same two-independent-opens pattern the
     * "bufferhooks" sub-test already established for the base hooks
     * alone. Two exchanges cover all four combinations (negotiated-and-
     * used / not-negotiated-falls-back, on both the read and write
     * side). --- */
    {
        BOOL ok = TRUE;
        struct MsgPort *port13a = make_port();
        struct IOSana2Req *io13a = port13a ? make_io(port13a) : NULL;
        struct MsgPort *port13b = make_port();
        struct IOSana2Req *io13b = port13b ? make_io(port13b) : NULL;

        if (io13a && io13b) {
            struct TagItem tags32[3];
            struct TagItem tagsbase[3];

            tags32[0].ti_Tag  = S2_CopyToBuff32;
            tags32[0].ti_Data = (ULONG)counting_copy_to32;
            tags32[1].ti_Tag  = S2_CopyFromBuff32;
            tags32[1].ti_Data = (ULONG)counting_copy_from32;
            tags32[2].ti_Tag  = TAG_DONE;
            tags32[2].ti_Data = 0;
            io13a->ios2_BufferManagement = tags32;

            tagsbase[0].ti_Tag  = S2_CopyToBuff;
            tagsbase[0].ti_Data = (ULONG)counting_copy_to;
            tagsbase[1].ti_Tag  = S2_CopyFromBuff;
            tagsbase[1].ti_Data = (ULONG)counting_copy_from;
            tagsbase[2].ti_Tag  = TAG_DONE;
            tagsbase[2].ti_Data = 0;
            io13b->ios2_BufferManagement = tagsbase;

            if (OpenDevice((STRPTR)DEVICE_NAME, 13, (struct IORequest *)io13a, 0) == 0 &&
                OpenDevice((STRPTR)DEVICE_NAME, 13, (struct IORequest *)io13b, 0) == 0) {
                int i;
                BOOL a_ok, b_ok;

                io13a->ios2_Req.io_Command = S2_ONLINE;
                do_io(io13a);

                g_Hook32ToCount = g_Hook32FromCount = 0;
                g_HookToCount = g_HookFromCount = 0;

                /* Exchange A: io13a reads (negotiated 32-bit), io13b
                 * writes (base only -- never negotiated 32-bit, must
                 * still work via graceful fallback). */
                io13a->ios2_Req.io_Command = CMD_READ;
                io13a->ios2_Data = readbuf;
                io13a->ios2_PacketType = 0x3200;
                io13a->ios2_Req.io_Flags = 0;
                send_io(io13a);

                io13b->ios2_Req.io_Command = CMD_WRITE;
                for (i = 0; i < SANA2_MAX_ADDR_BYTES; i++)
                    io13b->ios2_DstAddr[i] = (i < 6) ? (UBYTE)(0x30 + i) : 0;
                io13b->ios2_PacketType = 0x3200;
                io13b->ios2_DataLength = sizeof(PAYLOAD) - 1;
                io13b->ios2_Data = (APTR)PAYLOAD;
                do_io(io13b);

                WaitIO((struct IORequest *)io13a);
                a_ok = (io13a->ios2_Req.io_Error == 0) && (io13b->ios2_Req.io_Error == 0) &&
                       g_Hook32ToCount >= 1 && g_HookToCount == 0 &&
                       g_HookFromCount >= 1 && g_Hook32FromCount == 0 &&
                       io13a->ios2_DataLength == sizeof(PAYLOAD) - 1;
                if (a_ok)
                    for (i = 0; i < (int)io13a->ios2_DataLength; i++)
                        if (readbuf[i] != (UBYTE)PAYLOAD[i])
                            a_ok = FALSE;

                g_Hook32ToCount = g_Hook32FromCount = 0;
                g_HookToCount = g_HookFromCount = 0;

                /* Exchange B: io13b reads (base only -- never negotiated
                 * 32-bit, must still work via graceful fallback), io13a
                 * writes (negotiated 32-bit). */
                io13b->ios2_Req.io_Command = CMD_READ;
                io13b->ios2_Data = readbuf;
                io13b->ios2_PacketType = 0x3201;
                io13b->ios2_Req.io_Flags = 0;
                send_io(io13b);

                io13a->ios2_Req.io_Command = CMD_WRITE;
                for (i = 0; i < SANA2_MAX_ADDR_BYTES; i++)
                    io13a->ios2_DstAddr[i] = (i < 6) ? (UBYTE)(0x31 + i) : 0;
                io13a->ios2_PacketType = 0x3201;
                io13a->ios2_DataLength = sizeof(PAYLOAD) - 1;
                io13a->ios2_Data = (APTR)PAYLOAD;
                do_io(io13a);

                WaitIO((struct IORequest *)io13b);
                b_ok = (io13b->ios2_Req.io_Error == 0) && (io13a->ios2_Req.io_Error == 0) &&
                       g_HookToCount >= 1 && g_Hook32ToCount == 0 &&
                       g_Hook32FromCount >= 1 && g_HookFromCount == 0 &&
                       io13b->ios2_DataLength == sizeof(PAYLOAD) - 1;
                if (b_ok)
                    for (i = 0; i < (int)io13b->ios2_DataLength; i++)
                        if (readbuf[i] != (UBYTE)PAYLOAD[i])
                            b_ok = FALSE;

                ok = a_ok && b_ok;
                CloseDevice((struct IORequest *)io13a);
                CloseDevice((struct IORequest *)io13b);
            } else {
                ok = FALSE;
            }
        } else {
            ok = FALSE;
        }
        if (io13a)
            FreeMem(io13a, sizeof(struct IOSana2Req));
        if (io13b)
            FreeMem(io13b, sizeof(struct IOSana2Req));
        if (port13a) {
            FreeSignal(port13a->mp_SigBit);
            FreeMem(port13a, sizeof(struct MsgPort));
        }
        if (port13b) {
            FreeSignal(port13b->mp_SigBit);
            FreeMem(port13b, sizeof(struct MsgPort));
        }

        report(NAME_BUFFHOOKS, ok, &fails);
    }

    /* --- dmahooks: Rev 7 DMA hook negotiation -- accepted/recorded at
     * open() time (su_DMAHooksOffered), surfaced via S2_GETSPECIALSTATS
     * as SANA2LOOP_SPECIALSTAT_DMA_HOOKS_OFFERED, but NEVER actually
     * invoked (this device always gracefully declines DMA -- see
     * apply_buffer_mgmt_tags()'s own comment in loopback_device.c). A
     * normal read/write exchange through an open that offered a DMA hook
     * must still complete correctly via the base hook, with the DMA stub
     * left uncalled. --- */
    {
        BOOL ok = TRUE;
        struct MsgPort *port14 = make_port();
        struct IOSana2Req *io14 = port14 ? make_io(port14) : NULL;

        if (io14) {
            struct TagItem tagsdma[2];
            tagsdma[0].ti_Tag  = S2_DMACopyToBuff32;
            tagsdma[0].ti_Data = (ULONG)never_call_dma;
            tagsdma[1].ti_Tag  = TAG_DONE;
            tagsdma[1].ti_Data = 0;
            io14->ios2_BufferManagement = tagsdma;

            if (OpenDevice((STRPTR)DEVICE_NAME, 0, (struct IORequest *)io14, 0) == 0) {
                struct {
                    struct Sana2SpecialStatHeader hdr;
                    struct Sana2SpecialStatRecord recs[2];
                } stats;
                int i, found_dma = 0;

                g_DMAHookCalls = 0;

                io14->ios2_Req.io_Command = CMD_READ;
                io14->ios2_Data = readbuf;
                io14->ios2_PacketType = 0x0DA0;
                io14->ios2_Req.io_Flags = 0;
                send_io(io14);

                io2->ios2_Req.io_Command = CMD_WRITE;
                for (i = 0; i < SANA2_MAX_ADDR_BYTES; i++)
                    io2->ios2_DstAddr[i] = (i < 6) ? (UBYTE)(0xDA + i) : 0;
                io2->ios2_PacketType = 0x0DA0;
                io2->ios2_DataLength = sizeof(PAYLOAD) - 1;
                io2->ios2_Data = (APTR)PAYLOAD;
                do_io(io2);

                WaitIO((struct IORequest *)io14);
                ok = (io14->ios2_Req.io_Error == 0) && (io2->ios2_Req.io_Error == 0) &&
                     g_DMAHookCalls == 0 &&
                     io14->ios2_DataLength == sizeof(PAYLOAD) - 1;
                if (ok)
                    for (i = 0; i < (int)io14->ios2_DataLength; i++)
                        if (readbuf[i] != (UBYTE)PAYLOAD[i])
                            ok = FALSE;

                stats.hdr.RecordCountMax = 2;
                io1->ios2_Req.io_Command = S2_GETSPECIALSTATS;
                io1->ios2_StatData = &stats;
                do_io(io1);
                if (io1->ios2_Req.io_Error == 0)
                    for (i = 0; i < (int)stats.hdr.RecordCountSupplied; i++)
                        if (stats.recs[i].Type == SANA2LOOP_SPECIALSTAT_DMA_HOOKS_OFFERED &&
                            stats.recs[i].Count >= 1)
                            found_dma = 1;
                ok = ok && (io1->ios2_Req.io_Error == 0) && found_dma;

                CloseDevice((struct IORequest *)io14);
            } else {
                ok = FALSE;
            }
        } else {
            ok = FALSE;
        }
        if (io14)
            FreeMem(io14, sizeof(struct IOSana2Req));
        if (port14) {
            FreeSignal(port14->mp_SigBit);
            FreeMem(port14, sizeof(struct MsgPort));
        }

        report(NAME_DMAHOOKS, ok, &fails);
    }

    /* --- replaycap: M14's `replaycap=N` config knob. Unit 14's fixture
     * (replaycap=0, out of the valid 1..SANA2LOOP_CONFIG_MAX_REPLAYCAP
     * range) must fail OpenDevice() at pure config validation --
     * S2ERR_BAD_ARGUMENT, before any replay-mode/allocation logic even
     * runs. Deliberately the ONLY on-target coverage for this knob in
     * this session: the positive case (a raised cap actually loading
     * more records than the default) needs a real replay-buffer
     * allocation, which reliably exceeds bundled AROS's tight memory
     * budget at this point in the sequence (a third simultaneously-held
     * replay buffer, alongside units 9/10's own) -- verified instead via
     * real Kickstart 1.3/3.1 locally; see docs/PLAN.md's M14 risk note
     * for the full investigation. --- */
    {
        BOOL ok = FALSE;
        struct MsgPort *port14c = make_port();
        struct IOSana2Req *io14c = port14c ? make_io(port14c) : NULL;

        if (io14c) {
            ok = (OpenDevice((STRPTR)DEVICE_NAME, 14, (struct IORequest *)io14c, 0) != 0);
            if (!ok)
                CloseDevice((struct IORequest *)io14c); /* shouldn't happen; clean up if it does */
        }
        if (io14c)
            FreeMem(io14c, sizeof(struct IOSana2Req));
        if (port14c) {
            FreeSignal(port14c->mp_SigBit);
            FreeMem(port14c, sizeof(struct MsgPort));
        }

        report(NAME_REPLAYCAP, ok, &fails);
    }

    /* --- record: unit 11 (`record=S:sana2loop/recorded.pcap`,
     * recordmax=4096) taps 3 known echo writes into a RAM buffer, flushed
     * to that file at CloseDevice() (last close). No queued reader is
     * needed -- a plain CMD_WRITE with nobody listening still "succeeds"
     * from the writer's perspective under this device's strict rxqueue=0
     * default (the packet is simply dropped after delivery, per M2), and
     * record_append() taps deliver_packet() unconditionally regardless of
     * that outcome. run.sh extracts the flushed file off the ADF
     * post-run (via xdftool) and byte-compares it against these same 3
     * writes -- this sub-test only proves the writes themselves succeed;
     * the actual recorded-bytes assertion lives in run.sh, since verifying
     * what ended up on the host-side ADF is inherently a host-side
     * question, not something the on-target harness can check about
     * itself. --- */
    {
        BOOL ok = TRUE;
        struct MsgPort *port11 = make_port();
        struct IOSana2Req *io11 = port11 ? make_io(port11) : NULL;

        if (io11 && OpenDevice((STRPTR)DEVICE_NAME, 11, (struct IORequest *)io11, 0) == 0) {
            int i;

            io11->ios2_Req.io_Command = S2_ONLINE;
            do_io(io11);
            ok = (io11->ios2_Req.io_Error == 0);

            for (i = 0; i < 3; i++) {
                int k;
                io11->ios2_Req.io_Command = CMD_WRITE;
                io11->ios2_PacketType = RECORD_TYPE[i];
                io11->ios2_DataLength = RECORD_LEN[i];
                io11->ios2_Data = (APTR)RECORD_PAY[i];
                for (k = 0; k < SANA2_MAX_ADDR_BYTES; k++)
                    io11->ios2_DstAddr[k] = (k < 6) ? RECORD_WROTE_TO[k] : 0;
                do_io(io11);
                ok = ok && (io11->ios2_Req.io_Error == 0);
            }

            CloseDevice((struct IORequest *)io11);
        } else {
            ok = FALSE;
        }
        if (io11)
            FreeMem(io11, sizeof(struct IOSana2Req));
        if (port11) {
            FreeSignal(port11->mp_SigBit);
            FreeMem(port11, sizeof(struct MsgPort));
        }

        report(NAME_RECORD, ok, &fails);
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

/* M13: registered as S2_CopyToBuff32/S2_CopyFromBuff32 for the buffhooks
 * sub-test -- same ABI as the base 8-bit hooks above (confirmed by
 * wiki.amigaos.net's own "Revision 2+3" page: "the same calling interface
 * as for S2_CopyToBuff or S2_CopyFromBuff, respectively"). */
static BOOL counting_copy_to32(APTR to asm("a0"), APTR from asm("a1"), ULONG size asm("d0"))
{
    CopyMem(from, to, size);
    g_Hook32ToCount++;
    return TRUE;
}

static BOOL counting_copy_from32(APTR to asm("a0"), APTR from asm("a1"), ULONG size asm("d0"))
{
    CopyMem(from, to, size);
    g_Hook32FromCount++;
    return TRUE;
}

/* M13: registered as S2_DMACopyToBuff32 for the dmahooks sub-test. Must
 * NEVER actually be called -- this device always gracefully declines Rev 7
 * DMA hooks (see apply_buffer_mgmt_tags()'s own comment in
 * loopback_device.c), so a real write/read exchange through an open that
 * offered this must complete normally through the base hook while this
 * stays uncalled. The real DMA hook calling convention doesn't matter
 * here since it's never invoked either way -- this signature is a
 * plausible guess, not a confirmed one. */
static APTR never_call_dma(APTR data asm("a0"))
{
    (void)data;
    g_DMAHookCalls++;
    return NULL;
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

/* Reaps an IOSana2Req whose completion is expected to arrive via a REAL
 * hardware-interrupt-driven ReplyMsg() (M9's vblank_server(), not a
 * task-context BeginIO() completion like every other sub-test in this
 * file) -- needed because bundled AROS under Copperline has a real
 * completion-detection bug for exactly that case: CheckIO() (and, worse,
 * a plain WaitIO() called right after SendIO()) can both falsely report
 * the request as already complete before the device has actually
 * replied it. Confirmed empirically by instrumenting the device itself
 * (a stashed per-stage progress marker inside deliver_packet() showed it
 * had never even been entered at the exact moment WaitIO() returned) --
 * this is a bundled-AROS limitation, not a device bug: this exact
 * sub-test passes with a plain WaitIO() on real Kickstart 1.3.
 *
 * Waiting on `*evidence` reaching `want` instead -- a side effect
 * (g_HookToCount) only the REAL S2_CopyToBuff hook invocation inside
 * fill_read_from_packet() can produce -- sidesteps the false positive
 * entirely, since it can't be satisfied by anything other than genuine
 * delivery. The bound below is deliberately generous (the actual wait is
 * usually a single VBlank period; this is just a safety ceiling against
 * a genuine regression hanging the whole suite) and the loop exits the
 * instant the real evidence appears.
 *
 * AbortIO() then WaitIO() run UNCONDITIONALLY afterward, regardless of
 * whether the spin found real evidence -- AbortIO() on an
 * already-completed request is a documented no-op, so this is always
 * safe, and it's the only way to guarantee `io` is cleanly reaped (out
 * of su_OrphanReads and its message properly replied) before any later
 * reuse of the same IORequest: if the spin bound is ever hit for real
 * (genuine regression, not just the AROS quirk), the alternative --
 * silently reusing an IORequest that's still actually enqueued in the
 * device's own pending-reads list -- would double-insert it on the next
 * SendIO() and corrupt that list, exactly the kind of bug this helper
 * exists to prevent, not just paper over. */
static void reap_interrupt_io(struct IOSana2Req *io, volatile ULONG *evidence, ULONG want)
{
    long spins;

    for (spins = 0; spins < 50000000L && *evidence < want; spins++)
        ;
    AbortIO((struct IORequest *)io);
    WaitIO((struct IORequest *)io);
}

/* Neither exec.library's SendIO() nor DoIO() preserve caller-set io_Flags
 * bits (like SANA2IOF_RAW) -- their own autodocs say so plainly: SendIO()
 * "will be set to zero before the request is sent," DoIO() "is always set
 * to IOF_QUICK (0x01) before the internal device call." amiga.lib's
 * BeginIO() is documented as "equivalent to SendIO(), except that io_Flags
 * is not cleared" -- exactly what a caller wanting SANA2IOF_RAW honored
 * needs -- but this freestanding build links no amiga.lib. Call the
 * device's BeginIO vector directly instead: same a6=devbase/a1=IORequest
 * convention this project's own device uses for begin_io() (see
 * loopback_device.c), reached at LVO offset -30, the standard Exec device
 * jump-table slot (open=-6, close=-12, expunge=-18, ExtFunc=-24,
 * BeginIO=-30, AbortIO=-36) -- exactly what SendIO()/DoIO() already jump
 * to internally, just without their io_Flags-clearing side effect first. */
static void flagged_begin_io(struct IOSana2Req *io)
{
    io->ios2_Req.io_Message.mn_Node.ln_Type = NT_MESSAGE;
    {
        register struct Library *a6 __asm__("a6") = (struct Library *)io->ios2_Req.io_Device;
        register struct IORequest *a1 __asm__("a1") = (struct IORequest *)io;
        __asm__ volatile("jsr -30(%%a6)" : : "r"(a6), "r"(a1)
                          : "d0", "d1", "a0", "a1", "cc", "memory");
    }
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
