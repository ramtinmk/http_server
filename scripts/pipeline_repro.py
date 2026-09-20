#!/usr/bin/env python3
"""Byte-accurate pipeline test against the running event-loop server."""
import socket, sys, time

HOST, PORT = "127.0.0.1", 8081

class Conn:
    def __init__(self):
        self.s = socket.create_connection((HOST, PORT), timeout=3)
        self.buf = b""
    def sendall(self, data):
        self.s.sendall(data)
    def fill(self, want, deadline):
        while len(self.buf) < want:
            if time.time() > deadline:
                raise TimeoutError(f"stalled: buffered {len(self.buf)}, want {want}")
            chunk = self.s.recv(65536)
            if not chunk:
                raise ConnectionError(f"closed early: buffered {len(self.buf)}, want {want}")
            self.buf += chunk
    def read_response(self):
        deadline = time.time() + 3.0
        # Find end of headers
        idx = self.buf.find(b"\r\n\r\n")
        while idx < 0:
            if time.time() > deadline:
                raise TimeoutError("stalled reading headers")
            chunk = self.s.recv(65536)
            if not chunk:
                raise ConnectionError(f"closed during headers; buffered {self.buf[:120]!r}")
            self.buf += chunk
            idx = self.buf.find(b"\r\n\r\n")
        head = self.buf[:idx]
        self.buf = self.buf[idx + 4:]
        clen = 0
        for line in head.split(b"\r\n"):
            if line.lower().startswith(b"content-length:"):
                clen = int(line.split(b":")[1].strip())
        if len(self.buf) < clen:
            self.fill(clen, deadline)
        self.buf = self.buf[clen:]
        return head.split(b"\r\n")[0]
    def close(self):
        self.s.close()

def main():
    depth = int(sys.argv[1]) if len(sys.argv) > 1 else 20
    REQ = b"GET /home HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n"
    c = Conn()
    c.sendall(REQ * depth)
    print(f"sent {depth} pipelined requests in one write")
    t0 = time.time()
    try:
        for i in range(depth):
            status = c.read_response()
            if i == 0 or i == depth - 1 or (i % 5) == 0:
                print(f"  response {i+1}: {status.decode()} after {time.time()-t0:.3f}s")
        print(f"ALL {depth} responses received in {time.time()-t0:.3f}s")
    except Exception as e:
        print(f"FAILED at response {i+1} after {time.time()-t0:.3f}s: {e}")
        sys.exit(1)
    finally:
        c.close()

if __name__ == "__main__":
    main()