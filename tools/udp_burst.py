#!/usr/bin/env python3
"""UDP burst driver for udp_echo_server_ee.

Sends N UDP packets sequentially, expecting each reply before sending the next.
Reports per-iter latency, success/fail counts, and the iter at which the first
failure occurs (if any) — that's the "wedge point" for the EE-side hang.

Usage:
    udp_burst.py [HOST] [PORT] [N] [PER_TIMEOUT_S]

Defaults: 127.0.0.1 7777 100 2.0
"""
import socket
import sys
import time

HOST = sys.argv[1] if len(sys.argv) > 1 else '127.0.0.1'
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 7777
N = int(sys.argv[3]) if len(sys.argv) > 3 else 100
TIMEOUT = float(sys.argv[4]) if len(sys.argv) > 4 else 2.0

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.settimeout(TIMEOUT)

ok = 0
fail = 0
first_fail = None
total_t = 0.0
worst_t = 0.0
t0 = time.time()

for i in range(1, N + 1):
    msg = f"ping #{i}".encode()
    t_start = time.time()
    try:
        s.sendto(msg, (HOST, PORT))
        data, addr = s.recvfrom(1024)
        dt = time.time() - t_start
        total_t += dt
        worst_t = max(worst_t, dt)
        if data == msg:
            ok += 1
            if i <= 3 or i % 25 == 0:
                print(f"  iter {i}: ok ({dt*1000:.1f} ms)")
        else:
            fail += 1
            if first_fail is None:
                first_fail = i
            print(f"  iter {i}: mismatch — sent {msg!r}, got {data!r}")
    except socket.timeout:
        fail += 1
        if first_fail is None:
            first_fail = i
        if fail <= 5 or fail % 20 == 0:
            print(f"  iter {i}: TIMEOUT after {TIMEOUT:.1f}s")
    except Exception as e:
        fail += 1
        if first_fail is None:
            first_fail = i
        print(f"  iter {i}: error {e}")

elapsed = time.time() - t0
avg_ms = (total_t / ok * 1000) if ok else 0.0
print(f"\n{N}-burst to {HOST}:{PORT}")
print(f"  ok={ok}/{N} fail={fail} first_fail={first_fail or 'none'}")
print(f"  elapsed={elapsed:.2f}s  avg_rtt={avg_ms:.1f}ms  worst_rtt={worst_t*1000:.1f}ms")

sys.exit(0 if fail == 0 else 1)
