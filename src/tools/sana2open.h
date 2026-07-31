/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Simon Dick
 */

/* sana2open.h -- shared "open a SANA-II unit properly" helper for
 * SanaDump/SanaSend.
 *
 * Deliberately more than the bare minimum OpenDevice() call SanaInfo gets
 * away with (SanaInfo never reads or writes packet data, so it never
 * touches ios2_BufferManagement at all): per docs/PLAN.md's own design
 * decision, this is meant to be a "model citizen" -- it installs real,
 * explicit S2_CopyToBuff/S2_CopyFromBuff hooks via a proper tag list,
 * the way a real network stack is expected to, rather than leaving
 * ios2_BufferManagement NULL and relying on loopback.device's own
 * NULL-tags default (a plain internal CopyMem -- see
 * apply_buffer_mgmt_tags() in loopback_device.c). Functionally the two
 * are equivalent against THIS device today, but a tool meant to double as
 * a usage example shouldn't teach "just leave it NULL" as the pattern.
 */

#ifndef SANA2LOOP_SANA2OPEN_H
#define SANA2LOOP_SANA2OPEN_H

#include <exec/types.h>
#include <exec/ports.h>
#include <exec/io.h>
#include <utility/tagitem.h>
#include <devices/sana2.h>

struct Sana2Open {
    struct MsgPort    *port;
    struct IOSana2Req *io;
    struct TagItem     hook_tags[3]; /* owned here, not static/shared, so
                                         more than one Sana2Open can be
                                         open at once without one's hooks
                                         clobbering another's */
};

/* Zeroes `so`, then CreateMsgPort()+CreateIORequest()+OpenDevice()s
 * `device` unit `unit` with `flags` (e.g. SANA2OPF_PROM), with real
 * buffer-management hooks installed. FALSE on any failure -- `so`'s
 * fields are left in a state safe to pass to sana2_close() either way,
 * so callers never need their own partial-failure bookkeeping. */
BOOL sana2_open(struct Sana2Open *so, const char *device, LONG unit, ULONG flags);

/* CloseDevice()/DeleteIORequest()/DeleteMsgPort(), each only if actually
 * allocated -- safe to call on a `so` that sana2_open() only partially
 * populated before failing, or that's already been closed. */
void sana2_close(struct Sana2Open *so);

#endif /* SANA2LOOP_SANA2OPEN_H */
