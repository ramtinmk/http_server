#!/usr/bin/env python3
"""Compare running HTTP servers on the deterministic file-class corpus with wrk.

This is the peer-comparison companion to `http_benchmark.py`. It drives `wrk`
against one or more already-running servers, one corpus class at a time, and
reports throughput and latency per class rather than a single average. The
corpus (see `docs/benchmarks.md` and `scripts/benchmark_corpus.py`) makes every
target serve byte-identical assets, so a class comparison is apples-to-apples.

Start the targets first, then point this script at them:

    # ours on :8081 (corpus document root), nginx on :8082
    python3 scripts/compare_servers.py \
        --corpus benchmarks/corpus \
        --target ours=http://127.0.0.1:8081 \
        --target nginx=http://127.0.0.1:8082 \
        --wrk-cpus 4-7 --output benchmarks/server_comparison.csv

Pin the load generator to CPUs disjoint from the servers (`--wrk-cpus`) and run
both servers under the same worker count and optimization level. Loopback
saturates the generator at high rates; treat a class whose client CPU is pegged
as a client-side bound, not a server result.
"""
import argparse
import csv
import json
import os
import re
import shutil
import subprocess
import sys


REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

RPS_RE = re.compile(r"Requests/sec:\s+([\d.]+)")
REQ_RE = re.compile(r"([\d,]+) requests in ([\d.]+)s")
TRANS_RE = re.compile(r"Transfer/sec:\s+([\d.]+)([a-zA-Z]+)")
LAT_RE = re.compile(
    r"Latency\s+([\d.]+)([a-zA-Z]+)\s+([\d.]+)([a-zA-Z]+)\s+([\d.]+)([a-zA-Z]+)")
P50_RE = re.compile(r"50%\s+([\d.]+)([a-zA-Z]+)")
P99_RE = re.compile(r"99%\s+([\d.]+)([a-zA-Z]+)")
MAX_RE = re.compile(r"100%\s+([\d.]+)([a-zA-Z]+)")
ERR_RE = re.compile(r"Socket errors:\s+(.*)")
NON2XX_RE = re.compile(r"Non-2xx or 3xx responses:\s+(\d+)")

# Canonical class order for stable reports; anything else sorts after.
CLASS_ORDER = ("tiny", "small", "medium", "binary", "streamed", "large", "huge")

FIELDS = [
    "mode", "target", "class", "path", "gzip", "threads", "connections",
    "duration", "repeat", "requests", "elapsed_s", "rps", "transfer_per_sec",
    "p50", "p99", "max", "socket_errors", "non2xx",
]


def parse_target(spec):
    if "=" not in spec:
        raise argparse.ArgumentTypeError(
            "target must be NAME=BASEURL, got %r" % spec)
    name, url = spec.split("=", 1)
    if not name or not url:
        raise argparse.ArgumentTypeError(
            "target must be NAME=BASEURL, got %r" % spec)
    return name, url.rstrip("/")


def load_classes(corpus_dir, only=None):
    """Return [(class, path)] from the manifest, first entry per class."""
    with open(os.path.join(corpus_dir, "manifest.json"), encoding="utf-8") as f:
        manifest = json.load(f)
    seen = {}
    for entry in manifest["entries"]:
        seen.setdefault(entry["class"], entry["path"])
    classes = sorted(seen, key=lambda c: (CLASS_ORDER.index(c)
                                          if c in CLASS_ORDER else 99, c))
    if only:
        classes = [c for c in classes if c in only]
    return [(c, seen[c]) for c in classes]


def parse_wrk(out):
    row = {"requests": "", "elapsed_s": "", "rps": 0.0,
           "transfer_per_sec": "", "p50": "", "p99": "", "max": "",
           "socket_errors": "", "non2xx": ""}
    m = RPS_RE.search(out)
    if m:
        row["rps"] = float(m.group(1))
    m = REQ_RE.search(out)
    if m:
        row["requests"] = m.group(1).replace(",", "")
        row["elapsed_s"] = m.group(2)
    m = TRANS_RE.search(out)
    if m:
        row["transfer_per_sec"] = m.group(1) + m.group(2)
    m = P50_RE.search(out)
    if m:
        row["p50"] = m.group(1) + m.group(2)
    m = P99_RE.search(out)
    if m:
        row["p99"] = m.group(1) + m.group(2)
    m = MAX_RE.search(out)
    if m:
        row["max"] = m.group(1) + m.group(2)
    m = ERR_RE.search(out)
    if m:
        row["socket_errors"] = m.group(1).strip()
    m = NON2XX_RE.search(out)
    if m:
        row["non2xx"] = m.group(1)
    return row


def run_wrk(args, base_url, path, gzip):
    cmd = [args.wrk, "-t%d" % args.threads, "-c%d" % args.connections,
           "-d%ds" % args.duration, "--latency"]
    if gzip:
        cmd += ["-H", "Accept-Encoding: gzip"]
    if args.wrk_cpus:
        cmd = ["taskset", "-c", args.wrk_cpus] + cmd
    cmd.append(base_url + path)
    proc = subprocess.run(cmd, capture_output=True, text=True,
                          timeout=args.duration + 60)
    return proc.stdout + proc.stderr


def main(argv=None):
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--corpus", default=os.path.join(REPO_ROOT, "benchmarks", "corpus"),
                        help="corpus directory containing manifest.json")
    parser.add_argument("--target", action="append", type=parse_target,
                        required=True, metavar="NAME=URL",
                        help="a running server (repeatable); the first is the "
                             "baseline for the ratio column")
    parser.add_argument("--class", dest="classes", action="append",
                        help="restrict to this class (repeatable)")
    parser.add_argument("--gzip", action="store_true",
                        help="send Accept-Encoding: gzip on every class")
    parser.add_argument("--wrk", default="wrk", help="wrk executable")
    parser.add_argument("--threads", type=int, default=4)
    parser.add_argument("--connections", type=int, default=100)
    parser.add_argument("--duration", type=int, default=10)
    parser.add_argument("--repeats", type=int, default=2,
                        help="runs per class; the highest rps is reported")
    parser.add_argument("--mode", default="",
                        help="label stamped into the mode column (e.g. 'tls')")
    parser.add_argument("--wrk-cpus", default=None,
                        help="taskset list to pin wrk, e.g. 4-7")
    parser.add_argument("--output", default=None, help="write all runs as CSV")
    parser.add_argument("--append", action="store_true",
                        help="append to --output, writing the header only once")
    args = parser.parse_args(argv)

    if shutil.which(args.wrk) is None:
        raise SystemExit("wrk not found on PATH")
    if args.wrk_cpus and shutil.which("taskset") is None:
        raise SystemExit("--wrk-cpus requires taskset")

    classes = load_classes(args.corpus, args.classes)
    targets = args.target
    rows = []
    best = {}

    for class_name, path in classes:
        for target_name, base_url in targets:
            winner = None
            for repeat in range(1, args.repeats + 1):
                out = run_wrk(args, base_url, path, args.gzip)
                parsed = parse_wrk(out)
                row = {"target": target_name, "class": class_name,
                       "path": path, "gzip": int(bool(args.gzip)),
                       "mode": args.mode,
                       "threads": args.threads,
                       "connections": args.connections,
                       "duration": args.duration, "repeat": repeat}
                row.update(parsed)
                rows.append(row)
                print("%-7s %-9s run %d/%d rps=%-10.0f p50=%-8s p99=%-8s %s"
                      % (target_name, class_name, repeat, args.repeats,
                         row["rps"], row["p50"], row["p99"],
                         row["socket_errors"] or row["non2xx"]))
                if winner is None or row["rps"] > winner["rps"]:
                    winner = row
            best[(class_name, target_name)] = winner

    print("\n=== best-of-%d ===" % args.repeats)
    header = "%-9s %-10s %14s %10s %10s %8s" % (
        "class", "target", "rps", "p50", "p99", "vs base")
    print(header)
    print("-" * len(header))
    baseline_name = targets[0][0]
    for class_name, _ in classes:
        base = best.get((class_name, baseline_name))
        for target_name, _ in targets:
            row = best[(class_name, target_name)]
            ratio = ""
            if base and row is not base and base["rps"] > 0:
                ratio = "%.3f" % (row["rps"] / base["rps"])
            print("%-9s %-10s %14.0f %10s %10s %8s"
                  % (class_name, target_name, row["rps"], row["p50"],
                     row["p99"], ratio))

    if args.output:
        append = args.append and os.path.exists(args.output)
        with open(args.output, "a" if append else "w", newline="",
                  encoding="utf-8") as f:
            writer = csv.DictWriter(f, fieldnames=FIELDS, extrasaction="ignore")
            if not append:
                writer.writeheader()
            writer.writerows(rows)
        print("\n%s %d rows to %s"
              % ("appended" if append else "wrote", len(rows), args.output))
    return 0


if __name__ == "__main__":
    sys.exit(main())
