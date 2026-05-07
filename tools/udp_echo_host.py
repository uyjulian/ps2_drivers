#!/usr/bin/env python3
"""Trivial UDP echo server for the EE-side reproducer.

Listens on 0.0.0.0:7777 and echoes any received packet back to the sender,
also printing source IP and a sequential count.

Usage:
    udp_echo_host.py [PORT]

Defaults: 7777.
"""
import socket
import sys
import time

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 7777

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(('0.0.0.0', PORT))
print(f"udp_echo_host listening on :{PORT}", flush=True)

n = 0
try:
    while True:
        data, addr = s.recvfrom(1024)
        n += 1
        print(f"#{n} from {addr[0]}:{addr[1]} ({len(data)} bytes) {data!r}", flush=True)
        s.sendto(data, addr)
except KeyboardInterrupt:
    print(f"\nstopping after {n} packets")
