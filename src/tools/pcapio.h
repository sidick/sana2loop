/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Simon Dick
 */

/* pcapio.h -- dos.library-based pcap file I/O for the host/tool side
 * (SanaDump, SanaSend). Builds directly on src/pcap.h's parse/build
 * helpers (which deliberately do no I/O of their own -- see that header's
 * comment) rather than duplicating any byte-order or record-shape logic;
 * this file's only job is the actual Open()/Read()/Write()/Close()
 * mechanics around them.
 *
 * Deliberately dos.library (BPTR/Open/Read/Write/Close), NOT ISO C stdio
 * (fopen/fread/fwrite) despite this being normal AmigaOS 2.0+/clib2 code
 * that could use either: empirically, under Copperline's minimal
 * Startup-Sequence-launched boot environment (no Workbench, the same
 * environment this project's own on-target tests run these tools under),
 * clib2's fopen() reliably FAILS to open a file that plain dos.library
 * Open() opens without issue, for reasons that didn't repay further
 * digging once the fix was this simple -- matching the same dos.library-
 * direct approach loopback_device.c's own load_replay_file()/
 * read_unit_config() already use, for a different reason (freestanding,
 * no libc at all) but the identical mechanism. Real Amiga Shell/Workbench
 * environments may well not hit this, but there's no upside to using
 * stdio here when the dos.library equivalent is a handful of lines and
 * already proven reliable everywhere this project has tested it.
 */

#ifndef SANA2LOOP_PCAPIO_H
#define SANA2LOOP_PCAPIO_H

#include <exec/types.h>
#include <dos/dos.h>
#include "../pcap.h"

struct PcapReader {
    BPTR fh;
    BOOL swapped;              /* this FILE's own byte order -- see pcap.h */
    struct pcap_file_header hdr;
};

struct PcapWriter {
    BPTR fh;
};

/* Opens `path` for reading, validates the magic (either byte order
 * accepted) and linktype (must be PCAP_LINKTYPE_ETHERNET -- the only wire
 * format any of this project's tools understand), and leaves `r` ready for
 * repeated pcapio_read_packet() calls. FALSE on any failure (file missing,
 * bad magic, wrong linktype) -- `r->fh` is guaranteed closed either way, so
 * the caller never needs its own cleanup on a failed open. */
BOOL pcapio_open_read(struct PcapReader *r, const char *path);

/* Reads one record into `*hdr` (already byte-order-corrected) and `data`
 * (up to `bufsize` bytes -- a record whose incl_len exceeds that is a hard
 * error, not a silent truncation: silently truncating a real capture is
 * exactly the wrong default for a tool people rely on to reproduce a
 * capture's payload byte for byte). Returns FALSE at clean end-of-file
 * (with `*eof` set TRUE) or on any real error (`*eof` set FALSE) --
 * callers must check `*eof` to tell "done" from "something is wrong". */
BOOL pcapio_read_packet(struct PcapReader *r, struct pcap_pkthdr *hdr,
                        UBYTE *data, ULONG bufsize, BOOL *eof);

void pcapio_close_read(struct PcapReader *r);

/* Opens `path` for writing and immediately writes the file header
 * (native byte order -- see pcap.h's own comment on why that's fully
 * interoperable, not a limitation). FALSE if the file can't be created. */
BOOL pcapio_open_write(struct PcapWriter *w, const char *path,
                       ULONG snaplen, ULONG linktype);

/* Appends one record (`len` bytes of `data`, with `incl_len == orig_len`
 * always -- this project never writes a snaplen-truncated record). */
BOOL pcapio_write_packet(struct PcapWriter *w, ULONG ts_sec, ULONG ts_usec,
                         const UBYTE *data, ULONG len);

/* Closes the write handle. FALSE if dos.library's Close() itself fails --
 * a filesystem may defer buffered writes until close, making this the
 * last place a write error can surface, so callers must treat FALSE as a
 * real write failure, not a cleanup nicety. (The read-side close stays
 * void: a read handle has no deferred writes for Close() to fail on.) */
BOOL pcapio_close_write(struct PcapWriter *w);

#endif /* SANA2LOOP_PCAPIO_H */
