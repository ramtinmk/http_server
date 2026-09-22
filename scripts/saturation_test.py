#!/usr/bin/env python3
"""Phase 4 saturation acceptance test.

Starts the server with a reduced, operator-set connection capacity and drives
three observations required by plans/scaling-plan-phase4.md section B:

  1. Fixed-rate request waves below, equal to, and above the configured
     capacity. Every wave must complete responses without a read-error storm.
  2. Listener backpressure: saturating the table disables the listener, and a
     freed slot re-enables it so a fresh client is served.
  3. Post-drain cleanup: active connections and leased buffer bytes return to
     zero, and RSS / descriptors return to their pre-test baseline.

The run writes a repeatable JSON artifact to benchmarks/phase4_saturation.json
and exits non-zero when any acceptance condition fails.
"""

import argparse
import json
import os
import signal
import socket
import subprocess
import sys
import threading
import time

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_PORT = 8081
ARTIFACT = os.path.join(REPO_ROOT, "benchmarks", "phase4_saturation.json")


def now():
    return time.monotonic()


def port_in_use(host, port):
    try:
        with socket.create_connection((host, port), 0.2):
            return True
    except OSError:
        return False


def wait_for_server(host, port, timeout):
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


def read_rss_kb(pid):
    try:
        with open("/proc/%d/status" % pid, encoding="utf-8") as source:
            for line in source:
                if line.startswith("VmRSS:"):
                    return int(line.split()[1])
    except OSError:
        pass
    return 0


def count_fds(pid):
    try:
        return len(os.listdir("/proc/%d/fd" % pid))
    except OSError:
        return 0


def percentile(values, fraction):
    if not values:
        return 0.0
    ordered = sorted(values)
    index = min(len(ordered) - 1, int(len(ordered) * fraction))
    return ordered[index]


def read_one_response(sock, timeout):
    """Read one Content-Length-framed or close-delimited response."""
    sock.settimeout(timeout)
    data = b""
    while b"\r\n\r\n" not in data:
        chunk = sock.recv(4096)
        if not chunk:
            return data, False
        data += chunk
    head, _, body = data.partition(b"\r\n\r\n")
    content_length = None
    for line in head.split(b"\r\n")[1:]:
        if line.lower().startswith(b"content-length:"):
            content_length = int(line.split(b":", 1)[1].strip())
            break
    if content_length is None:
        return data, b"200" in head.split(b"\r\n", 1)[0]
    while len(body) < content_length:
        chunk = sock.recv(4096)
        if not chunk:
            return data, False
        body += chunk
    status_ok = head.startswith(b"HTTP/1.1 200") or head.startswith(b"HTTP/1.1 404")
    return data, status_ok


def request_worker(host, port, timeout, results):
    entry = {"ok": False, "error": None, "latency_ms": 0.0, "status": None}
    started = now()
    try:
        with socket.create_connection((host, port), timeout) as sock:
            request = (b"GET /home HTTP/1.1\r\nHost: saturation\r\n"
                       b"Connection: close\r\n\r\n")
            sock.sendall(request)
            raw, ok = read_one_response(sock, timeout)
        entry["latency_ms"] = (now() - started) * 1000.0
        first_line = raw.split(b"\r\n", 1)[0] if raw else b""
        if first_line[:12] == b"HTTP/1.1 503":
            entry["status"] = 503
            entry["error"] = "overload"
            entry["ok"] = False
        elif ok:
            entry["status"] = 200 if b"200" in first_line else 404
            entry["ok"] = True
        else:
            entry["error"] = "framing"
    except socket.timeout:
        entry["error"] = "timeout"
        entry["latency_ms"] = (now() - started) * 1000.0
    except ConnectionResetError:
        entry["error"] = "reset"
        entry["latency_ms"] = (now() - started) * 1000.0
    except OSError as exc:
        entry["error"] = "connect" if entry["latency_ms"] == 0 else "receive"
        entry["latency_ms"] = (now() - started) * 1000.0
        entry.setdefault("detail", str(exc))
    results.append(entry)


def run_wave(host, port, concurrency, timeout):
    results = []
    threads = [
        threading.Thread(target=request_worker,
                         args=(host, port, timeout, results))
        for _ in range(concurrency)
    ]
    started = now()
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    elapsed = max(1e-6, now() - started)

    completed = sum(1 for entry in results if entry["ok"])
    errors = {}
    for entry in results:
        if entry["error"]:
            errors[entry["error"]] = errors.get(entry["error"], 0) + 1
    latencies = [entry["latency_ms"] for entry in results]
    return {
        "concurrency": concurrency,
        "offered": len(results),
        "completed": completed,
        "elapsed_seconds": round(elapsed, 4),
        "throughput_rps": round(completed / elapsed, 2),
        "p99_ms": round(percentile(latencies, 0.99), 3),
        "errors": errors,
    }


def open_idle_connections(host, port, count, timeout):
    sockets = []
    for _ in range(count):
        sock = socket.create_connection((host, port), timeout)
        sockets.append(sock)
    return sockets


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--capacity", type=int, default=16,
                        help="HTTP_SERVER_MAX_CONNECTIONS for the run")
    parser.add_argument("--server", default=os.path.join(REPO_ROOT,
                                                         "bin/http_server"))
    parser.add_argument("--timeout", type=float, default=10.0)
    parser.add_argument("--artifact", default=ARTIFACT)
    args = parser.parse_args(argv)

    if not os.path.exists(args.server):
        print("FAIL: server binary not found at %s" % args.server, file=sys.stderr)
        return 2
    if port_in_use(args.host, args.port):
        print("FAIL: a server is already listening on %s:%d; stop it first" %
              (args.host, args.port), file=sys.stderr)
        return 2

    metrics_file = os.path.join(REPO_ROOT, "benchmarks",
                                ".phase4_saturation_metrics.json")
    env = dict(os.environ)
    env["HTTP_SERVER_ACCESS_LOG"] = "0"
    env["HTTP_SERVER_MAX_CONNECTIONS"] = str(args.capacity)
    env["HTTP_SERVER_METRICS_FILE"] = metrics_file

    if os.path.exists(metrics_file):
        os.remove(metrics_file)

    server = subprocess.Popen([args.server], cwd=REPO_ROOT, env=env,
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    artifact = {"capacity": args.capacity, "waves": {}, "checks": {}}
    failures = []

    try:
        if not wait_for_server(args.host, args.port, 5.0):
            print("FAIL: server did not start", file=sys.stderr)
            return 2

        baseline_rss = read_rss_kb(server.pid)
        baseline_fds = count_fds(server.pid)
        artifact["baseline"] = {"rss_kb": baseline_rss, "open_fds": baseline_fds}

        # --- Observation 1: below / equal / above capacity waves -----------
        waves = {
            "below": args.capacity // 2,
            "equal": args.capacity,
            "above": args.capacity * 3,
        }
        for name, concurrency in waves.items():
            concurrency = max(1, concurrency)
            wave = run_wave(args.host, args.port, concurrency, args.timeout)
            artifact["waves"][name] = wave
            print("wave=%-5s concurrency=%d completed=%d p99=%.1fms errors=%s" %
                  (name, concurrency, wave["completed"], wave["p99_ms"],
                   wave["errors"]))
            if wave["completed"] == 0:
                failures.append("wave %s completed no responses" % name)
            if wave["errors"].get("reset", 0) > 0:
                failures.append("wave %s saw resets: %d" % (
                    name, wave["errors"]["reset"]))
            # A read-error storm would be receive/reset errors far exceeding
            # the number of completed responses.
            read_errors = (wave["errors"].get("receive", 0) +
                           wave["errors"].get("reset", 0))
            if read_errors > max(4, wave["completed"]):
                failures.append("wave %s read-error storm: %d errors vs %d "
                                "completed" % (name, read_errors,
                                               wave["completed"]))

        # --- Observation 2: listener backpressure toggles ------------------
        idle = open_idle_connections(args.host, args.port, args.capacity,
                                     args.timeout)
        deadline = now() + 3.0
        saturated = {}
        while now() < deadline:
            saturated = read_metrics(metrics_file)
            if (saturated.get("active_connections", 0) >= args.capacity and
                    saturated.get("listener_disabled_count", 0) >= 1):
                break
            time.sleep(0.05)
        artifact["checks"]["saturated_metrics"] = {
            key: saturated.get(key) for key in (
                "active_connections", "active_connections_max",
                "connection_capacity", "listener_disabled_count",
                "buffer_bytes_current",
            )
        }
        print("saturated: active=%s max=%s capacity=%s listener_disabled=%s "
              "buffer_bytes=%s" % (
                  saturated.get("active_connections"),
                  saturated.get("active_connections_max"),
                  saturated.get("connection_capacity"),
                  saturated.get("listener_disabled_count"),
                  saturated.get("buffer_bytes_current")))
        if saturated.get("active_connections", 0) < args.capacity:
            failures.append("connection table never reached capacity")
        if saturated.get("listener_disabled_count", 0) < 1:
            failures.append("listener was never disabled under saturation")
        if saturated.get("connection_capacity") != args.capacity:
            failures.append("reported capacity %s != requested %d" % (
                saturated.get("connection_capacity"), args.capacity))
        if saturated.get("buffer_bytes_current", 0) != 0:
            failures.append("idle connections retained input buffers: %s bytes" %
                            saturated.get("buffer_bytes_current"))

        # Free half the slots and confirm a fresh client is served, which
        # proves the listener was re-enabled.
        for sock in idle[: len(idle) // 2]:
            sock.close()
        served = run_wave(args.host, args.port, 1, args.timeout)
        artifact["checks"]["after_free"] = served
        print("after freeing slots: completed=%d errors=%s" %
              (served["completed"], served["errors"]))
        if served["completed"] < 1:
            failures.append("listener did not re-enable after a slot freed")

        for sock in idle[len(idle) // 2:]:
            sock.close()
        for sock in idle[: len(idle) // 2]:
            sock.close()

        # --- Observation 3: post-drain cleanup -----------------------------
        deadline = now() + 5.0
        drained = {}
        while now() < deadline:
            drained = read_metrics(metrics_file)
            if (drained.get("active_connections", 1) == 0 and
                    drained.get("buffer_bytes_current", 1) == 0):
                break
            time.sleep(0.05)
        artifact["checks"]["drained_metrics"] = {
            key: drained.get(key) for key in (
                "active_connections", "buffer_bytes_current",
                "listener_disabled_count", "overload_responses",
                "connection_resets",
            )
        }
        print("drained: active=%s buffer_bytes=%s resets=%s" % (
            drained.get("active_connections"),
            drained.get("buffer_bytes_current"),
            drained.get("connection_resets")))
        if drained.get("active_connections", 1) != 0:
            failures.append("active connections did not return to zero")
        if drained.get("buffer_bytes_current", 1) != 0:
            failures.append("leased buffer bytes did not return to zero")

        final_rss = read_rss_kb(server.pid)
        final_fds = count_fds(server.pid)
        artifact["after"] = {"rss_kb": final_rss, "open_fds": final_fds}
        print("resources: rss %dkB -> %dkB, fds %d -> %d" % (
            baseline_rss, final_rss, baseline_fds, final_fds))
        if final_fds > baseline_fds + 16:
            failures.append("file descriptors grew from %d to %d" % (
                baseline_fds, final_fds))
        if final_rss > baseline_rss * 1.5 + 4096:
            failures.append("RSS grew from %dkB to %dkB" % (
                baseline_rss, final_rss))

        artifact["success"] = not failures
        artifact["failures"] = failures
    finally:
        server.send_signal(signal.SIGTERM)
        try:
            server.wait(timeout=3)
        except subprocess.TimeoutExpired:
            server.kill()
            server.wait()
        if os.path.exists(metrics_file):
            os.remove(metrics_file)

    os.makedirs(os.path.dirname(args.artifact), exist_ok=True)
    with open(args.artifact, "w", encoding="utf-8") as output:
        json.dump(artifact, output, indent=2, sort_keys=True)

    if failures:
        for failure in failures:
            print("FAIL: %s" % failure, file=sys.stderr)
        return 1
    print("PASS: phase 4 saturation acceptance (artifact: %s)" % args.artifact)
    return 0


if __name__ == "__main__":
    sys.exit(main())
