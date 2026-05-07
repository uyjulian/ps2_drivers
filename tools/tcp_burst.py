#!/usr/bin/env python3
"""TCP burst driver for tcp_server_ee.

Connects N times in sequence: socket -> connect -> send "ping #N" -> recv echo
-> close. Reports per-iter latency and where the first failure happens.

Usage:
    tcp_burst.py [HOST] [PORT] [N] [PER_TIMEOUT_S] [GAP_S]

Defaults: 192.168.31.131 6789 20 3.0 0.0
"""
import socket
import sys
import time

HOST = sys.argv[1] if len(sys.argv) > 1 else '192.168.31.131'
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 6789
N = int(sys.argv[3]) if len(sys.argv) > 3 else 20
TIMEOUT = float(sys.argv[4]) if len(sys.argv) > 4 else 3.0
GAP = float(sys.argv[5]) if len(sys.argv) > 5 else 0.0

ok = 0
fail = 0
first_fail = None
total_t = 0.0
t0 = time.time()

for i in range(1, N + 1):
    msg = f"ping #{i}".encode()
    t_start = time.time()
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(TIMEOUT)
    try:
        s.connect((HOST, PORT))
        s.sendall(msg)
        data = s.recv(1024)
        dt = time.time() - t_start
        total_t += dt
        if data == msg:
            ok += 1
            if i <= 3 or i % 10 == 0:
                print(f"  iter {i}: ok ({dt*1000:.1f} ms)")
        else:
            fail += 1
            if first_fail is None:
                first_fail = i
            print(f"  iter {i}: mismatch — sent {msg!r}, got {data!r}")
    except (socket.timeout, ConnectionRefusedError, OSError) as e:
        fail += 1
        if first_fail is None:
            first_fail = i
        if fail <= 5 or fail % 10 == 0:
            print(f"  iter {i}: {type(e).__name__}: {e}")
    finally:
        try: s.close()
        except: pass
    if GAP > 0: time.sleep(GAP)

elapsed = time.time() - t0
avg_ms = (total_t / ok * 1000) if ok else 0.0
print(f"\n{N}-burst to {HOST}:{PORT}")
print(f"  ok={ok}/{N} fail={fail} first_fail={first_fail or 'none'}")
print(f"  elapsed={elapsed:.2f}s  avg_rtt={avg_ms:.1f}ms")
sys.exit(0 if fail == 0 else 1)
