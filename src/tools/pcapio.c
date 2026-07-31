/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Simon Dick
 */

#include "pcapio.h"

#include <proto/dos.h>

BOOL pcapio_open_read(struct PcapReader *r, const char *path)
{
    UBYTE filehdr[PCAP_FILE_HEADER_SIZE];
    ULONG magic_raw;

    r->fh = Open((STRPTR)path, MODE_OLDFILE);
    if (!r->fh)
        return FALSE;

    if (Read(r->fh, filehdr, PCAP_FILE_HEADER_SIZE) != PCAP_FILE_HEADER_SIZE) {
        Close(r->fh);
        r->fh = 0;
        return FALSE;
    }

    /* Endianness is a property of THIS FILE, decided once from its magic
     * -- see pcap.h's own comment. Read with swapped=FALSE just to get
     * the raw 32 bits out; the comparison below is byte-pattern-vs-byte-
     * pattern, not numeric, so it doesn't matter which order that read
     * assembled them in as long as it's used consistently here. */
    magic_raw = pcap_read_u32(filehdr, FALSE);
    if (magic_raw == PCAP_MAGIC_NATIVE) {
        r->swapped = FALSE;
    } else if (magic_raw == PCAP_MAGIC_SWAPPED) {
        r->swapped = TRUE;
    } else {
        Close(r->fh);
        r->fh = 0;
        return FALSE;
    }

    pcap_parse_file_header(filehdr, r->swapped, &r->hdr);
    if (r->hdr.linktype != PCAP_LINKTYPE_ETHERNET) {
        Close(r->fh);
        r->fh = 0;
        return FALSE;
    }

    return TRUE;
}

BOOL pcapio_read_packet(struct PcapReader *r, struct pcap_pkthdr *hdr,
                        UBYTE *data, ULONG bufsize, BOOL *eof)
{
    UBYTE rechdr[PCAP_PKTHDR_SIZE];
    LONG got;

    *eof = FALSE;

    got = Read(r->fh, rechdr, PCAP_PKTHDR_SIZE);
    if (got == 0) {
        *eof = TRUE;
        return FALSE; /* clean end-of-file between records -- done */
    }
    if (got != PCAP_PKTHDR_SIZE)
        return FALSE; /* truncated header -- a real error, not EOF */

    pcap_parse_pkthdr(rechdr, r->swapped, hdr);

    /* incl_len != orig_len means the WRITER's own snaplen truncated this
     * record -- this project rejects that outright rather than silently
     * replaying/forwarding a partial frame (same stance load_replay_file()
     * takes in the device itself). */
    if (hdr->incl_len != hdr->orig_len)
        return FALSE;
    if (hdr->incl_len > bufsize)
        return FALSE; /* caller's buffer too small -- hard error, no
                          silent truncation (see this function's own
                          header comment) */

    if (Read(r->fh, data, (LONG)hdr->incl_len) != (LONG)hdr->incl_len)
        return FALSE;

    return TRUE;
}

void pcapio_close_read(struct PcapReader *r)
{
    if (r->fh) {
        Close(r->fh);
        r->fh = 0;
    }
}

BOOL pcapio_open_write(struct PcapWriter *w, const char *path,
                       ULONG snaplen, ULONG linktype)
{
    UBYTE filehdr[PCAP_FILE_HEADER_SIZE];

    w->fh = Open((STRPTR)path, MODE_NEWFILE);
    if (!w->fh)
        return FALSE;

    pcap_build_file_header(filehdr, snaplen, linktype);
    if (Write(w->fh, filehdr, PCAP_FILE_HEADER_SIZE) != PCAP_FILE_HEADER_SIZE) {
        Close(w->fh);
        w->fh = 0;
        return FALSE;
    }
    return TRUE;
}

BOOL pcapio_write_packet(struct PcapWriter *w, ULONG ts_sec, ULONG ts_usec,
                         const UBYTE *data, ULONG len)
{
    UBYTE rechdr[PCAP_PKTHDR_SIZE];

    pcap_build_pkthdr(rechdr, ts_sec, ts_usec, len, len);
    if (Write(w->fh, rechdr, PCAP_PKTHDR_SIZE) != PCAP_PKTHDR_SIZE)
        return FALSE;
    if (len > 0 && Write(w->fh, (APTR)data, (LONG)len) != (LONG)len)
        return FALSE;
    return TRUE;
}

void pcapio_close_write(struct PcapWriter *w)
{
    if (w->fh) {
        Close(w->fh);
        w->fh = 0;
    }
}
