#!/usr/bin/env python3
"""Trivial TCP echo server for tcp_burst_client_ee.

Accepts a connection, echoes received bytes back, closes. Repeats.

Usage:
    tcp_echo_host.py [PORT]

Defaults: 7777.
"""
import socket
import sys

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 7777

s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(('0.0.0.0', PORT))
s.listen(64)
print(f"tcp_echo_host listening on :{PORT}", flush=True)

n = 0
try:
    while True:
        c, addr = s.accept()
        n += 1
        try:
            data = c.recv(1024)
            if data:
                c.sendall(data)
                print(f"#{n} from {addr[0]}:{addr[1]} ({len(data)} bytes) {data!r}", flush=True)
        finally:
            c.close()
except KeyboardInterrupt:
    print(f"\nstopping after {n} connections")
