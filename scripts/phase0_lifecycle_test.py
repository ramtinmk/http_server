#!/usr/bin/env python3
"""Phase 0 lifecycle and configuration end-to-end acceptance.

Verifies the operational-safety work items that do not need a load generator:

  1. Invalid configuration exits non-zero and names the exact offending key,
     for an environment variable, a command-line flag, and a config file.
  2. Runtime keep-alive tuning takes effect: a connection is force-closed
     after the configured number of requests.
  3. SIGTERM under a burst of in-flight pipelined requests drains within the
     configured deadline, delivers every response without truncation, and
     exits with status 0.

Writes a repeatable JSON artifact to
benchmarks/production_phase0_lifecycle.json and exits non-zero on failure.
"""

import argparse
import json
import os
import signal
import socket
import subprocess
import sys
import time

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_PORT = 8081
ARTIFACT = os.path.join(REPO_ROOT, "benchmarks",
                        "production_phase0_lifecycle.json")
REQUEST = b"GET /home HTTP/1.1\r\nHost: lifecycle\r\n\r\n"


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


class ResponseReader:
    """Reads one Content-Length-framed response at a time, preserving any
    pipelined bytes that arrive coalesced in a single recv()."""

    def __init__(self, sock):
        self.sock = sock
        self.buf = b""

    def read(self, timeout=20.0):
        """Return the status int, or None on EOF/reset."""
        self.sock.settimeout(timeout)
        while True:
            marker = self.buf.find(b"\r\n\r\n")
            if marker != -1:
                head = self.buf[:marker]
                content_length = 0
                for line in head.split(b"\r\n")[1:]:
                    if line.lower().startswith(b"content-length:"):
                        content_length = int(line.split(b":", 1)[1].strip())
                        break
                end = marker + 4 + content_length
                if len(self.buf) >= end:
                    self.buf = self.buf[end:]
                    try:
                        return int(head.split(b"\r\n", 1)[0].split()[1])
                    except (IndexError, ValueError):
                        return None
            try:
                chunk = self.sock.recv(8192)
            except OSError:
                return None
            if not chunk:
                return None
            self.buf += chunk


class Server:
    def __init__(self, args, env):
        merged = dict(os.environ)
        merged.update(env)
        self.proc = subprocess.Popen(
            [args.server], cwd=REPO_ROOT, env=merged,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)

    def stop(self, timeout=20.0):
        if self.proc.poll() is None:
            self.proc.send_signal(signal.SIGTERM)
        try:
            self.proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()
        return self.proc.returncode


def check_invalid_config(args, checks, failures):
    """Invalid values must exit non-zero and name the offending key."""
    good = dict(os.environ)
    # Keep the FD preflight satisfied even if the host default is low.
    cases = [
        ("environment", {"HTTP_SERVER_IDLE_TIMEOUT": "0"}, "idle_timeout"),
        ("command-line", {"HTTP_SERVER_IDLE_TIMEOUT": "30"}, "idle_timeout",
         ["--idle-timeout", "-3"]),
    ]
    for case in cases:
        label = case[0]
        env = dict(good)
        env.update(case[1])
        cmd = [args.server] + (case[3] if len(case) > 3 else [])
        proc = subprocess.run(cmd, cwd=REPO_ROOT, env=env,
                              capture_output=True, text=True, timeout=15)
        output = proc.stdout + proc.stderr
        named = case[2] in output
        ok = proc.returncode != 0 and named
        checks["invalid_config_" + label] = {
            "exit_code": proc.returncode,
            "key_named": named,
            "output": output.strip().splitlines()[-1:] or [""],
        }
        if not ok:
            failures.append("invalid config (%s) exit=%d key_named=%s" % (
                label, proc.returncode, named))

    bad_file = os.path.join(REPO_ROOT, "benchmarks",
                            ".phase0_bad_config.conf")
    with open(bad_file, "w", encoding="utf-8") as out:
        out.write("port = 9099\nnonsense_key = 1\n")
    try:
        proc = subprocess.run([args.server, "--config", bad_file],
                              cwd=REPO_ROOT, env=good, capture_output=True,
                              text=True, timeout=15)
        output = proc.stdout + proc.stderr
        named = "nonsense_key" in output
        checks["invalid_config_file"] = {
            "exit_code": proc.returncode,
            "key_named": named,
            "output": output.strip().splitlines()[-1:] or [""],
        }
        if proc.returncode == 0 or not named:
            failures.append("invalid config file exit=%d key_named=%s" % (
                proc.returncode, named))
    finally:
        os.remove(bad_file)


def check_runtime_keepalive(args, checks, failures):
    """A lowered keep-alive limit closes the connection after N requests."""
    limit = 2
    server = Server(args, {
        "HTTP_SERVER_PORT": str(args.port),
        "HTTP_SERVER_MAX_KEEPALIVE_REQUESTS": str(limit),
    })
    try:
        if not wait_for_port(args.host, args.port):
            failures.append("keepalive server did not start")
            checks["runtime_keepalive"] = {"error": "no start"}
            return
        served = 0
        closed = False
        try:
            with socket.create_connection((args.host, args.port), 3) as sock:
                reader = ResponseReader(sock)
                for _ in range(limit + 1):
                    sock.sendall(REQUEST)
                    status = reader.read(timeout=5)
                    if status is None:
                        closed = True
                        break
                    if status != 200:
                        failures.append(
                            "keepalive runtime returned status %d" % status)
                    served += 1
        except OSError:
            closed = True
        checks["runtime_keepalive"] = {
            "configured_limit": limit,
            "responses_served": served,
            "closed_after_limit": closed,
        }
        if served != limit:
            failures.append("runtime keepalive served %d != %d" % (served, limit))
        if not closed:
            failures.append("runtime keepalive did not close after the limit")
    finally:
        server.stop()


def check_drain(args, checks, failures):
    """SIGTERM under in-flight pipelined load must deliver every response."""
    conns = args.conns
    pipeline = args.pipeline
    metrics_file = os.path.join(REPO_ROOT, "benchmarks",
                                ".phase0_lifecycle_metrics.json")
    if os.path.exists(metrics_file):
        os.remove(metrics_file)

    server = Server(args, {
        "HTTP_SERVER_PORT": str(args.port),
        "HTTP_SERVER_MAX_KEEPALIVE_REQUESTS": "1000",
        "HTTP_SERVER_METRICS_FILE": metrics_file,
        "HTTP_SERVER_SHUTDOWN_DRAIN_TIMEOUT": str(args.drain_timeout),
    })
    result = {"connections": conns, "pipeline_depth": pipeline}
    try:
        if not wait_for_port(args.host, args.port):
            failures.append("drain server did not start")
            return

        sockets = [socket.create_connection((args.host, args.port), 3)
                   for _ in range(conns)]
        try:
            for sock in sockets:
                sock.sendall(REQUEST * pipeline)

            # Wait until the server has accepted every connection so the
            # requests are in flight before the signal.
            deadline = now() + 5.0
            active = 0
            while now() < deadline:
                active = read_metrics(metrics_file).get("active_connections", 0)
                if active >= conns:
                    break
                time.sleep(0.05)
            result["active_before_signal"] = active

            started = now()
            server.proc.send_signal(signal.SIGTERM)

            complete_connections = 0
            total_responses = 0
            for sock in sockets:
                reader = ResponseReader(sock)
                got = 0
                for _ in range(pipeline):
                    status = reader.read(timeout=args.drain_timeout + 10)
                    if status is None:
                        break
                    if status != 200:
                        failures.append("drain returned status %d" % status)
                    got += 1
                if got == pipeline:
                    complete_connections += 1
                total_responses += got
                sock.close()

            result["drain_seconds"] = round(now() - started, 3)
            result["responses_delivered"] = total_responses
            result["connections_complete"] = complete_connections
            result["truncated_connections"] = conns - complete_connections
            if complete_connections != conns:
                failures.append("drain truncated %d of %d connections" % (
                    conns - complete_connections, conns))
            if total_responses != conns * pipeline:
                failures.append("drain delivered %d of %d responses" % (
                    total_responses, conns * pipeline))
        finally:
            for sock in sockets:
                try:
                    sock.close()
                except OSError:
                    pass

        result["exit_code"] = server.stop()
        if result["exit_code"] != 0:
            failures.append("drain exit code %d != 0" % result["exit_code"])
    finally:
        server.stop()
        if os.path.exists(metrics_file):
            os.remove(metrics_file)
    checks["drain_under_load"] = result


def check_sd_notify(args, checks, failures):
    """READY=1 after binding and STOPPING=1 on shutdown over $NOTIFY_SOCKET."""
    path = os.path.join(REPO_ROOT, "benchmarks", ".phase0_notify.sock")
    if os.path.exists(path):
        os.remove(path)

    notifier = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    notifier.bind(path)
    notifier.settimeout(5)
    server = Server(args, {
        "HTTP_SERVER_PORT": str(args.port),
        "NOTIFY_SOCKET": path,
    })
    ready = False
    stopping = False
    try:
        if not wait_for_port(args.host, args.port):
            failures.append("sd_notify server did not start")
            return
        try:
            data, _ = notifier.recvfrom(512)
            ready = b"READY=1" in data
        except socket.timeout:
            pass

        server.proc.send_signal(signal.SIGTERM)
        try:
            data, _ = notifier.recvfrom(512)
            stopping = b"STOPPING=1" in data
        except socket.timeout:
            pass
    finally:
        server.stop()
        notifier.close()
        if os.path.exists(path):
            os.remove(path)

    checks["sd_notify"] = {"ready": ready, "stopping": stopping}
    if not ready:
        failures.append("sd_notify READY=1 was not received")
    if not stopping:
        failures.append("sd_notify STOPPING=1 was not received")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--server", default=os.path.join(REPO_ROOT,
                                                         "bin/http_server"))
    parser.add_argument("--conns", type=int, default=8)
    parser.add_argument("--pipeline", type=int, default=5)
    parser.add_argument("--drain-timeout", type=int, default=10)
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

    checks = {}
    failures = []
    check_invalid_config(args, checks, failures)
    check_runtime_keepalive(args, checks, failures)
    check_drain(args, checks, failures)
    check_sd_notify(args, checks, failures)

    artifact = {"checks": checks, "success": not failures,
                "failures": failures}
    os.makedirs(os.path.dirname(args.artifact), exist_ok=True)
    with open(args.artifact, "w", encoding="utf-8") as out:
        json.dump(artifact, out, indent=2, sort_keys=True)

    for name, value in checks.items():
        print("%-24s %s" % (name, value))
    if failures:
        for failure in failures:
            print("FAIL: %s" % failure, file=sys.stderr)
        return 1
    print("PASS: phase 0 lifecycle acceptance (artifact: %s)" % args.artifact)
    return 0


if __name__ == "__main__":
    sys.exit(main())
