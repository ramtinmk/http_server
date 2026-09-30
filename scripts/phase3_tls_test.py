#!/usr/bin/env python3
"""E2E: Phase 3 TLS termination acceptance.

Starts a real server that serves the document root over both plaintext and TLS
and checks, black-box:

  * a TLS handshake succeeds with ALPN negotiated to http/1.1 and a modern
    protocol/cipher (no weak protocol, cipher, or certificate finding);
  * a small and a large (> cache threshold) file are delivered byte-for-byte
    over TLS, exercising the buffered TLS file-body path;
  * the plaintext listener keeps serving unchanged;
  * many concurrent handshakes complete with zero handshake failures and no
    event-loop stall (server-side header timeouts stay zero);
  * a SIGHUP certificate reload keeps an in-flight TLS connection alive and
    makes new connections present the reloaded certificate;
  * the TLS/plaintext throughput ratio is measured and recorded.

The result is written to benchmarks/production_phase3_tls.json and the process
exits non-zero when any gate fails.
"""
import argparse
import hashlib
import json
import os
import shutil
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
import time

SMALL_BODY = b"phase3 tls small body\n" * 32
LARGE_BYTES = 2 * 1024 * 1024
WEAK_TOKENS = ("RC4", "3DES", "DES-CBC", "DES-CBC3", "MD5", "NULL", "EXPORT",
               "anon", "ADH")
WEAK_VERSIONS = ("SSLv2", "SSLv3", "TLSv1", "TLSv1.1")


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def make_cert(dir_path, cn, prefix):
    cert = os.path.join(dir_path, prefix + ".pem")
    key = os.path.join(dir_path, prefix + ".key")
    subprocess.run(
        ["openssl", "req", "-x509", "-newkey", "rsa:2048", "-keyout", key,
         "-out", cert, "-days", "1", "-nodes", "-subj", "/CN=" + cn],
        check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    os.chmod(key, 0o600)
    return cert, key


def start_server(repo_root, root, port, tls_port, cert, key, metrics_path):
    server = os.path.join(repo_root, "bin", "http_server")
    env = dict(os.environ)
    env["HTTP_SERVER_METRICS_FILE"] = metrics_path
    env["HTTP_SERVER_ACCESS_LOG"] = "0"
    proc = subprocess.Popen(
        [server, "--document-root", root, "--port", str(port),
         "--tls", "1", "--tls-port", str(tls_port),
         "--tls-cert-file", cert, "--tls-key-file", key,
         "--cache-budget-bytes", "0", "--max-keepalive-requests", "1000000",
         "--log-level", "error"],
        cwd=repo_root, env=env, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, text=True)
    deadline = time.time() + 10
    while time.time() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.3):
                return proc
        except OSError:
            if proc.poll() is not None:
                out = proc.stdout.read() if proc.stdout else ""
                raise RuntimeError("server exited early:\n%s" % out)
            time.sleep(0.05)
    proc.kill()
    raise RuntimeError("server did not start")


def tls_context(alpn=("http/1.1",)):
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    if alpn:
        ctx.set_alpn_protocols(list(alpn))
    return ctx


def read_response(sock):
    """Read one HTTP response from a (TLS or plain) socket. Returns (status,
    headers dict, body)."""
    data = b""
    while b"\r\n\r\n" not in data:
        chunk = sock.recv(65536)
        if not chunk:
            break
        data += chunk
    head, _, rest = data.partition(b"\r\n\r\n")
    lines = head.split(b"\r\n")
    status = int(lines[0].split(b" ")[1]) if len(lines[0].split(b" ")) > 1 else 0
    headers = {}
    for line in lines[1:]:
        if b":" in line:
            k, v = line.split(b":", 1)
            headers[k.strip().lower()] = v.strip()
    body = rest
    if b"content-length" in headers:
        want = int(headers[b"content-length"])
        while len(body) < want:
            chunk = sock.recv(65536)
            if not chunk:
                break
            body += chunk
        body = body[:want]
    return status, headers, body


def tls_get(port, path, ctx=None, conn=None):
    own = conn is None
    if own:
        ctx = ctx or tls_context()
        raw = socket.create_connection(("127.0.0.1", port), timeout=10)
        conn = ctx.wrap_socket(raw, server_hostname="localhost")
    req = ("GET %s HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n"
           % path)
    conn.sendall(req.encode())
    status, headers, body = read_response(conn)
    return status, headers, body, conn


def plain_get(port, path):
    with socket.create_connection(("127.0.0.1", port), timeout=10) as s:
        req = ("GET %s HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"
               % path)
        s.sendall(req.encode())
        return read_response(s)[:2]


def read_metrics(path):
    try:
        with open(path, "r") as f:
            return json.loads(f.read().strip())
    except (OSError, ValueError):
        return {}


def handshake_worker(port, ctx, path, results, index):
    try:
        status, _, body, conn = tls_get(port, path, ctx=ctx)
        ok = status == 200 and body == SMALL_BODY
        conn.close()
        results[index] = ok
    except Exception as exc:  # noqa: BLE001 - report any failure
        results[index] = "error: %s" % exc


def measure_rps(port, path, ctx, iterations):
    best = 0.0
    for _ in range(3):
        conn = None
        start = time.perf_counter()
        try:
            for _ in range(iterations):
                if conn is None:
                    raw = socket.create_connection(("127.0.0.1", port),
                                                   timeout=10)
                    conn = ctx.wrap_socket(raw, server_hostname="localhost")
                status, _, _, conn = tls_get(port, path, conn=conn)
                if status != 200:
                    raise RuntimeError("status %d" % status)
        finally:
            if conn is not None:
                conn.close()
        elapsed = time.perf_counter() - start
        if elapsed > 0:
            best = max(best, iterations / elapsed)
    return best


def measure_plain_rps(port, path, iterations):
    import http.client
    best = 0.0
    for _ in range(3):
        conn = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
        start = time.perf_counter()
        try:
            for _ in range(iterations):
                conn.request("GET", path)
                resp = conn.getresponse()
                resp.read()
                if resp.status != 200:
                    raise RuntimeError("status %d" % resp.status)
        finally:
            conn.close()
        elapsed = time.perf_counter() - start
        if elapsed > 0:
            best = max(best, iterations / elapsed)
    return best


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", default=None,
                        help="artifact path (default: "
                             "benchmarks/production_phase3_tls.json)")
    parser.add_argument("--skip-throughput", action="store_true",
                        help="record throughput as null (for constrained hosts)")
    parser.add_argument("--concurrency", type=int, default=50,
                        help="concurrent TLS handshakes (default 50)")
    args = parser.parse_args(argv)

    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out_path = args.output or os.path.join(
        repo_root, "benchmarks", "production_phase3_tls.json")

    tmp = tempfile.mkdtemp(prefix="http_phase3_")
    root = os.path.join(tmp, "docroot")
    os.makedirs(root)
    with open(os.path.join(root, "hello.txt"), "wb") as f:
        f.write(SMALL_BODY)
    with open(os.path.join(root, "large.bin"), "wb") as f:
        f.write(os.urandom(LARGE_BYTES))
    cert, key = make_cert(tmp, "localhost", "server")

    plain_port = free_port()
    tls_port = free_port()
    metrics_path = os.path.join(tmp, "metrics.json")
    proc = start_server(repo_root, root, plain_port, tls_port, cert, key,
                        metrics_path)

    failures = []
    report = {"plain_port": plain_port, "tls_port": tls_port,
              "large_file_bytes": LARGE_BYTES, "concurrency": args.concurrency}
    ctx = tls_context()

    try:
        # 1. Handshake + protocol/cipher + ALPN.
        status, headers, body, conn = tls_get(tls_port, "/hello.txt", ctx=ctx)
        scan = {
            "version": conn.version(),
            "cipher": conn.cipher()[0] if conn.cipher() else None,
            "alpn": conn.selected_alpn_protocol(),
        }
        report["tls"] = scan
        if status != 200 or body != SMALL_BODY:
            failures.append("small file over TLS: status=%s len=%d" %
                            (status, len(body)))
        if scan["alpn"] != "http/1.1":
            failures.append("ALPN not negotiated to http/1.1: %r" % scan["alpn"])
        if scan["version"] in WEAK_VERSIONS or not scan["version"]:
            failures.append("weak/unknown protocol: %r" % scan["version"])
        if not scan["cipher"] or any(t.lower() in scan["cipher"].lower()
                                     for t in WEAK_TOKENS):
            failures.append("weak/unknown cipher: %r" % scan["cipher"])

        # 2. Large file over TLS (buffered path: cache disabled).
        status, _, large_body, _ = tls_get(tls_port, "/large.bin", conn=conn)
        with open(os.path.join(root, "large.bin"), "rb") as f:
            expected = f.read()
        report["large_sha256"] = hashlib.sha256(large_body).hexdigest()
        if status != 200 or large_body != expected:
            failures.append("large file over TLS mismatch: status=%s len=%d" %
                            (status, len(large_body)))
        conn.close()

        # 3. Plaintext listener unchanged.
        pstatus, phdrs = plain_get(plain_port, "/hello.txt")
        report["plaintext_status"] = pstatus
        if pstatus != 200:
            failures.append("plaintext listener status=%s" % pstatus)

        # 4. Concurrent handshakes: no failures, no event-loop stall.
        n = args.concurrency
        results = [None] * n
        threads = [threading.Thread(target=handshake_worker,
                                    args=(tls_port, ctx, "/hello.txt", results,
                                          i))
                   for i in range(n)]
        start = time.perf_counter()
        for t in threads:
            t.start()
        for t in threads:
            t.join(timeout=20)
        elapsed = time.perf_counter() - start
        bad = [r for r in results if r is not True]
        report["concurrent_handshakes"] = {
            "requested": n, "succeeded": n - len(bad),
            "failures": ["%s" % b for b in bad][:5],
            "elapsed_s": round(elapsed, 3),
            "handshakes_per_s": round(n / elapsed, 1) if elapsed > 0 else None,
        }
        if bad:
            failures.append("concurrent handshakes: %d/%d failed" % (len(bad), n))

        time.sleep(0.5)  # let the metrics reporter publish
        m = read_metrics(metrics_path)
        report["tls_handshakes"] = m.get("tls_handshakes")
        report["tls_handshake_failures"] = m.get("tls_handshake_failures")
        report["header_timeouts"] = m.get("header_timeout")
        if m.get("tls_handshake_failures"):
            failures.append("tls_handshake_failures=%s" %
                            m.get("tls_handshake_failures"))
        if m.get("header_timeout"):
            failures.append("header_timeout=%s during TLS load" %
                            m.get("header_timeout"))

        # 5. SIGHUP certificate reload keeps connections and swaps the cert.
        persist_raw = socket.create_connection(("127.0.0.1", tls_port),
                                               timeout=10)
        persist = ctx.wrap_socket(persist_raw, server_hostname="localhost")
        pstatus1, _, pbody1, persist = tls_get(tls_port, "/hello.txt",
                                               conn=persist)
        reload_cert, reload_key = make_cert(tmp, "reloaded.example", "reloaded")
        shutil.copyfile(reload_cert, cert)
        shutil.copyfile(reload_key, key)
        os.chmod(key, 0o600)
        os.kill(proc.pid, 1)  # SIGHUP
        time.sleep(0.6)
        pstatus2, _, pbody2, persist = tls_get(tls_port, "/hello.txt",
                                               conn=persist)
        # New connection should present the reloaded certificate.
        fresh_raw = socket.create_connection(("127.0.0.1", tls_port),
                                             timeout=10)
        fresh = ctx.wrap_socket(fresh_raw, server_hostname="localhost")
        served_der = fresh.getpeercert(binary_form=True)
        fresh.close()
        reload_der = ssl.PEM_cert_to_DER_cert(open(reload_cert).read())
        persist_after_ok = (pstatus1 == 200 and pstatus2 == 200 and
                            pbody1 == SMALL_BODY and pbody2 == SMALL_BODY)
        report["reload"] = {
            "persistent_connection_ok": persist_after_ok,
            "new_connection_served_reloaded_cert": served_der == reload_der,
            "connections_dropped": 0 if persist_after_ok else 1,
        }
        persist.close()
        if not persist_after_ok:
            failures.append("reload dropped/corrupted the persistent connection")
        if served_der != reload_der:
            failures.append("reload did not swap the served certificate")

        # 6. TLS/plaintext throughput ratio (recorded, not gated).
        if args.skip_throughput:
            report["throughput"] = None
        else:
            iters = 1000
            plain_rps = measure_plain_rps(plain_port, "/hello.txt", iters)
            tls_rps = measure_rps(tls_port, "/hello.txt", ctx, iters)
            ratio = tls_rps / plain_rps if plain_rps > 0 else 0.0
            report["throughput"] = {
                "plaintext_rps": round(plain_rps, 1),
                "tls_rps": round(tls_rps, 1),
                "ratio": round(ratio, 3),
                "note": "Python ssl client cost dominates; ratio is indicative",
            }
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
        shutil.rmtree(tmp, ignore_errors=True)

    report["failures"] = failures
    report["passed"] = not failures
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, "w") as f:
        json.dump(report, f, indent=2, sort_keys=True)
        f.write("\n")

    if failures:
        print("FAIL: Phase 3 TLS termination acceptance")
        for f_ in failures:
            print("  - %s" % f_)
        print("artifact: %s" % out_path)
        return 1
    print("PASS: Phase 3 TLS termination acceptance (artifact: %s)" % out_path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
