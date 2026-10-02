#!/usr/bin/env python3
"""E2E acceptance for Phase 4c OS sandboxing and Phase 4d limits.

The harness starts real server processes, drives sockets, inspects their Linux
process state, and writes a reproducible JSON artifact. The systemd unit is
checked statically because installing/starting a system service is not safe for
an application test harness.
"""
import argparse
import json
import os
import re
import socket
import subprocess
import sys
import time


def free_port():
    sock = socket.socket()
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
    sock.close()
    return port


def wait_port(proc, port):
    deadline = time.time() + 8
    while time.time() < deadline:
        if proc.poll() is not None:
            output = proc.stdout.read() if proc.stdout else ""
            raise RuntimeError("server exited early: %s" % output[-2000:])
        try:
            sock = socket.create_connection(("127.0.0.1", port), 0.25)
            sock.close()
            return
        except OSError:
            time.sleep(0.05)
    raise RuntimeError("server did not listen on port %d" % port)


def start_server(root, options):
    port = free_port()
    env = dict(os.environ)
    env["HTTP_SERVER_ACCESS_LOG"] = "0"
    argv = [os.path.join(root, "bin", "http_server"), "--log-level", "error",
            "--port", str(port)] + options
    proc = subprocess.Popen(argv, cwd=root, env=env, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True)
    wait_port(proc, port)
    return proc, port


def stop_server(proc):
    if proc.poll() is None:
        proc.terminate()
    try:
        proc.wait(timeout=8)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=3)


def request(port, payload=b"GET /hello HTTP/1.1\r\nHost: localhost\r\n"
                         b"Connection: close\r\n\r\n"):
    sock = socket.create_connection(("127.0.0.1", port), 3)
    sock.sendall(payload)
    chunks = []
    while True:
        data = sock.recv(4096)
        if not data:
            break
        chunks.append(data)
    sock.close()
    return b"".join(chunks)


def proc_status(pid):
    values = {}
    with open("/proc/%d/status" % pid) as status:
        for line in status:
            key, _, value = line.partition(":")
            if key in ("NoNewPrivs", "Seccomp"):
                values[key] = value.strip()
    return values


def core_limit(pid):
    with open("/proc/%d/limits" % pid) as limits:
        for line in limits:
            if line.startswith("Max core file size"):
                return re.findall(r"(?:unlimited|\d+)", line)[-2:]
    return []


def limit_value(pid, label):
    with open("/proc/%d/limits" % pid) as limits:
        for line in limits:
            if line.startswith(label):
                return re.findall(r"(?:unlimited|\d+)", line)[-2:]
    return []


def sandbox_case(root, name, options, failures, report):
    proc = None
    detail = {"options": options}
    try:
        proc, port = start_server(root, options)
        deadline = time.time() + 5
        detail["status"] = proc_status(proc.pid)
        while time.time() < deadline:
            if (("--seccomp" not in options or
                 detail["status"].get("Seccomp") == "2") and
                    detail["status"].get("NoNewPrivs") == "1"):
                break
            time.sleep(0.05)
            detail["status"] = proc_status(proc.pid)
        detail["http_status"] = request(port).split(b" ", 2)[1].decode()
        if detail["http_status"] != "200":
            failures.append("%s: sandboxed request was not 200" % name)
        if detail["status"].get("NoNewPrivs") != "1":
            failures.append("%s: NoNewPrivs is not 1" % name)
        if "--seccomp" in options and detail["status"].get("Seccomp") != "2":
            failures.append("%s: Seccomp is not filter mode 2" % name)
    except Exception as exc:  # noqa: BLE001
        failures.append("%s: %s" % (name, exc))
    finally:
        if proc is not None:
            stop_server(proc)
    report[name] = detail


def per_ip_case(root, failures, report):
    proc = None
    first = second = None
    detail = {"limit": 1}
    try:
        proc, port = start_server(root, ["--per-ip-connections", "1",
                                         "--max-connections", "8"])
        first = socket.create_connection(("127.0.0.1", port), 3)
        time.sleep(0.25)
        second = socket.create_connection(("127.0.0.1", port), 3)
        second.settimeout(3)
        data = second.recv(4096)
        detail["second_response"] = data[:64].decode(errors="replace")
        if data and not data.startswith(b"HTTP/1.1 503"):
            failures.append("per_ip_connections: second peer was not rejected")
    except Exception as exc:  # noqa: BLE001
        failures.append("per_ip_connections: %s" % exc)
    finally:
        for sock in (first, second):
            if sock is not None:
                sock.close()
        if proc is not None:
            stop_server(proc)
    report["per_ip_connections"] = detail


def slowloris_case(root, failures, report):
    proc = None
    sock = None
    detail = {"progress_timeout": 1}
    try:
        proc, port = start_server(root, ["--header-progress-timeout", "1",
                                         "--header-read-timeout", "5"])
        sock = socket.create_connection(("127.0.0.1", port), 3)
        sock.sendall(b"GET /hello HTTP/1.1\r\nHost: localhost\r\n")
        time.sleep(1.7)
        sock.settimeout(2)
        data = sock.recv(1)
        detail["closed"] = data == b""
        if data != b"":
            failures.append("slowloris: connection was not closed after progress timeout")
    except Exception as exc:  # noqa: BLE001
        failures.append("slowloris: %s" % exc)
    finally:
        if sock is not None:
            sock.close()
        if proc is not None:
            stop_server(proc)
    report["slowloris"] = detail


def backpressure_case(root, failures, report):
    proc = None
    detail = {"pipeline_depth": 8}
    try:
        proc, port = start_server(root, ["--max-keepalive-requests", "16"])
        requests = []
        for index in range(8):
            connection = "close" if index == 7 else "keep-alive"
            requests.append(("GET /hello HTTP/1.1\r\nHost: localhost\r\n"
                             "Connection: %s\r\n\r\n" % connection).encode())
        response = request(port, b"".join(requests))
        detail["responses"] = response.count(b"HTTP/1.1 200")
        if detail["responses"] != 8:
            failures.append("backpressure: expected 8 pipelined responses")
    except Exception as exc:  # noqa: BLE001
        failures.append("backpressure: %s" % exc)
    finally:
        if proc is not None:
            stop_server(proc)
    report["backpressure"] = detail


def resource_case(root, failures, report):
    proc = None
    detail = {"rlimit_nproc": 4096}
    try:
        proc, port = start_server(root, ["--rlimit-nproc", "4096"])
        detail["core_limit"] = core_limit(proc.pid)
        detail["nproc_limit"] = limit_value(proc.pid, "Max processes")
        detail["http_status"] = request(port).split(b" ", 2)[1].decode()
        if detail["core_limit"] != ["0", "0"]:
            failures.append("rlimits: Max core file size is not 0/0")
        if detail["nproc_limit"] != ["4096", "4096"]:
            failures.append("rlimits: Max processes is not 4096/4096")
        if detail["http_status"] != "200":
            failures.append("rlimits: request failed")
    except Exception as exc:  # noqa: BLE001
        failures.append("rlimits: %s" % exc)
    finally:
        if proc is not None:
            stop_server(proc)
    report["rlimits"] = detail


def systemd_case(root, failures, report):
    path = os.path.join(root, "deploy", "http-server.service")
    required = {
        "NoNewPrivileges=true", "ProtectSystem=strict", "ProtectHome=true",
        "PrivateTmp=true", "RestrictAddressFamilies=AF_INET AF_INET6 AF_UNIX",
        "SystemCallFilter=", "CapabilityBoundingSet=CAP_NET_BIND_SERVICE",
        "LimitCORE=0", "LimitNOFILE="
    }
    text = ""
    if os.path.exists(path):
        with open(path) as unit:
            text = unit.read()
    missing = sorted(item for item in required if item not in text)
    report["systemd_unit"] = {"path": path, "missing": missing,
                               "static_valid": not missing}
    if missing:
        failures.append("systemd unit missing: %s" % ", ".join(missing))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", default=os.getcwd())
    parser.add_argument("--output",
                        default="benchmarks/production_phase4_sandbox_resources.json")
    args = parser.parse_args()
    root = os.path.abspath(args.repo_root)
    output = args.output if os.path.isabs(args.output) else os.path.join(root, args.output)
    report = {"platform": sys.platform, "failures": []}
    failures = report["failures"]
    binary = os.path.join(root, "bin", "http_server")
    if not os.path.exists(binary):
        failures.append("binary not built: %s" % binary)
    else:
        sandbox_case(root, "landlock", ["--landlock", "1"], failures, report)
        sandbox_case(root, "seccomp", ["--seccomp", "1"], failures, report)
        sandbox_case(root, "landlock_seccomp", ["--landlock", "1",
                     "--seccomp", "1"], failures, report)
        per_ip_case(root, failures, report)
        slowloris_case(root, failures, report)
        backpressure_case(root, failures, report)
        resource_case(root, failures, report)
    systemd_case(root, failures, report)
    report["passed"] = not failures
    os.makedirs(os.path.dirname(output), exist_ok=True)
    with open(output, "w") as artifact:
        json.dump(report, artifact, indent=2, sort_keys=True)
        artifact.write("\n")
    if failures:
        print("FAIL: Phase 4 sandbox/resource acceptance")
        for failure in failures:
            print("  - %s" % failure)
        print("artifact: %s" % output)
        return 1
    print("PASS: Phase 4 sandbox/resource acceptance (artifact: %s)" % output)
    return 0


if __name__ == "__main__":
    sys.exit(main())
