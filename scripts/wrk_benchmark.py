#!/usr/bin/env python3
"""Drive `wrk` load tests against the C HTTP server and report best-of-N results.

This is the raw-throughput companion to the measurement-contract harness in
`scripts/http_benchmark.py`. It runs the same sweeps documented in the README
(thread count, connection count, keep-alive/close/path/gzip, and HTTP
pipelining), parses `wrk` output, and keeps the highest requests/sec run of
each configuration.

Requires `wrk` on PATH. Examples:

    # Start the server itself, run every sweep for 10s, write a CSV:
    python3 scripts/wrk_benchmark.py --start-server --sweep all \
        --duration 10 --output benchmarks/wrk_results.csv

    # Just the pipelining grid against an already-running server:
    python3 scripts/wrk_benchmark.py --sweep pipeline --repeats 2

The server must be able to read home.html/hello.html from the working
directory, so --start-server launches it from the repository root.
"""
import argparse
import csv
import os
import re
import shutil
import socket
import subprocess
import sys
import time

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

RPS = re.compile(r"Requests/sec:\s+([\d.]+)")
REQ = re.compile(r"([\d,]+) requests in ([\d.]+)s")
TRANS = re.compile(r"Transfer/sec:\s+([\d.]+)([a-zA-Z]+)")
LAT = re.compile(
    r"Latency\s+([\d.]+)([a-zA-Z]+)\s+([\d.]+)([a-zA-Z]+)\s+([\d.]+)([a-zA-Z]+)")
P50 = re.compile(r"50%\s+([\d.]+)([a-zA-Z]+)")
P99 = re.compile(r"99%\s+([\d.]+)([a-zA-Z]+)")
ERR = re.compile(r"Socket errors:\s+(.*)")

FIELDS = ["sweep", "label", "threads", "conns", "duration", "path", "headers",
          "pipeline_depth", "requests", "elapsed_s", "rps", "transfer",
          "lat_avg", "p50", "p99", "lat_max", "errors"]

THREAD_SWEEP = [1, 2, 4, 8, 16]
CONN_SWEEP = [1, 10, 50, 100, 400, 800]
PIPELINE_DEPTHS = [2, 4, 8, 16]
PIPELINE_CONNS = [100, 200, 400, 800, 1000]


def parse(out, cfg):
    row = dict(cfg)
    row["headers"] = "; ".join(cfg["headers"])
    m = RPS.search(out)
    row["rps"] = float(m.group(1)) if m else 0.0
    m = REQ.search(out)
    if m:
        row["requests"] = m.group(1).replace(",", "")
        row["elapsed_s"] = m.group(2)
    m = TRANS.search(out)
    if m:
        row["transfer"] = m.group(1) + m.group(2)
    m = LAT.search(out)
    if m:
        row["lat_avg"] = m.group(1) + m.group(2)
        row["lat_max"] = m.group(5) + m.group(6)
    m = P50.search(out)
    if m:
        row["p50"] = m.group(1) + m.group(2)
    m = P99.search(out)
    if m:
        row["p99"] = m.group(1) + m.group(2)
    m = ERR.search(out)
    if m:
        row["errors"] = m.group(1).strip()
    return row


def build_configs(args):
    """Yield (sweep, config-dict) tuples. The config carries wrk flags and the
    metadata columns that end up in the report."""
    base = {"duration": args.duration, "path": args.path, "headers": [],
            "pipeline_depth": ""}

    def cfg(sweep, label, threads, conns, **overrides):
        c = dict(base)
        c.update(sweep=sweep, label=label, threads=threads, conns=conns)
        c.update(overrides)
        return (sweep, c)

    sweeps = args.sweep
    if sweeps == "all":
        sweeps = ["threads", "connections", "settings", "pipeline"]
    else:
        sweeps = [sweeps]

    for sweep in sweeps:
        if sweep == "threads":
            for t in THREAD_SWEEP:
                yield cfg(sweep, "threads-%d" % t, t, args.conns)
        elif sweep == "connections":
            for c in CONN_SWEEP:
                threads = 1 if c == 1 else args.threads
                yield cfg(sweep, "conns-%d" % c, threads, c)
        elif sweep == "settings":
            yield cfg(sweep, "keepalive-%s" % args.path, args.threads,
                      args.conns)
            for label, path, headers, conns in [
                ("keepalive-hello", "/hello", [], args.conns),
                ("gzip-hello", "/hello", ["Accept-Encoding: gzip"],
                 args.conns),
                ("close-home", "/home", ["Connection: close"], args.conns),
                ("close-home-400", "/home", ["Connection: close"], 400),
                ("close-gzip-hello", "/hello",
                 ["Connection: close", "Accept-Encoding: gzip"], args.conns),
            ]:
                yield cfg(sweep, label, args.threads, conns, path=path,
                          headers=headers)
        elif sweep == "pipeline":
            for depth in PIPELINE_DEPTHS:
                for conns in PIPELINE_CONNS:
                    yield cfg(sweep, "pipe-d%d-c%d" % (depth, conns),
                              args.threads, conns, pipeline_depth=depth)
        else:
            raise SystemExit("unknown sweep: %s" % sweep)


def run_wrk(args, cfg):
    cmd = [args.wrk, "-t%d" % cfg["threads"], "-c%d" % cfg["conns"],
           "-d%ds" % cfg["duration"], "--latency"]
    for header in cfg["headers"]:
        cmd += ["-H", header]
    if cfg["pipeline_depth"]:
        cmd += ["-s", args.pipeline_script]
    cmd.append(args.url + cfg["path"])
    if cfg["pipeline_depth"]:
        cmd.append(str(cfg["pipeline_depth"]))
    proc = subprocess.run(cmd, capture_output=True, text=True,
                          timeout=cfg["duration"] + 60)
    return cmd, proc.stdout + proc.stderr


def wait_for_port(host, port, timeout=5.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        with socket.socket() as s:
            s.settimeout(0.2)
            if s.connect_ex((host, port)) == 0:
                return True
        time.sleep(0.05)
    return False


def start_server(server, host, port):
    proc = subprocess.Popen([server], cwd=REPO_ROOT,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True)
    if not wait_for_port(host, port):
        proc.terminate()
        out = proc.communicate(timeout=5)[0]
        raise SystemExit("server did not become ready on %s:%d\n%s"
                         % (host, port, out))
    return proc


def main(argv=None):
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--sweep",
                        choices=["all", "threads", "connections",
                                 "settings", "pipeline"],
                        default="all", help="which sweep(s) to run")
    parser.add_argument("--duration", type=int, default=10,
                        help="seconds per run (default: 10)")
    parser.add_argument("--repeats", type=int, default=2,
                        help="runs per configuration; best rps is kept "
                             "(default: 2)")
    parser.add_argument("--threads", type=int, default=4,
                        help="wrk threads for fixed-thread sweeps (default: 4)")
    parser.add_argument("--conns", type=int, default=100,
                        help="wrk connections for fixed-conn sweeps "
                             "(default: 100)")
    parser.add_argument("--url", default="http://127.0.0.1:8081",
                        help="server base URL (default: http://127.0.0.1:8081)")
    parser.add_argument("--path", default="/home",
                        help="request path for non-settings sweeps "
                             "(default: /home)")
    parser.add_argument("--wrk", default="wrk", help="wrk executable")
    parser.add_argument("--pipeline-script",
                        default=os.path.join(REPO_ROOT, "scripts",
                                             "wrk_pipeline.lua"),
                        help="Lua pipelining script")
    parser.add_argument("--server",
                        default=os.path.join(REPO_ROOT, "bin", "http_server"),
                        help="server binary used with --start-server")
    parser.add_argument("--start-server", action="store_true",
                        help="launch the server from the repo root and stop it "
                             "on exit")
    parser.add_argument("--output", default=None,
                        help="write results as CSV to this path")
    args = parser.parse_args(argv)

    if shutil.which(args.wrk) is None:
        raise SystemExit("wrk not found; install it (apt install wrk)")

    host = re.sub(r"^\w+://", "", args.url).split("/")[0].split(":")[0]
    port = int(re.sub(r"^\w+://", "", args.url).split("/")[0].split(":")[1])
    server_proc = start_server(args.server, host, port) if args.start_server \
        else None

    rows = []
    try:
        for sweep, cfg in build_configs(args):
            best = None
            tree = "[%s] %s" % (sweep, cfg["label"])
            for i in range(args.repeats):
                cmd, out = run_wrk(args, cfg)
                row = parse(out, cfg)
                print("%-22s run %d/%d  rps=%.0f  p50=%s  p99=%s  %s"
                      % (tree, i + 1, args.repeats, row["rps"],
                         row.get("p50", ""), row.get("p99", ""),
                         row.get("errors", "")))
                if best is None or row["rps"] > best["rps"]:
                    best = row
            rows.append(best)
    finally:
        if server_proc is not None:
            server_proc.terminate()
            try:
                server_proc.communicate(timeout=5)
            except subprocess.TimeoutExpired:
                server_proc.kill()

    print("\n=== best-of-%d results ===" % args.repeats)
    header = "%-8s %-18s %3s %5s %12s %10s %10s" % (
        "sweep", "label", "t", "c", "rps", "p50", "p99")
    print(header)
    print("-" * len(header))
    for row in rows:
        print("%-8s %-18s %3d %5d %12.0f %10s %10s"
              % (row["sweep"], row["label"], row["threads"], row["conns"],
                 row["rps"], row.get("p50", ""), row.get("p99", "")))

    if args.output:
        with open(args.output, "w", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=FIELDS, delimiter=",",
                                    extrasaction="ignore")
            writer.writeheader()
            for row in rows:
                writer.writerow(row)
        print("\nWrote %d rows to %s" % (len(rows), args.output))
    return 0


if __name__ == "__main__":
    sys.exit(main())
