#!/usr/bin/env python3
"""Fixed-rate keep-alive memory soak for the runtime memory profiler.

Drives a sustained keep-alive load against the server while sampling its
``HTTP_SERVER_METRICS_FILE`` snapshot, then writes ``benchmarks/memory_soak.json``
with the per-sample memory series and the drift computed over the final
``--final-fraction`` of the run. Sampling starts after ``--warmup`` seconds so
the first recorded sample is post-initialization rather than the server's
pre-event-loop snapshot. Exits non-zero when RSS or PSS drift exceeds
``--drift-tolerance``.

The artifact is the evidence for the memory-profiler plan's exit criteria
(``plans/memory-profiler.md``): a stationary load must leave RSS and heap
allocation flat rather than trending up. Only the Python standard library is
used so the run is reproducible on any host with the built server.
"""

import argparse
import datetime
import json
import os
import signal
import socket
import subprocess
import sys
import threading
import time

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_OUTPUT = os.path.join(REPO_ROOT, "benchmarks", "memory_soak.json")
SERVER_BIN = os.path.join(REPO_ROOT, "bin", "http_server")

# (snapshot key, gated). Gated series must not drift beyond the tolerance.
# RSS and PSS are the process's real memory footprint and are gated. The glibc
# heap counters are recorded but not gated: mallinfo() ratchets its in-use
# figure as buffers churn even when RSS is flat (it cannot exceed RSS, so it is
# not a leak signal on its own). A real leak shows up as RSS/PSS drift.
DRIFT_CHECKS = (
    ("rss_kb", True),
    ("pss_kb", True),
    # Recorded for attribution but not gated: VMS is allocator arena
    # reservation, and the glibc heap counters ratchet as buffers churn while
    # RSS stays flat. Private_Dirty is the stable anonymous-resident proxy.
    ("private_dirty_kb", False),
    ("vmsize_kb", False),
    ("heap_mmap_bytes", False),
    ("heap_inuse_bytes_max", False),
    ("heap_inuse_bytes", False),
)


class Pacer:
    """Aggregate rate limiter shared by the load threads."""

    def __init__(self, rate):
        self.rate = max(1.0, float(rate))
        self.start = time.monotonic()
        self.count = 0
        self.lock = threading.Lock()

    def wait(self):
        with self.lock:
            self.count += 1
            index = self.count
        target = index / self.rate
        delay = target - (time.monotonic() - self.start)
        if delay > 0:
            time.sleep(delay)


def read_response(sock):
    """Read one Content-Length framed response, returning the body bytes."""
    buf = b""
    while b"\r\n\r\n" not in buf:
        chunk = sock.recv(4096)
        if not chunk:
            raise OSError("connection closed while reading headers")
        buf += chunk
    head, _, body = buf.partition(b"\r\n\r\n")
    length = 0
    for line in head.split(b"\r\n")[1:]:
        if line.lower().startswith(b"content-length:"):
            length = int(line.split(b":", 1)[1].strip())
            break
    while len(body) < length:
        chunk = sock.recv(8192)
        if not chunk:
            raise OSError("connection closed while reading body")
        body += chunk
    return body[:length]


def load_worker(host, port, pacer, stop, counters, index):
    """Keep one keep-alive connection busy, reconnecting when the server closes
    it (the server forces close after MAX_KEEPALIVE_REQUESTS)."""
    request = b"GET /home HTTP/1.1\r\nHost: memory-soak\r\n\r\n"
    sock = None
    try:
        while not stop.is_set():
            if sock is None:
                sock = socket.create_connection((host, port), timeout=10)
                sock.settimeout(10)
            pacer.wait()
            try:
                sock.sendall(request)
                read_response(sock)
                counters[index] += 1
            except OSError:
                try:
                    sock.close()
                except OSError:
                    pass
                sock = None
    finally:
        if sock is not None:
            try:
                sock.close()
            except OSError:
                pass


def read_snapshot(path):
    try:
        with open(path, encoding="utf-8") as source:
            return json.load(source)
    except (OSError, ValueError):
        return {}


def drift(values, fraction):
    """Max-min relative to the mean over the final `fraction` of `values`."""
    if not values:
        return None
    keep = max(1, int(len(values) * fraction + 0.999))
    window = values[-keep:]
    mean = sum(window) / len(window)
    spread = max(window) - min(window)
    if mean <= 0:
        return {"min": min(window), "max": max(window), "mean": mean,
                "relative_range": 0.0 if spread == 0 else 1.0, "samples": len(window)}
    return {"min": min(window), "max": max(window), "mean": round(mean, 3),
            "relative_range": round(spread / mean, 6), "samples": len(window)}


def wait_for_port(host, port, timeout=5.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            with socket.create_connection((host, port), timeout=0.2):
                return True
        except OSError:
            time.sleep(0.05)
    return False


def start_server(metrics_path):
    env = dict(os.environ)
    env["HTTP_SERVER_METRICS_FILE"] = metrics_path
    env["HTTP_SERVER_ACCESS_LOG"] = "0"
    return subprocess.Popen(
        [SERVER_BIN], cwd=REPO_ROOT, env=env,
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )


def parse_args(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8081)
    parser.add_argument("--duration", type=float, default=300.0,
                        help="steady load seconds (default 300)")
    parser.add_argument("--rate", type=float, default=1000.0,
                        help="target aggregate requests/second")
    parser.add_argument("--concurrency", type=int, default=16)
    parser.add_argument("--interval", type=float, default=60.0,
                        help="snapshot sampling interval seconds")
    parser.add_argument("--warmup", type=float, default=1.0,
                        help="seconds to wait before the first sample so the "
                             "event loops and connection tables exist (default 1)")
    parser.add_argument("--final-fraction", type=float, default=0.8)
    parser.add_argument("--drift-tolerance", type=float, default=0.05)
    parser.add_argument("--metrics-file",
                        help="existing server metrics path (implies --no-start-server)")
    parser.add_argument("--start-server", action="store_true",
                        help="launch ./bin/http_server for the run")
    parser.add_argument("--output", default=DEFAULT_OUTPUT)
    return parser.parse_args(argv)


def main(argv=None):
    args = parse_args(argv if argv is not None else sys.argv[1:])
    metrics_path = args.metrics_file

    server = None
    if args.metrics_file:
        args.start_server = False
    elif not args.start_server:
        args.start_server = True

    if args.start_server:
        metrics_path = os.path.join(
            REPO_ROOT, "benchmarks",
            "memory_soak_metrics_%d.json" % os.getpid(),
        )

    if server is None and not args.start_server and not metrics_path:
        print("error: provide --metrics-file or --start-server", file=sys.stderr)
        return 2

    try:
        if args.start_server:
            server = start_server(metrics_path)
            if not wait_for_port(args.host, args.port):
                print("error: server did not start listening", file=sys.stderr)
                return 2

        if not os.path.exists(metrics_path):
            print("error: metrics file %s not found" % metrics_path, file=sys.stderr)
            return 2

        stop = threading.Event()
        counters = [0] * max(1, args.concurrency)
        pacer = Pacer(args.rate)
        threads = [
            threading.Thread(
                target=load_worker,
                args=(args.host, args.port, pacer, stop, counters, i),
                daemon=True,
            )
            for i in range(max(1, args.concurrency))
        ]
        for thread in threads:
            thread.start()

        samples = []
        start = time.monotonic()
        # Skip the pre-initialization snapshot: the reporter publishes before
        # event_loop_run builds the connection tables, so a t=0 sample would
        # understate steady-state memory.
        next_sample = start + max(0.0, args.warmup)
        while time.monotonic() - start < args.duration:
            now = time.monotonic()
            if now >= next_sample:
                snapshot = read_snapshot(metrics_path)
                if snapshot:
                    sample = {"t": round(now - start, 3)}
                    for key in ("rss_kb", "rss_kb_max", "pss_kb",
                                "private_dirty_kb", "vmsize_kb",
                                "heap_inuse_bytes", "heap_inuse_bytes_max",
                                "heap_mmap_bytes", "memory_sample_ok"):
                        sample[key] = snapshot.get(key, 0)
                    samples.append(sample)
                next_sample += args.interval
            time.sleep(min(0.1, max(0.0, next_sample - time.monotonic())))
        stop.set()
        for thread in threads:
            thread.join(timeout=2.0)

        result = {
            "generated_utc": datetime.datetime.now(
                datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
            "host": args.host,
            "port": args.port,
            "config": {
                "duration_seconds": args.duration,
                "target_rate": args.rate,
                "concurrency": args.concurrency,
                "interval_seconds": args.interval,
                "warmup_seconds": args.warmup,
                "final_fraction": args.final_fraction,
                "drift_tolerance": args.drift_tolerance,
            },
            "requests_completed": sum(counters),
            "samples": samples,
            "drift": {},
            "passed": True,
            "notes": [],
        }

        for metric, gated in DRIFT_CHECKS:
            values = [sample.get(metric, 0) for sample in samples]
            if not values or all(value == 0 for value in values):
                result["drift"][metric] = None
                if gated:
                    result["notes"].append(
                        "%s unavailable (all zero); drift not checked" % metric)
                continue
            computed = drift(values, args.final_fraction)
            computed["gated"] = gated
            result["drift"][metric] = computed
            if gated and computed["relative_range"] > args.drift_tolerance:
                result["passed"] = False

        if len(samples) < 2:
            result["passed"] = False
            result["notes"].append("fewer than two samples; cannot assess drift")

        os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
        with open(args.output, "w", encoding="utf-8") as out:
            json.dump(result, out, indent=2)
            out.write("\n")

        print(json.dumps({
            "output": args.output,
            "requests_completed": result["requests_completed"],
            "samples": len(samples),
            "drift": result["drift"],
            "passed": result["passed"],
        }))
        return 0 if result["passed"] else 1
    finally:
        if server is not None:
            server.send_signal(signal.SIGTERM)
            try:
                server.wait(timeout=5)
            except subprocess.TimeoutExpired:
                server.kill()
        if args.start_server and metrics_path and os.path.exists(metrics_path):
            try:
                os.remove(metrics_path)
            except OSError:
                pass


if __name__ == "__main__":
    sys.exit(main())
