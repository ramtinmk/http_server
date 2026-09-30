#!/usr/bin/env python3
"""E2E: Phase 2 secure static file serving acceptance.

Drives a real server over a temporary document root and checks, black-box:

  * a traversal / symlink / malformed-encoding corpus with exact status codes,
    and that no response ever leaks a sentinel that lives outside the root;
  * directory indexing, nested files, and MIME types;
  * that a file larger than the cache threshold streams correctly, including a
    slow reader that forces partial writes;
  * that cache bytes and open file descriptors stay inside their budgets under
    sustained mixed-file load;
  * that doc-root serving throughput is within 10% of the cached fixed-path
    baseline (best of several samples).

The result is written to benchmarks/production_phase2_static.json and the
process exits non-zero when any gate fails.
"""
import argparse
import http.client
import json
import os
import socket
import subprocess
import sys
import tempfile
import time

DOCROOT_INDEX = b"<h1>phase2 index</h1>\n"
SMALL_BODY = b"S" * 2048
NESTED_BODY = b"nested hello\n"
LARGE_BYTES = 3 * 1024 * 1024
SENTINEL = b"PHASE2_OUTSIDE_ROOT_SENTINEL_9137"


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def raw_request(port, request, timeout=10):
    """Send raw bytes, return (status, headers_bytes, body_bytes)."""
    with socket.create_connection(("127.0.0.1", port), timeout=timeout) as s:
        s.sendall(request)
        chunks = []
        while True:
            try:
                data = s.recv(65536)
            except socket.timeout:
                break
            if not data:
                break
            chunks.append(data)
        raw = b"".join(chunks)
    head, _, body = raw.partition(b"\r\n\r\n")
    status = 0
    first = head.split(b"\r\n", 1)[0]
    parts = first.split(b" ")
    if len(parts) >= 2 and parts[1].isdigit():
        status = int(parts[1])
    return status, head, body


def get(port, path, headers=None):
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=15)
    conn.request("GET", path, headers=headers or {})
    resp = conn.getresponse()
    body = resp.read()
    hdrs = {k.lower(): v for k, v in resp.getheaders()}
    status = resp.status
    conn.close()
    return status, hdrs, body


def make_fixtures(root):
    os.makedirs(os.path.join(root, "nested"), exist_ok=True)
    with open(os.path.join(root, "index.html"), "wb") as f:
        f.write(DOCROOT_INDEX)
    with open(os.path.join(root, "small.html"), "wb") as f:
        f.write(SMALL_BODY)
    with open(os.path.join(root, "nested", "hello.txt"), "wb") as f:
        f.write(NESTED_BODY)
    with open(os.path.join(root, ".hidden"), "wb") as f:
        f.write(b"hidden\n")
    with open(os.path.join(root, "large.bin"), "wb") as f:
        f.write(os.urandom(LARGE_BYTES))
    # Many distinct files, each larger than the tiny cache budget, to force
    # eviction and prove the byte bound holds.
    gen = os.path.join(root, "gen")
    os.makedirs(gen, exist_ok=True)
    for i in range(40):
        with open(os.path.join(gen, "f%02d.bin" % i), "wb") as f:
            f.write(bytes([i]) * 8192)
    # A relative in-root symlink and an absolute escaping symlink.
    try:
        os.symlink("nested/hello.txt", os.path.join(root, "inside.link"))
        os.symlink("/etc/passwd", os.path.join(root, "escape.link"))
        has_symlinks = True
    except OSError:
        has_symlinks = False
    return has_symlinks


def start_server(repo_root, root, port, budget, metrics_path):
    server = os.path.join(repo_root, "bin", "http_server")
    env = dict(os.environ)
    env["HTTP_SERVER_METRICS_FILE"] = metrics_path
    env["HTTP_SERVER_ACCESS_LOG"] = "0"
    proc = subprocess.Popen(
        [server, "--document-root", root, "--port", str(port),
         "--cache-budget-bytes", str(budget), "--max-keepalive-requests",
         "1000000", "--log-level", "error"],
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


def read_metrics(path):
    try:
        with open(path, "r") as f:
            return json.loads(f.read().strip())
    except (OSError, ValueError):
        return {}


def count_fds(pid):
    try:
        return len(os.listdir("/proc/%d/fd" % pid))
    except OSError:
        return -1


def corpus(port, root, has_symlinks=True):
    """Return (results, failures)."""
    cases = [
        # name, raw request, expected status, must_not_contain
        ("root-index", "GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n", 200, None),
        ("index-file", "GET /index.html HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n", 200, None),
        ("nested-file", "GET /nested/hello.txt HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n", 200, None),
        ("query-stripped", "GET /small.html?x=1 HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n", 200, None),
        ("absolute-form", "GET http://x/small.html HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n", 200, None),
        ("missing", "GET /missing HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n", 404, None),
        ("dir-no-index", "GET /nested/ HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n", 404, None),
        ("hidden-file", "GET /.hidden HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n", 403, None),
        # Traversal and malformed encodings: none may reveal the sentinel.
        ("dotdot", "GET /../sentinel.txt HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n", 403, SENTINEL),
        ("enc-dotdot", "GET /%2e%2e/sentinel.txt HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n", 403, SENTINEL),
        ("enc-slash-dotdot", "GET /..%2f..%2fsentinel.txt HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n", 403, SENTINEL),
        ("double-slash-dotdot", "GET //../sentinel.txt HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n", 403, SENTINEL),
        ("nested-dotdot", "GET /nested/../../sentinel.txt HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n", 403, SENTINEL),
        ("mixed-enc-dotdot", "GET /nested/%2e%2e/%2e%2e/sentinel.txt HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n", 403, SENTINEL),
        ("null-byte", "GET /%00 HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n", 400, None),
        ("backslash", "GET /a%5cb HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n", 400, None),
        ("bad-escape", "GET /%zz HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n", 400, None),
        ("literal-dots", "GET /..../sentinel.txt HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n", 403, SENTINEL),
    ]
    if has_symlinks:
        cases.append(
            ("symlink-escape", "GET /escape.link HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n", 403, SENTINEL))

    results = []
    failures = []
    for name, req, want, forbidden in cases:
        status, head, body = raw_request(port, req.encode())
        leaked = bool(forbidden and forbidden in body)
        # The root must serve the directory index, not the legacy fallback.
        index_ok = (name != "root-index") or (DOCROOT_INDEX in body)
        ok = (status == want) and not leaked and index_ok
        results.append({"name": name, "expected": want, "actual": status,
                        "leaked": leaked})
        if not ok:
            reason = ""
            if leaked:
                reason = " (LEAK)"
            elif not index_ok:
                reason = " (no index)"
            failures.append("%s: expected %d got %d%s" %
                            (name, want, status, reason))
    return results, failures


def throughput(repo_root, port, path, samples=3, per_sample=2000):
    """Best-of-N keep-alive request rate for `path` over one connection.

    Request/response is serialized (no pipelining) so both the fixed-path
    baseline and the doc-root path pay the same Python-side cost; the ratio is
    what matters.
    """
    best = 0.0
    for _ in range(samples):
        conn = http.client.HTTPConnection("127.0.0.1", port, timeout=15)
        start = time.perf_counter()
        for _ in range(per_sample):
            conn.request("GET", path)
            resp = conn.getresponse()
            resp.read()
            if resp.status != 200:
                conn.close()
                raise RuntimeError("throughput %s -> %d" % (path, resp.status))
        elapsed = time.perf_counter() - start
        conn.close()
        rate = per_sample / elapsed if elapsed > 0 else 0.0
        best = max(best, rate)
    return best


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", default=None,
                        help="artifact path (default: benchmarks/production_phase2_static.json)")
    parser.add_argument("--skip-throughput", action="store_true",
                        help="record throughput as null (for constrained hosts)")
    args = parser.parse_args(argv)

    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out_path = args.output or os.path.join(
        repo_root, "benchmarks", "production_phase2_static.json")

    tmp = tempfile.mkdtemp(prefix="http_phase2_")
    root = os.path.join(tmp, "docroot")
    os.makedirs(root)
    has_symlinks = make_fixtures(root)
    with open(os.path.join(tmp, "sentinel.txt"), "wb") as f:
        f.write(SENTINEL)

    port = free_port()
    budget = 65536
    metrics_path = os.path.join(tmp, "metrics.json")
    proc = start_server(repo_root, root, port, budget, metrics_path)
    failures = []
    report = {"document_root": root, "cache_budget_bytes": budget,
              "large_file_bytes": LARGE_BYTES}

    try:
        results, fails = corpus(port, root, has_symlinks)
        report["traversal_cases"] = results
        failures += fails

        # MIME type.
        status, hdrs, body = get(port, "/nested/hello.txt")
        report["mime_text_plain"] = hdrs.get("content-type")
        if status != 200 or hdrs.get("content-type") != "text/plain":
            failures.append("MIME: nested/hello.txt -> %s %s" %
                            (status, hdrs.get("content-type")))
        status, hdrs, body = get(port, "/index.html")
        report["mime_text_html"] = hdrs.get("content-type")
        if hdrs.get("content-type") != "text/html":
            failures.append("MIME: index.html -> %s" % hdrs.get("content-type"))

        # Large file streamed correctly, including a slow reader.
        status, hdrs, body = get(port, "/large.bin")
        if status != 200 or len(body) != LARGE_BYTES:
            failures.append("large: status=%s len=%d" % (status, len(body)))
        else:
            with open(os.path.join(root, "large.bin"), "rb") as f:
                if body != f.read():
                    failures.append("large: body mismatch")
        # Range over the streamed file.
        status, hdrs, body = get(port, "/large.bin", {"Range": "bytes=100-199"})
        if status != 206 or len(body) != 100 or \
                hdrs.get("content-range") != "bytes 100-199/%d" % LARGE_BYTES:
            failures.append("large range: status=%s range=%s len=%d" %
                            (status, hdrs.get("content-range"), len(body)))

        # Slow reader: read the 3 MiB in small chunks with pauses.
        slow_ok = True
        with socket.create_connection(("127.0.0.1", port), timeout=20) as s:
            s.sendall(b"GET /large.bin HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
            data = b""
            while True:
                chunk = s.recv(16384)
                if not chunk:
                    break
                data += chunk
                time.sleep(0.002)
            _, _, slow_body = data.partition(b"\r\n\r\n")
        report["slow_reader_bytes"] = len(slow_body)
        if len(slow_body) != LARGE_BYTES:
            slow_ok = False
            failures.append("slow reader: got %d bytes" % len(slow_body))

        # Cache and fd budgets under mixed load.
        gen_files = ["/gen/f%02d.bin" % i for i in range(40)]
        fd_before = count_fds(proc.pid)
        for _ in range(3):
            for p in gen_files:
                get(port, p)
            get(port, "/large.bin")
            get(port, "/small.html")
        time.sleep(0.5)  # let the reporter refresh
        m = read_metrics(metrics_path)
        cache_bytes = m.get("cache_bytes", -1)
        cache_entries = m.get("cache_entries", -1)
        fd_after = count_fds(proc.pid)
        report["cache_bytes"] = cache_bytes
        report["cache_entries"] = cache_entries
        report["fds_before"] = fd_before
        report["fds_after"] = fd_after
        if cache_bytes < 0 or cache_bytes > budget:
            failures.append("cache bytes %s outside budget %d" %
                            (cache_bytes, budget))
        if fd_before > 0 and fd_after > fd_before + 8:
            failures.append("fd growth %d -> %d" % (fd_before, fd_after))

        # Throughput: doc-root cached file vs fixed-path baseline.
        if args.skip_throughput:
            report["throughput"] = None
        else:
            baseline = throughput(repo_root, port, "/home")
            docroot = throughput(repo_root, port, "/small.html")
            ratio = docroot / baseline if baseline > 0 else 0.0
            report["throughput"] = {
                "baseline_home_rps": baseline,
                "docroot_small_rps": docroot,
                "ratio": ratio,
            }
            if ratio < 0.90:
                failures.append("doc-root throughput %.3f of baseline (< 0.90)"
                                % ratio)
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()

    report["failures"] = failures
    report["passed"] = not failures
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, "w") as f:
        json.dump(report, f, indent=2, sort_keys=True)
        f.write("\n")

    if failures:
        print("FAIL: Phase 2 static serving acceptance")
        for f_ in failures:
            print("  - %s" % f_)
        print("artifact: %s" % out_path)
        return 1
    print("PASS: Phase 2 static serving acceptance (artifact: %s)" % out_path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
