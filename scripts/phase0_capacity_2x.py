#!/usr/bin/env python3
"""Phase 0 above-capacity acceptance with `wrk`.

Drives `wrk` at twice the configured connection capacity and verifies the
bounded overload contract: no client timeouts, listener interest never disabled
for capacity, and the active-connection maximum never exceeds capacity. A full
`503` (or a prompt refusal) must account for the excess clients.

Writes benchmarks/production_phase0_2x.json and exits non-zero on failure.
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
ARTIFACT = os.path.join(REPO_ROOT, "benchmarks", "production_phase0_2x.json")

RPS = re.compile(r"Requests/sec:\s+([\d.]+)")
REQ = re.compile(r"([\d,]+) requests in ([\d.]+)s")
P99 = re.compile(r"99%\s+([\d.]+)([a-zA-Z]+)")
NON2XX = re.compile(r"Non-2xx or 3xx responses:\s+([\d,]+)")
ERR = re.compile(r"Socket errors:\s+(.*)")


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


def read_metrics(path):
    try:
        with open(path, encoding="utf-8") as source:
            return json.load(source)
    except (OSError, ValueError):
        return {}


def parse_wrk(output):
    result = {"raw_tail": output.strip().splitlines()[-3:]}
    m = RPS.search(output)
    result["requests_per_sec"] = float(m.group(1)) if m else 0.0
    m = REQ.search(output)
    if m:
        result["requests"] = int(m.group(1).replace(",", ""))
        result["elapsed_seconds"] = float(m.group(2))
    m = P99.search(output)
    if m:
        result["p99"] = float(m.group(1))
        result["p99_unit"] = m.group(2)
    m = NON2XX.search(output)
    result["non_2xx_responses"] = int(m.group(1).replace(",", "")) if m else 0
    errors = {"connect": 0, "read": 0, "write": 0, "timeout": 0}
    m = ERR.search(output)
    if m:
        for part in m.group(1).split(","):
            bits = part.split()
            if len(bits) == 2 and bits[0] in errors:
                try:
                    errors[bits[0]] = int(bits[1])
                except ValueError:
                    pass
    result["socket_errors"] = errors
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--capacity", type=int, default=256,
                        help="HTTP_SERVER_MAX_CONNECTIONS for the run")
    parser.add_argument("--duration", type=int, default=8)
    parser.add_argument("--threads", type=int, default=4)
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

    metrics_file = os.path.join(REPO_ROOT, "benchmarks",
                                ".phase0_2x_metrics.json")
    if os.path.exists(metrics_file):
        os.remove(metrics_file)

    env = dict(os.environ)
    env.update({
        "HTTP_SERVER_PORT": str(args.port),
        "HTTP_SERVER_MAX_CONNECTIONS": str(args.capacity),
        "HTTP_SERVER_METRICS_FILE": metrics_file,
        "HTTP_SERVER_ACCESS_LOG": "0",
    })

    conns = args.capacity * 2
    cmd = [args.wrk, "-t%d" % args.threads, "-c%d" % conns,
           "-d%ds" % args.duration, "--latency",
           "http://%s:%d/home" % (args.host, args.port)]

    server = subprocess.Popen([args.server], cwd=REPO_ROOT, env=env,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              text=True)
    artifact = {"capacity": args.capacity, "connections": conns,
                "wrk_command": " ".join(cmd)}
    failures = []
    try:
        if not wait_for_port(args.host, args.port):
            print("FAIL: server did not start", file=sys.stderr)
            return 2
        proc = subprocess.run(cmd, cwd=REPO_ROOT, capture_output=True,
                              text=True, timeout=args.duration + 60)
        output = proc.stdout + proc.stderr
        artifact["wrk"] = parse_wrk(output)

        metrics = read_metrics(metrics_file)
        artifact["server_metrics"] = {
            key: metrics.get(key) for key in (
                "connection_capacity", "active_connections",
                "active_connections_max", "admission_rejected_capacity",
                "overload_responses", "connection_resets",
                "listener_disabled_count", "listen_drops",
                "accept_error_emfile", "accept_error_enfile",
            )
        }

        wrk = artifact["wrk"]
        errors = wrk["socket_errors"]
        if errors.get("timeout", 0) > 0:
            failures.append("client timeouts under 2x capacity: %d" %
                            errors["timeout"])
        if metrics.get("listener_disabled_count", 0) != 0:
            failures.append("listener was disabled under load")
        if metrics.get("connection_capacity") != args.capacity:
            failures.append("reported capacity %s != %d" % (
                metrics.get("connection_capacity"), args.capacity))
        active_max = metrics.get("active_connections_max", 0)
        if active_max > args.capacity:
            failures.append("active_connections_max %d exceeded capacity %d" % (
                active_max, args.capacity))
        # Excess clients must be answered with a 503/refusal, not queued.
        overloaded = (wrk["non_2xx_responses"] > 0 or
                      metrics.get("admission_rejected_capacity", 0) > 0)
        if not overloaded:
            failures.append("no above-capacity rejection recorded")
        # wrk reports percentile latency with an explicit unit (`3.87ms`,
        # `1.20s`); normalize to milliseconds without double-converting.
        p99_value = wrk.get("p99", 0.0)
        p99_unit = wrk.get("p99_unit", "ms")
        if p99_unit == "s":
            p99_ms = p99_value * 1000.0
        elif p99_unit == "us":
            p99_ms = p99_value / 1000.0
        else:
            p99_ms = p99_value
        if p99_ms > 5000:
            failures.append("p99 %.1f ms is unbounded" % p99_ms)

        artifact["success"] = not failures
        artifact["failures"] = failures
    finally:
        if server.poll() is None:
            server.send_signal(signal.SIGTERM)
        try:
            server.wait(timeout=20)
        except subprocess.TimeoutExpired:
            server.kill()
            server.wait()
        if os.path.exists(metrics_file):
            os.remove(metrics_file)

    os.makedirs(os.path.dirname(args.artifact), exist_ok=True)
    with open(args.artifact, "w", encoding="utf-8") as out:
        json.dump(artifact, out, indent=2, sort_keys=True)

    print("2x capacity: conns=%d rps=%.0f p99=%.1fms non2xx=%d errors=%s" % (
        conns, artifact["wrk"]["requests_per_sec"],
        artifact["wrk"].get("p99", 0.0), artifact["wrk"]["non_2xx_responses"],
        artifact["wrk"]["socket_errors"]))
    print("server: capacity=%s active_max=%s rejected=%s overload=%s "
          "listener_disabled=%s" % (
              artifact["server_metrics"].get("connection_capacity"),
              artifact["server_metrics"].get("active_connections_max"),
              artifact["server_metrics"].get("admission_rejected_capacity"),
              artifact["server_metrics"].get("overload_responses"),
              artifact["server_metrics"].get("listener_disabled_count")))
    if failures:
        for failure in failures:
            print("FAIL: %s" % failure, file=sys.stderr)
        return 1
    print("PASS: phase 0 2x capacity (artifact: %s)" % args.artifact)
    return 0


if __name__ == "__main__":
    sys.exit(main())
