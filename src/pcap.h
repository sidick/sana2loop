/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Simon Dick
 */

/* pcap.h -- hand-rolled classic pcap ("libpcap savefile") format pieces
 * this project actually touches. Shared between the freestanding device
 * (src/loopback_device.c, M8's consume-mode replay) and, later, host-side
 * companion tools (M11) -- no I/O in here at all, callers own Open/Read/
 * Write; this header only knows how to make sense of the bytes once
 * they're in memory.
 *
 * Both endians, deliberately: a capture written by a little-endian host
 * (scapy, tcpdump -- the overwhelmingly common case) needs to be readable
 * on this project's big-endian 68000 device without requiring the file to
 * be "fixed up" first. The pcap format itself supports this natively --
 * the file's own magic number identifies which byte order every OTHER
 * numeric field in the file uses, decided per-file, not by whichever CPU
 * happens to be reading it.
 *
 * Every struct below is an ALREADY-PARSED, in-memory record -- native
 * ULONG/UWORD/LONG, no further swapping needed once populated. Never cast
 * a raw file byte buffer directly to one of these struct types: that
 * would depend on this compiler's own struct padding/alignment rules
 * matching the on-disk layout exactly, which isn't guaranteed (and isn't
 * needed -- the pcap_parse_*() helpers below read every field explicitly,
 * byte by byte, through pcap_read_u16()/pcap_read_u32(), and are the only
 * sanctioned way to get bytes from a file into one of these). */

#ifndef SANA2LOOP_PCAP_H
#define SANA2LOOP_PCAP_H

#include <exec/types.h>

#define PCAP_MAGIC_NATIVE      0xa1b2c3d4UL /* file's numeric fields use
                                                the SAME byte order as
                                                whatever's reading it */
#define PCAP_MAGIC_SWAPPED     0xd4c3b2a1UL /* same magic, byte-reversed --
                                                every other numeric field
                                                in the file needs
                                                byte-swapping too */
#define PCAP_LINKTYPE_ETHERNET 1UL

#define PCAP_FILE_HEADER_SIZE 24 /* on-disk byte size -- deliberately not
                                     sizeof(struct pcap_file_header), which
                                     says nothing about file layout, only
                                     about this compiler's in-memory one */
#define PCAP_PKTHDR_SIZE      16

struct pcap_file_header {
    ULONG magic;
    UWORD version_major; /* 2 */
    UWORD version_minor; /* 4 */
    LONG  thiszone;      /* 0 -- GMT offset, unused by any writer/reader
                             this project cares about */
    ULONG sigfigs;       /* 0 -- accuracy of timestamps, likewise unused */
    ULONG snaplen;       /* max bytes captured per packet by the writer */
    ULONG linktype;      /* PCAP_LINKTYPE_ETHERNET, or this device rejects
                             the file */
};

struct pcap_pkthdr {
    ULONG ts_sec;
    ULONG ts_usec;
    ULONG incl_len; /* bytes actually captured/stored for this packet */
    ULONG orig_len; /* bytes on the real wire -- may exceed incl_len if the
                        writer's snaplen truncated it; this project rejects
                        that case rather than silently replaying a
                        truncated frame, see load_replay_file()'s own
                        comment in loopback_device.c */
};

/* Reads a big/little-endian ULONG/UWORD from a raw byte buffer. `swapped`
 * describes THIS FILE's own byte order (decided once, by comparing its
 * magic against PCAP_MAGIC_NATIVE/_SWAPPED above) -- not this CPU's own. A
 * big-endian 68000 reading a little-endian-written host capture needs
 * swapped=TRUE despite nothing about the 68000 itself changing. */
static ULONG pcap_read_u32(const UBYTE *p, BOOL swapped)
{
    if (swapped)
        return ((ULONG)p[0]) | ((ULONG)p[1] << 8) |
               ((ULONG)p[2] << 16) | ((ULONG)p[3] << 24);
    return ((ULONG)p[0] << 24) | ((ULONG)p[1] << 16) |
           ((ULONG)p[2] << 8) | (ULONG)p[3];
}

static UWORD pcap_read_u16(const UBYTE *p, BOOL swapped)
{
    if (swapped)
        return (UWORD)(((UWORD)p[0]) | ((UWORD)p[1] << 8));
    return (UWORD)(((UWORD)p[0] << 8) | (UWORD)p[1]);
}

/* Parses a PCAP_FILE_HEADER_SIZE-byte buffer into `out`, given `swapped`
 * (the caller's job to determine -- reading just the first 4 bytes and
 * comparing against PCAP_MAGIC_NATIVE/_SWAPPED is cheap and doesn't need
 * this function's help). */
static void pcap_parse_file_header(const UBYTE *buf, BOOL swapped, struct pcap_file_header *out)
{
    out->magic         = pcap_read_u32(buf + 0, swapped);
    out->version_major = pcap_read_u16(buf + 4, swapped);
    out->version_minor = pcap_read_u16(buf + 6, swapped);
    out->thiszone       = (LONG)pcap_read_u32(buf + 8, swapped);
    out->sigfigs       = pcap_read_u32(buf + 12, swapped);
    out->snaplen       = pcap_read_u32(buf + 16, swapped);
    out->linktype      = pcap_read_u32(buf + 20, swapped);
}

/* Parses a PCAP_PKTHDR_SIZE-byte buffer into `out`, same `swapped`
 * contract as pcap_parse_file_header() above (one file, one byte order,
 * decided once from the file header and reused for every record in it). */
static void pcap_parse_pkthdr(const UBYTE *buf, BOOL swapped, struct pcap_pkthdr *out)
{
    out->ts_sec   = pcap_read_u32(buf + 0, swapped);
    out->ts_usec  = pcap_read_u32(buf + 4, swapped);
    out->incl_len = pcap_read_u32(buf + 8, swapped);
    out->orig_len = pcap_read_u32(buf + 12, swapped);
}

#endif /* SANA2LOOP_PCAP_H */
