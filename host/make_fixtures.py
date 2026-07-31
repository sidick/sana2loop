#!/usr/bin/env python3
"""make_fixtures.py -- build sana2loop's canonical .pcap fixture corpus.

Generates a small, well-known set of captures (ARP request/reply, a DHCP
DISCOVER, a TCP handshake) into fixtures/ using scapy -- real, valid
Ethernet frames a real network stack would actually produce, not
hand-crafted byte arrays. These are meant as example/reference traffic for
SanaSend (replay real-shaped packets through a loopback.device unit) and
as input to host/lint_pcap.py -- NOT the same as tests/copperline/fixtures/,
which are on-target-test-specific captures with fixed byte content the
test harness asserts against exactly.

Requires scapy (`pip install scapy`); not part of the cross-build or the
on-target test suite, so it has no place in Makefile/CI -- run by hand
when the corpus needs regenerating.

Usage: host/make_fixtures.py [output-dir]   (default: fixtures/)
"""
import os
import sys

from scapy.all import (
    ARP,
    BOOTP,
    DHCP,
    IP,
    TCP,
    UDP,
    Ether,
    wrpcap,
)

MAC_A = "aa:bb:cc:dd:ee:01"
MAC_B = "aa:bb:cc:dd:ee:02"
IP_A = "192.168.1.1"
IP_B = "192.168.1.2"


def arp_exchange():
    """ARP request (who-has B, tell A) then reply (B is-at MAC_B)."""
    req = Ether(dst="ff:ff:ff:ff:ff:ff", src=MAC_A) / ARP(
        op=1, hwsrc=MAC_A, psrc=IP_A, hwdst="00:00:00:00:00:00", pdst=IP_B
    )
    reply = Ether(dst=MAC_A, src=MAC_B) / ARP(
        op=2, hwsrc=MAC_B, psrc=IP_B, hwdst=MAC_A, pdst=IP_A
    )
    return [req, reply]


def dhcp_discover():
    """A single DHCPDISCOVER broadcast, client MAC_A."""
    pkt = (
        Ether(dst="ff:ff:ff:ff:ff:ff", src=MAC_A)
        / IP(src="0.0.0.0", dst="255.255.255.255")
        / UDP(sport=68, dport=67)
        / BOOTP(chaddr=bytes.fromhex(MAC_A.replace(":", "")), xid=0x12345678)
        / DHCP(options=[("message-type", "discover"), "end"])
    )
    return [pkt]


def tcp_handshake():
    """SYN / SYN-ACK / ACK between A (client) and B (server), port 80."""
    syn = (
        Ether(dst=MAC_B, src=MAC_A)
        / IP(src=IP_A, dst=IP_B)
        / TCP(sport=40000, dport=80, flags="S", seq=1000)
    )
    synack = (
        Ether(dst=MAC_A, src=MAC_B)
        / IP(src=IP_B, dst=IP_A)
        / TCP(sport=80, dport=40000, flags="SA", seq=2000, ack=1001)
    )
    ack = (
        Ether(dst=MAC_B, src=MAC_A)
        / IP(src=IP_A, dst=IP_B)
        / TCP(sport=40000, dport=80, flags="A", seq=1001, ack=2001)
    )
    return [syn, synack, ack]


FIXTURES = {
    "arp.pcap": arp_exchange,
    "dhcp-discover.pcap": dhcp_discover,
    "tcp-handshake.pcap": tcp_handshake,
}


def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else "fixtures"
    os.makedirs(outdir, exist_ok=True)
    for name, builder in FIXTURES.items():
        path = os.path.join(outdir, name)
        pkts = builder()
        wrpcap(path, pkts)
        print("%s: %d packet%s" % (path, len(pkts), "" if len(pkts) == 1 else "s"))


if __name__ == "__main__":
    main()
