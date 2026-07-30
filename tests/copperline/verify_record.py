#!/usr/bin/env python3
"""verify_record.py -- M9 record-flush check.

Parses the pcap file loopback.device's `record=` unit 11 wrote to the ADF
(extracted by run.sh via xdftool, since checking what ended up on the
host-side image is inherently a host-side question the on-target harness
can't answer about itself) and byte-compares it against the 3 known writes
sana2test.c's own "record" sub-test makes.

Also prints (not asserts -- docs/PLAN.md's M9 entry: "68000 per-packet
overhead measurement ... tracked not gated") the VBlank-tick delta between
the first and last recorded timestamp, as a rough measure of how much
virtual time elapsed while unit 11 made its 3 writes.

Usage: verify_record.py <path-to-extracted-recorded.pcap>
Exit 0 and prints "RECORD_VERIFY=PASS" on success; non-zero and a message
on stderr otherwise.
"""
import struct
import sys

EXPECT = [
    (0x1000, bytes.fromhex("0253324c000b"), bytes.fromhex("aabbccddee01"), b"RECORD-ONE"),
    (0x1001, bytes.fromhex("0253324c000b"), bytes.fromhex("aabbccddee01"), b"RECORD-TWO"),
    (0x1002, bytes.fromhex("0253324c000b"), bytes.fromhex("aabbccddee01"), b"RECORD-THREE"),
]


def fail(msg):
    print(f"FAIL: {msg}", file=sys.stderr)
    sys.exit(1)


def main():
    if len(sys.argv) != 2:
        fail("usage: verify_record.py <path>")
    with open(sys.argv[1], "rb") as f:
        data = f.read()

    if len(data) < 24:
        fail(f"file too short ({len(data)} bytes)")

    magic = data[0:4]
    if magic == b"\xa1\xb2\xc3\xd4":
        endian = ">"
    elif magic == b"\xd4\xc3\xb2\xa1":
        endian = "<"
    else:
        fail(f"bad magic {magic.hex()}")

    linktype = struct.unpack(endian + "I", data[20:24])[0]
    if linktype != 1:
        fail(f"linktype={linktype}, expected 1 (Ethernet)")

    off = 24
    records = []
    while off < len(data):
        if off + 16 > len(data):
            fail("truncated record header")
        ts_sec, ts_usec, incl_len, orig_len = struct.unpack(endian + "IIII", data[off:off + 16])
        off += 16
        if incl_len != orig_len:
            fail(f"record snaplen-truncated ({incl_len} != {orig_len})")
        if off + incl_len > len(data):
            fail("truncated record body")
        frame = data[off:off + incl_len]
        off += incl_len
        records.append((ts_sec, ts_usec, frame))

    if len(records) != len(EXPECT):
        fail(f"got {len(records)} records, expected {len(EXPECT)}")

    for i, ((ts_sec, ts_usec, frame), (exp_type, exp_dst, exp_src, exp_payload)) in enumerate(zip(records, EXPECT)):
        if ts_usec != 0:
            fail(f"record {i}: ts_usec={ts_usec}, expected 0 (whole-tick granularity)")
        dst, src, etype = frame[0:6], frame[6:12], struct.unpack(">H", frame[12:14])[0]
        payload = frame[14:]
        if dst != exp_dst:
            fail(f"record {i}: dst={dst.hex()}, expected {exp_dst.hex()}")
        if src != exp_src:
            fail(f"record {i}: src={src.hex()}, expected {exp_src.hex()}")
        if etype != exp_type:
            fail(f"record {i}: type=0x{etype:04x}, expected 0x{exp_type:04x}")
        if payload != exp_payload:
            fail(f"record {i}: payload={payload!r}, expected {exp_payload!r}")

    ticks = records[-1][0] - records[0][0]
    print(f"RECORD_OVERHEAD_TICKS={ticks}")  # informational only -- not gated
    print("RECORD_VERIFY=PASS")


if __name__ == "__main__":
    main()
