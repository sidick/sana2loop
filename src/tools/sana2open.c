/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Simon Dick
 */

#include "sana2open.h"

#include <proto/exec.h>

/* Real, interrupt-safe copy hooks: SANA-II requires buffer-management
 * hooks to be safe to call from interrupt context (a real driver may
 * complete a read from a hardware interrupt -- this device does exactly
 * that for scheduled-replay/record, see loopback_device.c's
 * vblank_server()), so a model-citizen consumer installs real ones
 * rather than relying on a device's own default. CopyMem() is Exec's own
 * function and is documented safe from interrupt context, so these are
 * genuinely correct, not just plausible-looking -- exactly the same
 * default-hook shape loopback.device installs internally
 * (apply_buffer_mgmt_tags()'s default_copy()), just supplied explicitly
 * here instead of left implicit. */
static BOOL copy_to_buff(APTR to asm("a0"), APTR from asm("a1"), ULONG size asm("d0"))
{
    CopyMem(from, to, size);
    return TRUE;
}

static BOOL copy_from_buff(APTR to asm("a0"), APTR from asm("a1"), ULONG size asm("d0"))
{
    CopyMem(from, to, size);
    return TRUE;
}

BOOL sana2_open(struct Sana2Open *so, const char *device, LONG unit, ULONG flags)
{
    so->port = NULL;
    so->io   = NULL;

    so->port = CreateMsgPort();
    if (!so->port)
        return FALSE;

    so->io = (struct IOSana2Req *)CreateIORequest(so->port, sizeof(struct IOSana2Req));
    if (!so->io) {
        DeleteMsgPort(so->port);
        so->port = NULL;
        return FALSE;
    }

    so->hook_tags[0].ti_Tag  = S2_CopyToBuff;
    so->hook_tags[0].ti_Data = (ULONG)copy_to_buff;
    so->hook_tags[1].ti_Tag  = S2_CopyFromBuff;
    so->hook_tags[1].ti_Data = (ULONG)copy_from_buff;
    so->hook_tags[2].ti_Tag  = TAG_DONE;
    so->hook_tags[2].ti_Data = 0;
    /* Per SANA-II convention: ios2_BufferManagement carries the CALLER's
     * tag list INTO OpenDevice(); the device replaces it with its own
     * per-open cookie afterward, which every subsequent request on this
     * same io must keep carrying unmodified (see loopback_device.c's own
     * comment on this). */
    so->io->ios2_BufferManagement = so->hook_tags;

    if (OpenDevice((STRPTR)device, unit, (struct IORequest *)so->io, flags)) {
        DeleteIORequest((struct IORequest *)so->io);
        DeleteMsgPort(so->port);
        so->io   = NULL;
        so->port = NULL;
        return FALSE;
    }
    return TRUE;
}

void sana2_close(struct Sana2Open *so)
{
    if (so->io) {
        CloseDevice((struct IORequest *)so->io);
        DeleteIORequest((struct IORequest *)so->io);
        so->io = NULL;
    }
    if (so->port) {
        DeleteMsgPort(so->port);
        so->port = NULL;
    }
}
