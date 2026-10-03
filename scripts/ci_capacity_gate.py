#!/usr/bin/env python3
"""CI capacity smoke gate.

Runs the deterministic keep-alive scenario at a fixed, low offered rate and
fails when the host-independent ``hardware_agnostic_rps`` drops below a
checked-in floor. The floor lives in ``benchmarks/ci_baseline.json`` so raising
it is a reviewed change; the measurement is written to
``benchmarks/ci_capacity_gate.json``.

``hardware_agnostic_rps`` is successful requests per server CPU-second, so the
threshold stays meaningful on shared CI runners of different speed. Only the
standard library is used.
"""
import argparse
import json
import os
import re
import subprocess
import sys
import time

RPS_RE = re.compile(r"hardware_agnostic_rps=([0-9.]+)")


def load_baseline(path):
    with open(path) as handle:
        baseline = json.load(handle)
    required = ("scenario", "duration", "concurrency",
                "hardware_agnostic_rps_floor")
    missing = [key for key in required if key not in baseline]
    if missing:
        raise SystemExit("baseline %s missing keys: %s" %
                         (path, ", ".join(missing)))
    return baseline


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", default=os.getcwd())
    parser.add_argument("--baseline", default="benchmarks/ci_baseline.json")
    parser.add_argument("--output", default="benchmarks/ci_capacity_gate.json")
    args = parser.parse_args()

    root = os.path.abspath(args.repo_root)
    baseline = load_baseline(os.path.join(root, args.baseline))
    output = os.path.join(root, args.output)

    floor = float(baseline["hardware_agnostic_rps_floor"])
    command = [
        sys.executable, os.path.join(root, "scripts", "http_benchmark.py"),
        "--start-server", "--scenario", baseline["scenario"],
        "--warmup", str(baseline.get("warmup", 0)),
        "--duration", str(baseline["duration"]),
        "--concurrency", str(baseline["concurrency"]),
        "--min-hardware-agnostic-rps", str(floor),
    ]
    if baseline.get("calibrate"):
        command += ["--calibrate", str(baseline["calibrate"])]

    print("running: %s" % " ".join(command), flush=True)
    started = time.time()
    proc = subprocess.run(command, cwd=root, text=True,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    sys.stdout.write(proc.stdout)

    match = RPS_RE.search(proc.stdout)
    measured = float(match.group(1)) if match else None
    passed = (proc.returncode == 0 and measured is not None and
              measured >= floor)
    report = {
        "baseline_file": args.baseline,
        "scenario": baseline["scenario"],
        "duration_seconds": baseline["duration"],
        "concurrency": baseline["concurrency"],
        "floor_hardware_agnostic_rps": floor,
        "measured_hardware_agnostic_rps": measured,
        "harness_returncode": proc.returncode,
        "seconds": round(time.time() - started, 3),
        "passed": passed,
    }
    os.makedirs(os.path.dirname(output), exist_ok=True)
    with open(output, "w") as handle:
        json.dump(report, handle, indent=2, sort_keys=True)
        handle.write("\n")

    if not passed:
        print("FAIL: capacity smoke gate; measured=%s floor=%.3f; artifact: %s" %
              (measured, floor, output), file=sys.stderr)
        return 1
    print("PASS: capacity smoke gate; measured=%.3f floor=%.3f; artifact: %s" %
          (measured, floor, output))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
