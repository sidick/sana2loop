#!/usr/bin/env python3
"""lint_pcap.py -- structural sanity checks for a classic-format .pcap file.

General-purpose (not tied to any one fixture's expected content, unlike
tests/copperline/verify_record.py's exact byte-compare): checks the things
loopback.device itself would reject a capture for (see
load_replay_file()'s comment in src/loopback_device.c and
userdocs/Replay-and-Record.md), so a fixture that passes this is one the
device itself can actually load.

Checks:
  - valid magic (either byte order), linktype == Ethernet
  - every record's incl_len == orig_len (this project rejects
    snaplen-truncated records outright, never silently replays a partial
    frame)
  - every record is at least 14 bytes (a real Ethernet header)
  - timestamps are non-decreasing record to record (this project's
    scheduled-replay mode computes each record's due tick as ts_sec minus
    the FIRST record's ts_sec -- a safe subtraction only if the file is
    already in non-decreasing time order)

Usage: lint_pcap.py <file.pcap> [file2.pcap ...]
Exit 0 if every file passes; non-zero (with a message per problem) otherwise.
"""
import struct
import sys

ETH_HDR_SIZE = 14


def lint(path):
    problems = []
    with open(path, "rb") as f:
        data = f.read()

    if len(data) < 24:
        return [f"{path}: file too short ({len(data)} bytes) for even a file header"]

    magic = data[0:4]
    if magic == b"\xa1\xb2\xc3\xd4":
        endian = ">"
    elif magic == b"\xd4\xc3\xb2\xa1":
        endian = "<"
    else:
        return [f"{path}: bad magic {magic.hex()}"]

    linktype = struct.unpack(endian + "I", data[20:24])[0]
    if linktype != 1:
        problems.append(f"{path}: linktype={linktype}, expected 1 (Ethernet)")

    off = 24
    prev_sec = None
    count = 0
    while off < len(data):
        if off + 16 > len(data):
            problems.append(f"{path}: record {count}: truncated header")
            break
        ts_sec, _ts_usec, incl_len, orig_len = struct.unpack(
            endian + "IIII", data[off : off + 16]
        )
        off += 16
        if incl_len != orig_len:
            problems.append(
                f"{path}: record {count}: snaplen-truncated "
                f"(incl_len={incl_len} != orig_len={orig_len})"
            )
        if off + incl_len > len(data):
            problems.append(f"{path}: record {count}: truncated body")
            break
        if incl_len < ETH_HDR_SIZE:
            problems.append(
                f"{path}: record {count}: {incl_len} bytes, "
                f"shorter than an Ethernet header ({ETH_HDR_SIZE})"
            )
        if prev_sec is not None and ts_sec < prev_sec:
            problems.append(
                f"{path}: record {count}: ts_sec={ts_sec} decreases from "
                f"previous record's {prev_sec}"
            )
        prev_sec = ts_sec
        off += incl_len
        count += 1

    if count == 0:
        problems.append(f"{path}: no records")

    return problems


def main():
    if len(sys.argv) < 2:
        sys.exit("usage: lint_pcap.py <file.pcap> [file2.pcap ...]")

    all_problems = []
    for path in sys.argv[1:]:
        problems = lint(path)
        if problems:
            all_problems.extend(problems)
        else:
            print(f"{path}: OK")

    for p in all_problems:
        print(f"FAIL: {p}", file=sys.stderr)

    sys.exit(1 if all_problems else 0)


if __name__ == "__main__":
    main()
