#!/usr/bin/env python3
"""Phase 0 access-logging overhead acceptance.

Runs the same keep-alive `wrk` load twice on one host: once with access
logging disabled and once enabled (writing JSON lines to a file). The enabled
run must stay within 5% of the disabled run, proving the nonblocking writer
does not stall an event loop. Any records the writer could not keep up with are
reported as `dropped_logs` rather than blocking.

Writes benchmarks/production_phase0_accesslog.json and exits non-zero when the
overhead exceeds the bound.
"""

import argparse
import json
import os
import re
import signal
import socket
import subprocess
import sys
import time

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_PORT = 8081
ARTIFACT = os.path.join(REPO_ROOT, "benchmarks",
                        "production_phase0_accesslog.json")
OVERHEAD_LIMIT = 0.05

RPS = re.compile(r"Requests/sec:\s+([\d.]+)")
P99 = re.compile(r"99%\s+([\d.]+)([a-zA-Z]+)")
DROPPED = re.compile(r"dropped_logs=(\d+)")


def now():
    return time.monotonic()


def port_in_use(host, port):
    try:
        with socket.create_connection((host, port), 0.2):
            return True
    except OSError:
        return False


def wait_for_port(host, port, timeout=5.0):
    deadline = now() + timeout
    while now() < deadline:
        try:
            with socket.create_connection((host, port), 0.2):
                return True
        except OSError:
            time.sleep(0.05)
    return False


def parse_wrk(output):
    m = RPS.search(output)
    rps = float(m.group(1)) if m else 0.0
    m = P99.search(output)
    p99 = float(m.group(1)) if m else 0.0
    unit = m.group(2) if m else ""
    return {"rps": rps, "p99": p99, "p99_unit": unit}


def run_case(args, access_log, log_file):
    env = dict(os.environ)
    env.update({
        "HTTP_SERVER_PORT": str(args.port),
        "HTTP_SERVER_MAX_CONNECTIONS": str(args.capacity),
        "HTTP_SERVER_ACCESS_LOG": "1" if access_log else "0",
    })
    if log_file:
        env["HTTP_SERVER_LOG_FILE"] = log_file
        if os.path.exists(log_file):
            os.remove(log_file)

    cmd = [args.wrk, "-t%d" % args.threads, "-c%d" % args.conns,
           "-d%ds" % args.duration, "--latency",
           "http://%s:%d/home" % (args.host, args.port)]

    server = subprocess.Popen([args.server], cwd=REPO_ROOT, env=env,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              text=True)
    best = None
    try:
        if not wait_for_port(args.host, args.port):
            raise RuntimeError("server did not start")
        for _ in range(args.repeats):
            proc = subprocess.run(cmd, cwd=REPO_ROOT, capture_output=True,
                                  text=True, timeout=args.duration + 60)
            row = parse_wrk(proc.stdout + proc.stderr)
            if best is None or row["rps"] > best["rps"]:
                best = row
        if server.poll() is None:
            server.send_signal(signal.SIGTERM)
        try:
            _, stderr = server.communicate(timeout=20)
        except subprocess.TimeoutExpired:
            server.kill()
            _, stderr = server.communicate()
        # The shutdown record (which carries the drop counter) goes to the log
        # file when one is configured, otherwise to stderr.
        tail = stderr or ""
        if log_file and os.path.exists(log_file):
            with open(log_file, encoding="utf-8", errors="replace") as source:
                tail += source.read()[-4096:]
        m = DROPPED.search(tail)
        best["dropped_logs"] = int(m.group(1)) if m else 0
    finally:
        if server.poll() is None:
            server.kill()
            server.wait()
    return best


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--capacity", type=int, default=256)
    parser.add_argument("--conns", type=int, default=64)
    parser.add_argument("--threads", type=int, default=4)
    parser.add_argument("--duration", type=int, default=6)
    parser.add_argument("--repeats", type=int, default=2)
    parser.add_argument("--server", default=os.path.join(REPO_ROOT,
                                                         "bin/http_server"))
    parser.add_argument("--wrk", default="wrk")
    parser.add_argument("--artifact", default=ARTIFACT)
    args = parser.parse_args(argv)

    if not os.path.exists(args.server):
        print("FAIL: server binary not found at %s" % args.server,
              file=sys.stderr)
        return 2
    if port_in_use(args.host, args.port):
        print("FAIL: a server is already listening on %s:%d" % (
            args.host, args.port), file=sys.stderr)
        return 2

    log_file = os.path.join(REPO_ROOT, "benchmarks",
                            ".phase0_accesslog_target.jsonl")
    failures = []
    try:
        disabled = run_case(args, access_log=False, log_file=None)
        enabled = run_case(args, access_log=True, log_file=log_file)
    finally:
        if os.path.exists(log_file):
            os.remove(log_file)

    overhead = 0.0
    if disabled["rps"] > 0:
        overhead = (disabled["rps"] - enabled["rps"]) / disabled["rps"]

    artifact = {
        "capacity": args.capacity,
        "connections": args.conns,
        "overhead_limit": OVERHEAD_LIMIT,
        "access_log_disabled": disabled,
        "access_log_enabled": enabled,
        "rps_overhead": round(overhead, 5),
    }
    if overhead >= OVERHEAD_LIMIT:
        failures.append("access-log overhead %.2f%% exceeds %.0f%%" % (
            overhead * 100.0, OVERHEAD_LIMIT * 100.0))
    artifact["success"] = not failures
    artifact["failures"] = failures

    os.makedirs(os.path.dirname(args.artifact), exist_ok=True)
    with open(args.artifact, "w", encoding="utf-8") as out:
        json.dump(artifact, out, indent=2, sort_keys=True)

    print("access-log overhead: disabled=%.0f rps, enabled=%.0f rps, "
          "overhead=%.2f%%, dropped=%d" % (
              disabled["rps"], enabled["rps"], overhead * 100.0,
              enabled["dropped_logs"]))
    if failures:
        for failure in failures:
            print("FAIL: %s" % failure, file=sys.stderr)
        return 1
    print("PASS: phase 0 access-log overhead (artifact: %s)" % args.artifact)
    return 0


if __name__ == "__main__":
    sys.exit(main())
