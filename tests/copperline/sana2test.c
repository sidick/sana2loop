/* sana2test.c -- on-target (m68k/AmigaOS) smoke test for loopback.device.
 *
 * Opens loopback.device unit 0, brings it online, checks S2_DEVICEQUERY and
 * S2_GETSTATIONADDRESS, then exercises the echo path (CMD_WRITE followed by
 * CMD_READ, which must come back with src/dst swapped per the proposal's
 * echo semantics). Emits results over serial via exec/RawPutChar -- the ROM
 * debug path, which needs no serial.device handler, no Mount, and no
 * Workbench files, so it works in the most minimal boot. Copperline's
 * `--serial stdout` captures it; see run.sh for the pass/fail check.
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
#include <proto/exec.h>

struct ExecBase *SysBase;

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
static char MSG_ONLINE_ERR[]   = "ONLINE_ERR=";
static char MSG_DQ_ERR[]       = "DQ_ERR=";
static char MSG_MTU[]          = "MTU=";
static char MSG_HWTYPE[]       = "HWTYPE=";
static char MSG_ADDR_ERR[]     = "ADDR_ERR=";
static char MSG_ADDR[]         = "ADDR=";
static char MSG_WRITE_ERR[]    = "WRITE_ERR=";
static char MSG_READ_ERR[]     = "READ_ERR=";
static char MSG_ECHO_ADDR[]    = "ECHO_ADDR=";
static char MSG_ECHO_DATA[]    = "ECHO_DATA=";
static char MSG_OK[]           = "OK";
static char MSG_MISMATCH[]     = "MISMATCH";
static char MSG_CRLF[]         = "\r\n";
static char MSG_RESULT_PASS[]  = "RESULT=PASS\r\n";
static char MSG_RESULT_FAIL[]  = "RESULT=FAIL\r\n";
static char MSG_END[]          = "END\r\n";
static char HEX_DIGITS[]       = "0123456789abcdef";
static char PAYLOAD[]          = "sana2loop-echo-test";
static char DEVICE_NAME[]      = "loopback.device";

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

int __attribute__((no_reorder)) _start(void)
{
    return test_main();
}

static int test_main(void)
{
    struct MsgPort *port;
    struct IOSana2Req *io;
    static UBYTE readbuf[64];
    int fails = 0;
    BYTE sigbit;

    SysBase = *(struct ExecBase **)4UL;

    raw_str(MSG_BEGIN);

    port = AllocMem(sizeof(struct MsgPort), MEMF_PUBLIC | MEMF_CLEAR);
    if (!port) {
        raw_str(MSG_FAIL_PORT);
        raw_str(MSG_END);
        return 1;
    }
    sigbit = AllocSignal(-1);
    if (sigbit == -1) {
        raw_str(MSG_FAIL_SIGNAL);
        FreeMem(port, sizeof(struct MsgPort));
        raw_str(MSG_END);
        return 1;
    }
    port->mp_Node.ln_Type = NT_MSGPORT;
    port->mp_Flags        = PA_SIGNAL;
    port->mp_SigBit       = sigbit;
    port->mp_SigTask      = FindTask(NULL);
    port->mp_MsgList.lh_Head     = (struct Node *)&port->mp_MsgList.lh_Tail;
    port->mp_MsgList.lh_Tail     = NULL;
    port->mp_MsgList.lh_TailPred = (struct Node *)&port->mp_MsgList.lh_Head;

    io = AllocMem(sizeof(struct IOSana2Req), MEMF_PUBLIC | MEMF_CLEAR);
    if (!io) {
        raw_str(MSG_FAIL_IOREQ);
        FreeSignal(sigbit);
        FreeMem(port, sizeof(struct MsgPort));
        raw_str(MSG_END);
        return 1;
    }
    io->ios2_Req.io_Message.mn_Node.ln_Type = NT_MESSAGE;
    io->ios2_Req.io_Message.mn_ReplyPort    = port;
    io->ios2_Req.io_Message.mn_Length       = sizeof(struct IOSana2Req);

    if (OpenDevice((STRPTR)DEVICE_NAME, 0, (struct IORequest *)io, 0)) {
        raw_str(MSG_OPEN_FAIL);
        FreeMem(io, sizeof(struct IOSana2Req));
        FreeSignal(sigbit);
        FreeMem(port, sizeof(struct MsgPort));
        raw_str(MSG_END);
        return 1;
    }
    raw_str(MSG_OPEN_OK);

    io->ios2_Req.io_Command = S2_ONLINE;
    DoIO((struct IORequest *)io);
    raw_str(MSG_ONLINE_ERR);
    raw_u16((unsigned short)(unsigned char)io->ios2_Req.io_Error, 1);
    raw_str(MSG_CRLF);
    if (io->ios2_Req.io_Error)
        fails++;

    {
        struct Sana2DeviceQuery dq;
        dq.SizeAvailable = sizeof(dq);
        io->ios2_Req.io_Command = S2_DEVICEQUERY;
        io->ios2_StatData = &dq;
        DoIO((struct IORequest *)io);
        raw_str(MSG_DQ_ERR);
        raw_u16((unsigned short)(unsigned char)io->ios2_Req.io_Error, 1);
        raw_str(MSG_CRLF);
        if (io->ios2_Req.io_Error) {
            fails++;
        } else {
            raw_str(MSG_MTU);
            raw_u16(dq.MTU, 1);
            raw_str(MSG_CRLF);
            raw_str(MSG_HWTYPE);
            raw_u16(dq.HardwareType, 1);
            raw_str(MSG_CRLF);
            if (dq.MTU != 1500 || dq.HardwareType != S2WireType_Ethernet)
                fails++;
        }
    }

    io->ios2_Req.io_Command = S2_GETSTATIONADDRESS;
    DoIO((struct IORequest *)io);
    raw_str(MSG_ADDR_ERR);
    raw_u16((unsigned short)(unsigned char)io->ios2_Req.io_Error, 1);
    raw_str(MSG_CRLF);
    if (io->ios2_Req.io_Error) {
        fails++;
    } else {
        int i;
        raw_str(MSG_ADDR);
        for (i = 0; i < 6; i++)
            raw_hex_byte(io->ios2_SrcAddr[i]);
        raw_str(MSG_CRLF);
    }

    /* Echo test: write a packet to a made-up destination, then read it back.
     * With no CMD_READ queued yet, the write queues the packet; the
     * subsequent read must find it immediately (quick completion) with
     * src/dst swapped, per the device's echo semantics. */
    {
        int i;
        io->ios2_Req.io_Command = CMD_WRITE;
        for (i = 0; i < SANA2_MAX_ADDR_BYTES; i++)
            io->ios2_DstAddr[i] = (i < 6) ? (UBYTE)(0xAA + i) : 0;
        io->ios2_PacketType = 0x1234;
        io->ios2_DataLength = sizeof(PAYLOAD) - 1;
        io->ios2_Data = (APTR)PAYLOAD;
        DoIO((struct IORequest *)io);
        raw_str(MSG_WRITE_ERR);
        raw_u16((unsigned short)(unsigned char)io->ios2_Req.io_Error, 1);
        raw_str(MSG_CRLF);
        if (io->ios2_Req.io_Error)
            fails++;
    }

    {
        int i;
        BOOL addr_ok = TRUE, data_ok = TRUE;

        io->ios2_Req.io_Command = CMD_READ;
        io->ios2_Data = readbuf;
        DoIO((struct IORequest *)io);
        raw_str(MSG_READ_ERR);
        raw_u16((unsigned short)(unsigned char)io->ios2_Req.io_Error, 1);
        raw_str(MSG_CRLF);

        if (io->ios2_Req.io_Error) {
            fails++;
        } else {
            /* Echoed src must equal what we wrote as dst. */
            for (i = 0; i < 6; i++)
                if (io->ios2_SrcAddr[i] != (UBYTE)(0xAA + i))
                    addr_ok = FALSE;
            if (io->ios2_DataLength != sizeof(PAYLOAD) - 1)
                data_ok = FALSE;
            else
                for (i = 0; i < (int)io->ios2_DataLength; i++)
                    if (readbuf[i] != (UBYTE)PAYLOAD[i])
                        data_ok = FALSE;

            raw_str(MSG_ECHO_ADDR);
            raw_str(addr_ok ? MSG_OK : MSG_MISMATCH);
            raw_str(MSG_CRLF);
            raw_str(MSG_ECHO_DATA);
            raw_str(data_ok ? MSG_OK : MSG_MISMATCH);
            raw_str(MSG_CRLF);
            if (!addr_ok || !data_ok)
                fails++;
        }
    }

    CloseDevice((struct IORequest *)io);
    FreeMem(io, sizeof(struct IOSana2Req));
    FreeSignal(sigbit);
    FreeMem(port, sizeof(struct MsgPort));

    raw_str(fails ? MSG_RESULT_FAIL : MSG_RESULT_PASS);
    raw_str(MSG_END);
    return fails ? 1 : 0;
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
