#!/usr/bin/env python3
"""verify_sanadump.py -- M11 SanaSend/SanaDump on-target round-trip check.

run.sh's Startup-Sequence runs, on unit 12 (rxqueue=5, echo mode, config
fixture unit12.config):
    SanaSend 12 S:sana2loop/arp.pcap
    SanaDump 12 S:sana2loop/captured.pcap COUNT=2

This parses the extracted captured.pcap (via xdftool, since checking what
ended up on the host-side image is inherently a host-side question the
guest can't answer about itself -- same reasoning as verify_record.py)
and checks it against the ORIGINAL fixtures/arp.pcap SanaSend was given,
accounting for echo mode's own known address transformation (see
userdocs/Echo-and-Crossover.md): a plain CMD_WRITE on an echo unit swaps
addresses so the reader sees SRC = whatever was written as DST, and
DST = the unit's own (factory, since nothing here ever configures it
away) station address -- unit 12's is 02:53:32:4c:00:0c (see
reset_unit()'s own comment in src/loopback_device.c for the factory
address scheme). Packet type and payload must round-trip byte-for-byte
unchanged; SanaSend never touches either.

Usage: verify_sanadump.py <captured.pcap> <original-fixture.pcap>
Exit 0 and prints "SANADUMP_VERIFY=PASS" on success; non-zero and a
message on stderr otherwise.
"""
import struct
import sys

UNIT12_STATION = bytes.fromhex("0253324c000c")


def fail(msg):
    print(f"FAIL: {msg}", file=sys.stderr)
    sys.exit(1)


def read_pcap(path):
    with open(path, "rb") as f:
        data = f.read()

    if len(data) < 24:
        fail(f"{path}: file too short ({len(data)} bytes)")

    magic = data[0:4]
    if magic == b"\xa1\xb2\xc3\xd4":
        endian = ">"
    elif magic == b"\xd4\xc3\xb2\xa1":
        endian = "<"
    else:
        fail(f"{path}: bad magic {magic.hex()}")

    off = 24
    records = []
    while off < len(data):
        if off + 16 > len(data):
            fail(f"{path}: truncated record header")
        ts_sec, ts_usec, incl_len, orig_len = struct.unpack(endian + "IIII", data[off : off + 16])
        off += 16
        if incl_len != orig_len:
            fail(f"{path}: record snaplen-truncated ({incl_len} != {orig_len})")
        if off + incl_len > len(data):
            fail(f"{path}: truncated record body")
        records.append(data[off : off + incl_len])
        off += incl_len
    return records


def main():
    if len(sys.argv) != 3:
        fail("usage: verify_sanadump.py <captured.pcap> <original-fixture.pcap>")

    captured = read_pcap(sys.argv[1])
    original = read_pcap(sys.argv[2])

    if len(captured) != len(original):
        fail(f"captured {len(captured)} record(s), expected {len(original)} (from {sys.argv[2]})")

    for i, (cap, orig) in enumerate(zip(captured, original)):
        if len(cap) < 14:
            fail(f"record {i}: captured frame ({len(cap)} bytes) shorter than an Ethernet header")
        if len(orig) < 14:
            fail(f"record {i}: original fixture frame ({len(orig)} bytes) shorter than an Ethernet header")

        cap_dst, cap_src, cap_type, cap_payload = cap[0:6], cap[6:12], cap[12:14], cap[14:]
        orig_dst, orig_type, orig_payload = orig[0:6], orig[12:14], orig[14:]

        if cap_dst != UNIT12_STATION:
            fail(f"record {i}: dst={cap_dst.hex()}, expected unit 12's own station {UNIT12_STATION.hex()}")
        if cap_src != orig_dst:
            fail(f"record {i}: src={cap_src.hex()}, expected original dst {orig_dst.hex()} (echo-mode swap)")
        if cap_type != orig_type:
            fail(f"record {i}: type={cap_type.hex()}, expected {orig_type.hex()}")
        if cap_payload != orig_payload:
            fail(f"record {i}: payload mismatch -- SanaSend/SanaDump must round-trip it unchanged")

    print("SANADUMP_VERIFY=PASS")


if __name__ == "__main__":
    main()
