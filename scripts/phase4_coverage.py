#!/usr/bin/env python3
"""Measure and enforce Phase 4 gcov coverage for critical modules."""
import argparse
import json
import os
import re
import signal
import socket
import subprocess
import tempfile
import time


TARGETS = {
    "parser": "src/http_server.c",
    "path_resolver": "src/path_resolver.c",
    "connection_state": "src/event_loop.c",
}


def run(command, cwd, timeout=180):
    result = subprocess.run(command, cwd=cwd, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            timeout=timeout)
    if result.returncode != 0:
        raise RuntimeError("command failed (%d): %s\n%s" %
                           (result.returncode, " ".join(command), result.stdout[-4000:]))
    return result.stdout


def wait_for_server(process, port):
    deadline = time.time() + 15
    while time.time() < deadline:
        if process.poll() is not None:
            raise RuntimeError("coverage server exited before readiness")
        try:
            sock = socket.create_connection(("127.0.0.1", port), 0.25)
            sock.close()
            return
        except OSError:
            time.sleep(0.05)
    raise RuntimeError("coverage server did not become ready")


def run_server_suite(root):
    server = os.path.join(root, "bin", "http_server")
    process = subprocess.Popen([server, "--document-root", "."], cwd=root,
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               text=True)
    try:
        wait_for_server(process, 8081)
        run([os.path.join(root, "bin", "run_tests"), "server"], root)
    finally:
        if process.poll() is None:
            process.send_signal(signal.SIGTERM)
        try:
            process.wait(timeout=15)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)


def parse_gcov(source, object_dir, output_dir):
    os.makedirs(output_dir, exist_ok=True)
    data_file = os.path.join(object_dir, os.path.basename(source) + ".gcda")
    stdout = run(["gcov", "-b", "-c", "-p", data_file], output_dir)
    lines = {}
    functions = {}
    current_function = None
    for line in stdout.splitlines():
        match = re.match(r"Function '([^']+)'", line)
        if match:
            current_function = match.group(1)
            functions.setdefault(current_function, False)
            continue
        match = re.search(r"Lines executed:([0-9.]+)%", line)
        if match and current_function is not None:
            functions[current_function] |= float(match.group(1)) > 0.0

    for gcov_file in os.listdir(output_dir):
        if not gcov_file.endswith(".gcov"):
            continue
        with open(os.path.join(output_dir, gcov_file), errors="replace") as stream:
            for record in stream:
                function_match = re.match(
                    r"function (.+) called (\d+)", record.strip())
                if function_match:
                    functions[function_match.group(1)] = int(function_match.group(2)) > 0
                    continue
                fields = record.split(":", 2)
                if len(fields) != 3:
                    continue
                count = fields[0].strip()
                try:
                    line_number = int(fields[1].strip())
                except ValueError:
                    continue
                if line_number <= 0 or count == "-":
                    continue
                executed = count.isdigit() and int(count) > 0
                lines[line_number] = lines.get(line_number, False) or executed
    return lines, functions


def object_dirs(build_dir):
    result = []
    for root, dirs, files in os.walk(build_dir):
        if any(name.endswith(".gcda") for name in files):
            result.append(root)
    return result


def collect_coverage(root, build_dir):
    objects = object_dirs(build_dir)
    if not objects:
        raise RuntimeError("no gcda files found under %s" % build_dir)
    report = {}
    with tempfile.TemporaryDirectory(prefix="phase4-gcov-") as output:
        for name, relative in TARGETS.items():
            merged_lines = {}
            merged_functions = {}
            source = os.path.join(root, relative)
            for index, object_dir in enumerate(objects):
                try:
                    lines, functions = parse_gcov(
                        source, object_dir, os.path.join(output, name + str(index)))
                except RuntimeError:
                    continue
                for line_number, executed in lines.items():
                    merged_lines[line_number] = merged_lines.get(line_number, False) or executed
                for function, executed in functions.items():
                    merged_functions[function] = (merged_functions.get(function, False) or
                                                  executed)
            line_total = len(merged_lines)
            function_total = len(merged_functions)
            report[name] = {
                "source": relative,
                "lines_total": line_total,
                "lines_covered": sum(merged_lines.values()),
                "functions_total": function_total,
                "functions_covered": sum(merged_functions.values()),
            }
            report[name]["line_percent"] = (
                100.0 * report[name]["lines_covered"] / line_total
                if line_total else 0.0)
            report[name]["function_percent"] = (
                100.0 * report[name]["functions_covered"] / function_total
                if function_total else 0.0)
    return report


def reset_coverage(build_dir):
    for root, _, files in os.walk(build_dir):
        for name in files:
            if name.endswith(".gcda"):
                os.remove(os.path.join(root, name))


def enforce(report, baseline):
    failures = []
    for name, expected in baseline["targets"].items():
        actual = report.get(name)
        if actual is None:
            failures.append("missing coverage target: %s" % name)
            continue
        for metric in ("line_percent", "function_percent"):
            if actual[metric] + 1e-9 < expected[metric]:
                failures.append("%s %s %.2f%% below baseline %.2f%%" %
                                (name, metric, actual[metric], expected[metric]))
    return failures


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", default=os.getcwd())
    parser.add_argument("--build-dir", required=True)
    parser.add_argument("--baseline", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    root = os.path.abspath(args.repo_root)
    build_dir = os.path.abspath(args.build_dir)
    baseline_path = os.path.abspath(args.baseline)
    output_path = os.path.abspath(args.output)
    failures = []
    report = {"targets": {}, "failures": failures}
    try:
        reset_coverage(build_dir)
        run([os.path.join(root, "bin", "run_tests"), "ring"], root)
        run([os.path.join(root, "bin", "run_tests"), "tls"], root)
        run_server_suite(root)
        report["targets"] = collect_coverage(root, build_dir)
        with open(baseline_path) as stream:
            baseline = json.load(stream)
        failures.extend(enforce(report["targets"], baseline))
    except (OSError, RuntimeError, ValueError, json.JSONDecodeError) as exc:
        failures.append(str(exc))
    report["passed"] = not failures
    os.makedirs(os.path.dirname(output_path), exist_ok=True)
    with open(output_path, "w") as stream:
        json.dump(report, stream, indent=2, sort_keys=True)
        stream.write("\n")
    if failures:
        print("FAIL: Phase 4 coverage")
        for failure in failures:
            print("  - %s" % failure)
        print("artifact: %s" % output_path)
        return 1
    print("PASS: Phase 4 coverage (artifact: %s)" % output_path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
