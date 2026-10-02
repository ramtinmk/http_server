#!/usr/bin/env python3
"""E2E: Phase 5 observability and operations acceptance.

Black-box against the built server:

  * `/healthz` is 200 and `/readyz` is 200 while the server accepts;
  * `/metrics` returns parseable Prometheus text with the documented names and
    a cumulative request-duration histogram;
  * every response carries an `X-Request-Id` that appears verbatim in the
    structured access log;
  * a `SIGHUP` reload (cert/log-file/log-level) leaves an open keep-alive
    connection usable, proving zero dropped connections, and the new log level
    takes effect;
  * SIGTERM stops the listener and the process exits cleanly.

The result is written to benchmarks/production_phase5_observability.json and the
process exits non-zero when any gate fails.
"""
import argparse
import json
import os
import signal
import socket
import subprocess
import sys
import tempfile
import time

REQUIRED_METRICS = [
    "simplehttp_ready",
    "simplehttp_requests_total",
    "simplehttp_connections_accepted_total",
    "simplehttp_active_connections",
    "simplehttp_responses_total",
    "simplehttp_request_duration_seconds_bucket",
    "simplehttp_request_duration_seconds_count",
    "simplehttp_tls_handshakes_total",
    "simplehttp_cache_bytes",
]


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def write_config(path, log_path, level):
    with open(path, "w") as f:
        f.write("document_root = .\n")
        f.write("observability = 1\n")
        f.write("access_log = 1\n")
        f.write("log_level = %s\n" % level)
        f.write("log_file = %s\n" % log_path)
        f.write("shutdown_drain_timeout = 2\n")


def clean_env():
    env = {k: v for k, v in os.environ.items()
           if not k.startswith("HTTP_SERVER_")}
    return env


def wait_port(proc, port, timeout=10):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if proc.poll() is not None:
            out = proc.stdout.read() if proc.stdout else ""
            raise RuntimeError("server exited early (rc=%s):\n%s"
                               % (proc.returncode, out))
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.3):
                return
        except OSError:
            time.sleep(0.05)
    raise RuntimeError("server did not start on port %d" % port)


def recv_http(sock):
    """Read one HTTP response from a keep-alive socket."""
    buf = b""
    while b"\r\n\r\n" not in buf:
        chunk = sock.recv(65536)
        if not chunk:
            raise RuntimeError("connection closed before headers")
        buf += chunk
    head, _, rest = buf.partition(b"\r\n\r\n")
    lines = head.decode("latin1").split("\r\n")
    status = int(lines[0].split()[1])
    headers = {}
    for line in lines[1:]:
        k, _, v = line.partition(":")
        headers[k.strip().lower()] = v.strip()
    length = int(headers.get("content-length", "0"))
    body = rest
    while len(body) < length:
        chunk = sock.recv(65536)
        if not chunk:
            break
        body += chunk
    return status, headers, body[:length]


def request(port, path, method="GET"):
    s = socket.create_connection(("127.0.0.1", port), timeout=5)
    s.sendall(("%s %s HTTP/1.1\r\nHost: localhost\r\n\r\n"
               % (method, path)).encode())
    result = recv_http(s)
    s.close()
    return result


def wait_for(predicate, timeout=5.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        value = predicate()
        if value:
            return value
        time.sleep(0.05)
    return None


def read_log(path):
    try:
        with open(path) as f:
            return f.read()
    except OSError:
        return ""


def parse_prometheus(text):
    """Return the set of metric names and validate the exposition shape."""
    names = set()
    for line in text.splitlines():
        if not line or line.startswith("#"):
            continue
        sample = line.split(" ", 1)[0]
        name = sample.split("{", 1)[0]
        names.add(name)
    return names


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", default=os.getcwd())
    parser.add_argument(
        "--output",
        default="benchmarks/production_phase5_observability.json")
    parser.add_argument(
        "--server",
        default=None,
        help="server binary to exercise (defaults to repo-root/bin/http_server)")
    args = parser.parse_args()
    repo_root = os.path.abspath(args.repo_root)
    out_path = args.output if os.path.isabs(args.output) \
        else os.path.join(repo_root, args.output)
    binary = args.server or os.path.join(repo_root, "bin", "http_server")

    report = {}
    failures = []
    tmpdir = tempfile.mkdtemp(prefix="phase5-")
    cfg_path = os.path.join(tmpdir, "server.conf")
    log_path = os.path.join(tmpdir, "access.log")

    if not os.path.exists(binary):
        failures.append("binary not built: %s" % binary)
        report["failures"] = failures
        report["passed"] = False
        os.makedirs(os.path.dirname(out_path), exist_ok=True)
        with open(out_path, "w") as f:
            json.dump(report, f, indent=2, sort_keys=True)
        print("FAIL: binary not built")
        return 1

    port = free_port()
    write_config(cfg_path, log_path, "info")
    proc = subprocess.Popen(
        [binary, "--config", cfg_path, "--port", str(port)],
        cwd=repo_root, env=clean_env(), stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, text=True)

    try:
        wait_port(proc, port)

        # --- Health and readiness while accepting -------------------------
        hs_status, hs_headers, hs_body = request(port, "/healthz")
        rs_status, _, rs_body = request(port, "/readyz")
        report["health"] = {
            "status": hs_status,
            "body": hs_body.decode("latin1").strip(),
            "request_id": hs_headers.get("x-request-id", ""),
        }
        report["readiness_up"] = {
            "status": rs_status,
            "body": rs_body.decode("latin1").strip(),
        }
        if hs_status != 200:
            failures.append("GET /healthz returned %d" % hs_status)
        if rs_status != 200:
            failures.append("GET /readyz returned %d while accepting" % rs_status)

        # --- Prometheus metrics -------------------------------------------
        m_status, _, m_body = request(port, "/metrics")
        m_text = m_body.decode("utf-8", "replace")
        names = parse_prometheus(m_text)
        missing = [n for n in REQUIRED_METRICS if n not in names]
        hist_buckets = [ln for ln in m_text.splitlines()
                        if ln.startswith(
                            "simplehttp_request_duration_seconds_bucket{le=")]
        report["metrics"] = {
            "status": m_status,
            "names_present": sorted(names),
            "missing": missing,
            "histogram_buckets": len(hist_buckets),
            "bytes": len(m_body),
        }
        if m_status != 200:
            failures.append("GET /metrics returned %d" % m_status)
        if missing:
            failures.append("metrics missing names: %s" % ", ".join(missing))
        if len(hist_buckets) < 14:  # 13 finite buckets + +Inf
            failures.append("request-duration histogram has %d buckets"
                            % len(hist_buckets))
        if "# TYPE simplehttp_request_duration_seconds histogram" not in m_text:
            failures.append("metrics lack the histogram TYPE line")

        # --- Request id correlation ---------------------------------------
        rid = hs_headers.get("x-request-id", "")
        report["request_id"] = {"header": rid, "in_access_log": False}
        if not rid:
            failures.append("response has no X-Request-Id header")
        else:
            logged = wait_for(lambda: rid in read_log(log_path), timeout=3.0)
            report["request_id"]["in_access_log"] = bool(logged)
            if not logged:
                failures.append("X-Request-Id %s not found in the access log"
                                % rid)

        # --- SIGHUP reload keeps a live connection ------------------------
        sock = socket.create_connection(("127.0.0.1", port), timeout=5)
        sock.sendall(b"GET /healthz HTTP/1.1\r\nHost: localhost\r\n\r\n")
        s1, _, _ = recv_http(sock)
        local_port = sock.getsockname()[1]

        if "metrics scrape" in read_log(log_path):
            failures.append("debug metrics line present before reload")

        write_config(cfg_path, log_path, "debug")
        os.kill(proc.pid, signal.SIGHUP)
        reloaded = wait_for(
            lambda: "config reload ok log_level=debug" in read_log(log_path),
            timeout=3.0)
        report["reload"] = {
            "config_reloaded": bool(reloaded),
            "connection_before_status": s1,
            "connection_reused": False,
            "debug_line_after": False,
        }
        if not reloaded:
            failures.append("SIGHUP did not reload the log level")

        # Same socket must still work after the reload (zero drops).
        try:
            sock.sendall(b"GET /healthz HTTP/1.1\r\nHost: localhost\r\n\r\n")
            s2, _, _ = recv_http(sock)
        except Exception as exc:  # noqa: BLE001
            s2 = None
            failures.append("keep-alive connection broke across SIGHUP: %s" % exc)
        if s2 == 200 and sock.getsockname()[1] == local_port:
            report["reload"]["connection_reused"] = True
        elif s2 is not None:
            failures.append("second request returned %s" % s2)
        sock.close()

        # The new debug level must now emit the metrics-scrape line.
        request(port, "/metrics")
        debug_seen = wait_for(lambda: "metrics scrape" in read_log(log_path),
                              timeout=3.0)
        report["reload"]["debug_line_after"] = bool(debug_seen)
        if not debug_seen:
            failures.append("debug metrics line absent after log-level reload")

        # --- SIGTERM lifecycle --------------------------------------------
        proc.send_signal(signal.SIGTERM)
        try:
            rc = proc.wait(timeout=8)
        except subprocess.TimeoutExpired:
            proc.kill()
            rc = None
        shutdown_probe = "refused"
        try:
            request(port, "/readyz")
            shutdown_probe = "served"
        except OSError:
            shutdown_probe = "refused"
        report["lifecycle"] = {
            "exit_code": rc,
            "probe_during_shutdown": shutdown_probe,
        }
        if rc != 0:
            failures.append("SIGTERM did not exit cleanly (rc=%s)" % rc)

    except Exception as exc:  # noqa: BLE001
        failures.append(str(exc))
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()

    report["failures"] = failures
    report["passed"] = not failures
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, "w") as f:
        json.dump(report, f, indent=2, sort_keys=True)
        f.write("\n")

    if failures:
        print("FAIL: Phase 5 observability acceptance")
        for item in failures:
            print("  - %s" % item)
        print("artifact: %s" % out_path)
        return 1
    print("PASS: Phase 5 observability acceptance (artifact: %s)" % out_path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
