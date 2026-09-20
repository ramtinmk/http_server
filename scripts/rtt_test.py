#!/usr/bin/env python3
"""Measure serial keep-alive RTT against the running server."""
import socket, time, sys

def rtt_test(name, req, n=300):
    s = socket.create_connection(("127.0.0.1", 8081), timeout=5)
    s.settimeout(5)
    t0 = time.monotonic()
    lat = []
    try:
        for i in range(n):
            t1 = time.monotonic()
            s.sendall(req)
            buf = b""
            while b"\r\n\r\n" not in buf:
                chunk = s.recv(65536)
                if not chunk:
                    raise ConnectionError(f"EOF at iter {i}, buffered {buf[:200]!r}")
                buf += chunk
            head, _, rest = buf.partition(b"\r\n\r\n")
            clen = 0
            for line in head.split(b"\r\n"):
                if line.lower().startswith(b"content-length:"):
                    clen = int(line.split(b":")[1].strip())
            while len(rest) < clen:
                chunk = s.recv(65536)
                if not chunk:
                    raise ConnectionError(f"EOF during body at iter {i}")
                rest += chunk
            lat.append((time.monotonic() - t1) * 1000.0)
            if i == 0:
                print(f"  first response OK after {lat[-1]:.3f}ms")
    except Exception as e:
        print(f"{name}: FAILED after {len(lat)} reqs: {e}")
        s.close()
        return
    s.close()
    t_total = time.monotonic() - t0
    lat.sort()
    print(f"{name}: {n} serial reqs in {t_total:.3f}s = {n/t_total:.0f} rps | "
          f"p50={lat[len(lat)//2]:.3f}ms p95={lat[int(len(lat)*0.95)]:.3f}ms "
          f"max={lat[-1]:.3f}ms")

REQ_KA = b"GET /home HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n"
REQ_CLOSE = b"GET /home HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"

rtt_test("keep-alive serial", REQ_KA)
rtt_test("close serial", REQ_CLOSE)