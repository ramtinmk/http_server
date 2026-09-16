#!/usr/bin/env python3
"""Dependency-free HTTP load generator implementing the measurement contract in
docs/scaling-plan.md.

The client validates HTTP framing (Content-Length or chunked encoding) instead
of treating an idle socket timeout as response completion. It records offered
load, completed and failed requests, status distribution, latency percentiles,
connection and keep-alive request rates, and (when it owns or is told about the
server process) CPU, RSS, open file descriptors, context switches, server
thread-pool queue depth, active workers, and rejected tasks.

Run the scenario matrix described by the plan through --scenario, or drive a
custom run with --rate/--duration/--keep-alive/--path. Results are written to
CSV or JSON Lines while human-readable output is retained.
"""

import argparse
import collections
import csv
import json
import math
import os
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time


CSV_FIELDS = (
    "timestamp_utc", "commit_id", "scenario", "host", "port", "mode",
    "target_rate", "duration_seconds", "concurrency", "requests_planned",
    "responses_received", "successful", "failed", "throughput_rps",
    "successful_rps", "offered_rps", "connection_rate", "keepalive_request_rate",
    "requests_per_connection", "status_200", "status_400", "status_404",
    "status_413", "status_431", "status_500", "status_501", "status_other",
    "latency_min_ms", "latency_p50_ms", "latency_p95_ms", "latency_p99_ms",
    "latency_max_ms", "fail_connect", "fail_send", "fail_receive",
    "fail_framing", "fail_status", "fail_timeout", "fail_other",
    "server_cpu_percent", "server_rss_kb", "server_open_fds",
    "server_active_workers_max", "server_queue_depth_max",
    "server_accepted_connections", "server_rejected_tasks",
    "server_completed_requests", "server_request_failures",
    "ctx_switches_voluntary", "ctx_switches_involuntary", "tcp_retransmits",
)

SCENARIOS = {
    "new-connection-1000": {
        "rate": 1000.0,
        "duration": 10.0,
        "keep_alive": False,
        "specs": [{"path": "/home"}],
    },
    "new-connection-5000": {
        "rate": 5000.0,
        "duration": 10.0,
        "keep_alive": False,
        "specs": [{"path": "/home"}],
    },
    "keep-alive-5000": {
        "rate": 5000.0,
        "duration": 10.0,
        "keep_alive": True,
        "specs": [{"path": "/home"}],
    },
    "mixed-paths-5000": {
        "rate": 5000.0,
        "duration": 10.0,
        "keep_alive": False,
        "specs": [{"path": "/home"}, {"path": "/hello"}],
    },
    "gzip-500": {
        "rate": 500.0,
        "duration": 10.0,
        "keep_alive": False,
        "gzip": True,
        "specs": [{"path": "/home"}],
    },
    "error-paths": {
        "rate": 1000.0,
        "duration": 10.0,
        "keep_alive": False,
        "diagnostic": True,
        "specs": [
            {"method": "GET", "path": "/benchmark_not_found", "expect": [404]},
            {"method": "POST", "path": "/home", "expect": [501]},
        ],
    },
    "slow-clients": {
        "rate": 500.0,
        "duration": 10.0,
        "keep_alive": False,
        "diagnostic": True,
        "slow_clients": 8,
        "specs": [{"path": "/home"}],
    },
}


class RequestError(Exception):
    category = "framing"


class ConnectError(RequestError):
    category = "connect"


class SendError(RequestError):
    category = "send"


class ReceiveError(RequestError):
    category = "receive"


class FramingError(RequestError):
    category = "framing"


class TimeoutError_(RequestError):
    category = "timeout"


class StatusError(RequestError):
    category = "status"


def percentile(values, fraction):
    if not values:
        return 0.0
    ordered = sorted(values)
    index = min(len(ordered) - 1, int(math.ceil(fraction * len(ordered))) - 1)
    return ordered[index]


def git_commit_id():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    try:
        return subprocess.check_output(
            ["git", "-C", repo_root, "rev-parse", "HEAD"],
            text=True,
            stderr=subprocess.DEVNULL,
        ).strip()
    except (OSError, subprocess.CalledProcessError):
        return "unknown"


def read_exact(sock, size):
    result = bytearray()
    while len(result) < size:
        try:
            chunk = sock.recv(min(65536, size - len(result)))
        except socket.timeout:
            raise TimeoutError_("timeout while reading response body")
        except OSError as exc:
            raise ReceiveError("receive failed: %s" % exc)
        if not chunk:
            raise FramingError("connection closed before Content-Length")
        result.extend(chunk)
    return bytes(result)


def read_response(sock):
    head = bytearray()
    while b"\r\n\r\n" not in head:
        try:
            chunk = sock.recv(4096)
        except socket.timeout:
            raise TimeoutError_("timeout while reading response headers")
        except OSError as exc:
            raise ReceiveError("receive failed: %s" % exc)
        if not chunk:
            raise FramingError("connection closed before response headers")
        head.extend(chunk)
        if len(head) > 65536:
            raise FramingError("response headers exceed 64 KiB")

    header_bytes, remainder = bytes(head).split(b"\r\n\r\n", 1)
    lines = header_bytes.split(b"\r\n")
    try:
        version, status_text, _reason = lines[0].decode("ascii").split(" ", 2)
        status = int(status_text)
    except (ValueError, UnicodeDecodeError):
        raise FramingError("malformed HTTP status line")
    if version != "HTTP/1.1":
        raise FramingError("unexpected HTTP version")

    headers = {}
    for line in lines[1:]:
        if b":" not in line:
            raise FramingError("malformed response header")
        name, value = line.split(b":", 1)
        headers[name.decode("ascii").lower()] = value.strip().decode("ascii").lower()

    if "content-length" in headers:
        try:
            length = int(headers["content-length"])
        except ValueError:
            raise FramingError("invalid Content-Length")
        if length < 0:
            raise FramingError("negative Content-Length")
        body = remainder + read_exact(sock, max(0, length - len(remainder)))
        if len(body) != length:
            raise FramingError("response body length mismatch")
    elif headers.get("transfer-encoding") == "chunked":
        body = bytearray()
        pending = bytearray(remainder)
        while True:
            while b"\r\n" not in pending:
                try:
                    chunk = sock.recv(4096)
                except socket.timeout:
                    raise TimeoutError_("timeout while reading chunk header")
                except OSError as exc:
                    raise ReceiveError("receive failed: %s" % exc)
                if not chunk:
                    raise FramingError("connection closed before chunk header")
                pending.extend(chunk)
            line, _, pending = pending.partition(b"\r\n")
            try:
                chunk_size = int(line.split(b";", 1)[0], 16)
            except ValueError:
                raise FramingError("invalid chunk size")
            if chunk_size == 0:
                while len(pending) < 2:
                    try:
                        chunk = sock.recv(4096)
                    except socket.timeout:
                        raise TimeoutError_("timeout while reading chunk trailer")
                    except OSError as exc:
                        raise ReceiveError("receive failed: %s" % exc)
                    if not chunk:
                        raise FramingError("connection closed before chunk trailer")
                    pending.extend(chunk)
                if pending[:2] != b"\r\n":
                    raise FramingError("invalid chunk terminator")
                break
            while len(pending) < chunk_size + 2:
                try:
                    chunk = sock.recv(4096)
                except socket.timeout:
                    raise TimeoutError_("timeout while reading chunk body")
                except OSError as exc:
                    raise ReceiveError("receive failed: %s" % exc)
                if not chunk:
                    raise FramingError("connection closed inside chunk")
                pending.extend(chunk)
            body.extend(pending[:chunk_size])
            if pending[chunk_size:chunk_size + 2] != b"\r\n":
                raise FramingError("invalid chunk terminator")
            del pending[:chunk_size + 2]
    elif headers.get("connection") == "close" or version == "HTTP/1.0":
        # A close-delimited body is valid HTTP/1.1 when the server signals that
        # it will close the connection; used by the error templates.
        body = bytearray(remainder)
        while True:
            try:
                chunk = sock.recv(65536)
            except socket.timeout:
                raise TimeoutError_("timeout while reading close-delimited body")
            except OSError as exc:
                raise ReceiveError("receive failed: %s" % exc)
            if not chunk:
                break
            body.extend(chunk)
    else:
        raise FramingError("response has neither Content-Length nor chunked framing")

    return status, bytes(body)


def send_request(args, spec, sock=None, counters=None):
    """Send one request, return (status, body). Counts connection attempts."""
    own_socket = sock is None
    if own_socket:
        if counters is not None:
            counters["connections"] += 1
        try:
            sock = socket.create_connection((args.host, args.port), args.timeout)
        except OSError as exc:
            raise ConnectError("connect failed: %s" % exc)
        sock.settimeout(args.timeout)
    request = (
        "%s %s HTTP/1.1\r\nHost: %s\r\nConnection: %s\r\n\r\n"
        % (spec["method"], spec["path"], args.host,
           "keep-alive" if args.keep_alive else "close")
    )
    if spec.get("gzip"):
        request = request.replace(
            "\r\n\r\n", "\r\nAccept-Encoding: gzip\r\n\r\n"
        )
    try:
        try:
            sock.sendall(request.encode("ascii"))
        except socket.timeout:
            raise TimeoutError_("timeout while sending request")
        except OSError as exc:
            raise SendError("send failed: %s" % exc)
        status, body = read_response(sock)
        return status, body
    finally:
        if own_socket:
            sock.close()


def worker(args, state, results, worker_id):
    latencies = []
    statuses = collections.Counter()
    errors = collections.Counter()
    counters = {"connections": 0}
    sock = None
    specs = state["specs"]

    if args.keep_alive:
        try:
            counters["connections"] += 1
            sock = socket.create_connection((args.host, args.port), args.timeout)
            sock.settimeout(args.timeout)
        except OSError as exc:
            state["startup_error"] = "connect failed: %s" % exc
            return

    while True:
        with state["lock"]:
            if state["next_request"] >= state["request_limit"]:
                break
            request_number = state["next_request"]
            state["next_request"] += 1
        if args.rate > 0:
            due = state["start"] + request_number / args.rate
            delay = due - time.monotonic()
            if delay > 0:
                time.sleep(delay)

        spec = specs[request_number % len(specs)]
        started = time.monotonic()
        try:
            status, body = send_request(args, spec, sock, counters)
            statuses[status] += 1
            if status not in spec["expect"]:
                errors["status"] += 1
            elif status == 200 and not body:
                errors["framing"] += 1
        except RequestError as exc:
            errors[exc.category] += 1
            if sock is not None:
                sock.close()
                sock = None
                try:
                    counters["connections"] += 1
                    sock = socket.create_connection((args.host, args.port), args.timeout)
                    sock.settimeout(args.timeout)
                except OSError:
                    pass
        except OSError as exc:
            errors["other"] += 1
            if sock is not None:
                try:
                    sock.close()
                except OSError:
                    pass
                sock = None
        latencies.append((time.monotonic() - started) * 1000.0)

    if sock is not None:
        sock.close()

    with state["results_lock"]:
        results.append((latencies, statuses, errors, counters["connections"]))


# --- Server process monitoring (CPU, RSS, fds, context switches) ------------

CLK_TCK = os.sysconf("SC_CLK_TCK")


def read_proc_tcp_retransmits():
    try:
        with open("/proc/net/snmp") as fh:
            lines = fh.read().splitlines()
    except OSError:
        return None
    keys = None
    for i, line in enumerate(lines):
        if line.startswith("Tcp:") and "RetransSegs" in line:
            keys = line.split()[1:]
            if i + 1 < len(lines):
                values = lines[i + 1].split()[1:]
                if len(values) == len(keys):
                    return int(values[keys.index("RetransSegs")])
    return None


class ServerMonitor:
    def __init__(self, pid, interval=0.25):
        self.pid = pid
        self.interval = interval
        self.samples = []
        self.max_rss_kb = 0
        self.max_open_fds = 0
        self.tcp_start = read_proc_tcp_retransmits()
        self.tcp_end = self.tcp_start
        self.start_time = None
        self.end_time = None
        self._stop = threading.Event()
        self._thread = None

    def _read(self):
        base = "/proc/%d" % self.pid
        try:
            with open(base + "/stat") as fh:
                data = fh.read()
            end = data.rfind(")")
            fields = data[end + 2:].split()
            utime = int(fields[11])
            stime = int(fields[12])
            with open(base + "/status") as fh:
                status = {}
                for line in fh:
                    key, _, value = line.partition(":")
                    status[key.strip()] = value.strip()
            rss_kb = int(status.get("VmRSS", "0 kB").split()[0])
            voluntary = int(status.get("voluntary_ctxt_switches", "0"))
            involuntary = int(status.get("nonvoluntary_ctxt_switches", "0"))
            try:
                open_fds = len(os.listdir(base + "/fd"))
            except OSError:
                open_fds = 0
            return {
                "t": time.monotonic(),
                "cpu_ticks": utime + stime,
                "rss_kb": rss_kb,
                "open_fds": open_fds,
                "voluntary": voluntary,
                "involuntary": involuntary,
            }
        except (OSError, ValueError, IndexError):
            return None

    def _loop(self):
        while not self._stop.wait(self.interval):
            sample = self._read()
            if sample:
                self.samples.append(sample)

    def start(self):
        if not self.pid:
            return
        sample = self._read()
        if sample:
            self.samples.append(sample)
            self.start_time = sample["t"]
        self._thread = threading.Thread(target=self._loop, daemon=True)
        self._thread.start()

    def stop(self):
        if not self.pid:
            return
        self._stop.set()
        if self._thread is not None:
            self._thread.join(timeout=1.0)
        sample = self._read()
        if sample:
            self.samples.append(sample)
            self.end_time = sample["t"]
        self.tcp_end = read_proc_tcp_retransmits()
        for sample in self.samples:
            self.max_rss_kb = max(self.max_rss_kb, sample["rss_kb"])
            self.max_open_fds = max(self.max_open_fds, sample["open_fds"])

    def summary(self):
        result = {
            "server_cpu_percent": 0.0,
            "server_rss_kb": self.max_rss_kb,
            "server_open_fds": self.max_open_fds,
            "ctx_switches_voluntary": 0,
            "ctx_switches_involuntary": 0,
            "tcp_retransmits": 0,
        }
        if len(self.samples) >= 2:
            first = self.samples[0]
            last = self.samples[-1]
            elapsed = max(1e-6, last["t"] - first["t"])
            cpu_ticks = last["cpu_ticks"] - first["cpu_ticks"]
            result["server_cpu_percent"] = round(
                100.0 * (cpu_ticks / CLK_TCK) / elapsed, 2
            )
            result["ctx_switches_voluntary"] = last["voluntary"] - first["voluntary"]
            result["ctx_switches_involuntary"] = last["involuntary"] - first["involuntary"]
        if self.tcp_start is not None and self.tcp_end is not None:
            result["tcp_retransmits"] = max(0, self.tcp_end - self.tcp_start)
        return result


def read_server_metrics(path):
    defaults = {
        "server_active_workers_max": 0,
        "server_queue_depth_max": 0,
        "server_accepted_connections": 0,
        "server_rejected_tasks": 0,
        "server_completed_requests": 0,
        "server_request_failures": 0,
    }
    if not path or not os.path.exists(path):
        return defaults
    try:
        with open(path) as fh:
            data = json.load(fh)
    except (OSError, ValueError):
        return defaults
    mapping = {
        "server_active_workers_max": "active_workers_max",
        "server_queue_depth_max": "queue_depth_max",
        "server_accepted_connections": "accepted_connections",
        "server_rejected_tasks": "rejected_tasks",
        "server_completed_requests": "completed_requests",
        "server_request_failures": "request_failures",
    }
    for field, key in mapping.items():
        if key in data:
            defaults[field] = data[key]
    return defaults


# --- Load generation --------------------------------------------------------


def run_load(args, specs, slow_clients=0):
    state = {
        "lock": threading.Lock(),
        "results_lock": threading.Lock(),
        "next_request": 0,
        "request_limit": args.requests,
        "start": time.monotonic(),
        "startup_error": None,
        "specs": specs,
    }
    results = []
    threads = []
    barrier = threading.Barrier(args.concurrency)
    idle = []

    if slow_clients:
        for i in range(slow_clients):
            try:
                sock = socket.create_connection((args.host, args.port), args.timeout)
                sock.settimeout(args.timeout)
                if i % 2 == 0:
                    sock.sendall(b"GET /home HTTP/1.1\r\nHost: benchmark\r\n")
                else:
                    spec = {"method": "GET", "path": "/home", "gzip": False}
                    args_keep = args.keep_alive
                    args.keep_alive = True
                    send_request(args, spec, sock)
                    args.keep_alive = args_keep
                idle.append(sock)
            except OSError:
                break

    def run_worker(_worker_id):
        barrier.wait()
        worker(args, state, results, _worker_id)

    for worker_id in range(args.concurrency):
        thread = threading.Thread(target=run_worker, args=(worker_id,))
        thread.start()
        threads.append(thread)
    for thread in threads:
        thread.join()

    elapsed = max(0.000001, time.monotonic() - state["start"])

    for sock in idle:
        try:
            sock.close()
        except OSError:
            pass

    latencies = [latency for result in results for latency in result[0]]
    statuses = collections.Counter()
    errors = collections.Counter()
    connections = 0
    for _, result_statuses, result_errors, result_connections in results:
        statuses.update(result_statuses)
        errors.update(result_errors)
        connections += result_connections

    return {
        "elapsed": elapsed,
        "latencies": latencies,
        "statuses": statuses,
        "errors": errors,
        "connections": connections,
        "startup_error": state["startup_error"],
        "idle_connections": len(idle),
    }


def summarize(args, spec_names, outcome, server_summary, server_metrics):
    elapsed = outcome["elapsed"]
    latencies = outcome["latencies"]
    statuses = outcome["statuses"]
    errors = outcome["errors"]
    connections = outcome["connections"]

    expected = set()
    for spec in args.specs:
        expected.update(spec["expect"])

    responses_received = sum(
        count for status, count in statuses.items()
        if isinstance(status, int)
    )
    successful = sum(
        count for status, count in statuses.items()
        if isinstance(status, int) and status in expected
    )
    unexpected = responses_received - successful
    failed = unexpected + sum(errors.values())

    status_fields = {
        "status_200": statuses.get(200, 0),
        "status_400": statuses.get(400, 0),
        "status_404": statuses.get(404, 0),
        "status_413": statuses.get(413, 0),
        "status_431": statuses.get(431, 0),
        "status_500": statuses.get(500, 0),
        "status_501": statuses.get(501, 0),
    }
    status_fields["status_other"] = responses_received - sum(status_fields.values())

    result = collections.OrderedDict()
    result["timestamp_utc"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    result["commit_id"] = git_commit_id()
    result["scenario"] = spec_names
    result["host"] = args.host
    result["port"] = args.port
    result["mode"] = "keep-alive" if args.keep_alive else "new-connection"
    result["target_rate"] = args.rate
    result["duration_seconds"] = round(elapsed, 3)
    result["concurrency"] = args.concurrency
    result["requests_planned"] = args.requests
    result["responses_received"] = responses_received
    result["successful"] = successful
    result["failed"] = failed
    result["throughput_rps"] = round(responses_received / elapsed, 2)
    result["successful_rps"] = round(successful / elapsed, 2)
    result["offered_rps"] = args.rate
    result["connection_rate"] = round(connections / elapsed, 2)
    if args.keep_alive and connections:
        result["keepalive_request_rate"] = round(successful / elapsed, 2)
        result["requests_per_connection"] = round(responses_received / connections, 2)
    else:
        result["keepalive_request_rate"] = 0.0
        result["requests_per_connection"] = (
            round(responses_received / connections, 2) if connections else 0.0
        )
    result.update(status_fields)
    result["latency_min_ms"] = round(min(latencies) if latencies else 0.0, 3)
    result["latency_p50_ms"] = round(percentile(latencies, 0.50), 3)
    result["latency_p95_ms"] = round(percentile(latencies, 0.95), 3)
    result["latency_p99_ms"] = round(percentile(latencies, 0.99), 3)
    result["latency_max_ms"] = round(max(latencies) if latencies else 0.0, 3)
    result["fail_connect"] = errors.get("connect", 0)
    result["fail_send"] = errors.get("send", 0)
    result["fail_receive"] = errors.get("receive", 0)
    result["fail_framing"] = errors.get("framing", 0)
    result["fail_status"] = errors.get("status", 0)
    result["fail_timeout"] = errors.get("timeout", 0)
    result["fail_other"] = errors.get("other", 0)
    result.update(server_summary)
    result.update(server_metrics)
    return result


# --- Result logging ---------------------------------------------------------


def log_result(path, result):
    parent = os.path.dirname(path)
    if parent:
        os.makedirs(parent, exist_ok=True)
    if path.endswith(".json") or path.endswith(".jsonl"):
        with open(path, "a") as output:
            output.write(json.dumps(result) + "\n")
    else:
        write_header = not os.path.exists(path) or os.path.getsize(path) == 0
        with open(path, "a", newline="") as output:
            writer = csv.DictWriter(output, fieldnames=CSV_FIELDS)
            if write_header:
                writer.writeheader()
            writer.writerow(result)


def wait_for_server(host, port, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            with socket.create_connection((host, port), 0.2):
                return
        except OSError:
            time.sleep(0.05)
    raise RuntimeError("server did not start on %s:%d" % (host, port))


def port_in_use(host, port):
    try:
        with socket.create_connection((host, port), 0.2):
            return True
    except OSError:
        return False


def build_specs(args):
    if args.expect_status:
        expected = [int(code) for code in args.expect_status.split(",") if code]
    else:
        expected = None
    specs = []
    base_gzip = args.gzip if args.gzip is not None else False
    base_method = args.method or "GET"
    if args.paths:
        for path in args.paths:
            specs.append({
                "method": base_method,
                "path": path,
                "gzip": base_gzip,
                "expect": expected or [200],
            })
    return specs, expected


def parse_args(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8081)
    parser.add_argument("--scenario", choices=sorted(SCENARIOS.keys()),
                        help="run one of the scenarios from the measurement contract")
    parser.add_argument("--path", dest="paths", action="append",
                        help="request path; repeat for a path mix")
    parser.add_argument("--method", default=None, help="HTTP method (default GET)")
    parser.add_argument("--duration", type=float, default=None)
    parser.add_argument("--rate", type=float, default=None,
                        help="target request rate; 0 means closed-loop")
    parser.add_argument("--requests", type=int, default=0,
                        help="total requests (defaults to ceil(rate * duration))")
    parser.add_argument("--concurrency", type=int, default=None)
    parser.add_argument("--timeout", type=float, default=10.0)
    parser.add_argument("--min-rps", type=float, default=0.0)
    parser.add_argument("--expect-status", default=None,
                        help="comma-separated expected status codes (default 200)")
    parser.add_argument("--log-file", help="append results and commit ID to CSV or JSON")
    parser.add_argument("--keep-alive", action="store_true", default=None)
    parser.add_argument("--gzip", action="store_true", default=None,
                        help="send Accept-Encoding: gzip")
    parser.add_argument("--slow-clients", type=int, default=None,
                        help="number of idle/partial connections held during the run")
    parser.add_argument("--start-server", action="store_true")
    parser.add_argument("--server-pid", type=int, default=None,
                        help="sample an already-running server process")
    parser.add_argument("--server", default="./bin/http_server")
    parser.add_argument("--server-metrics-file", default=None,
                        help="JSON metrics snapshot written by the server")
    args = parser.parse_args(argv)

    scenario = SCENARIOS.get(args.scenario, {}) if args.scenario else {}

    def pick(value, key, fallback):
        if value is not None:
            return value
        if key in scenario:
            return scenario[key]
        return fallback

    args.rate = pick(args.rate, "rate", 100.0)
    args.duration = pick(args.duration, "duration", 10.0)
    args.concurrency = pick(args.concurrency, "concurrency", 16)
    args.keep_alive = bool(pick(args.keep_alive, "keep_alive", False))
    args.gzip = bool(pick(args.gzip, "gzip", False))
    args.slow_clients = pick(args.slow_clients, "slow_clients", 0)
    args.diagnostic = bool(scenario.get("diagnostic", False))

    if not args.paths:
        scenario_specs = scenario.get("specs")
        if scenario_specs:
            args.specs = []
            expected = None
            if args.expect_status:
                expected = [int(c) for c in args.expect_status.split(",") if c]
            for spec in scenario_specs:
                merged = {
                    "method": spec.get("method", args.method or "GET"),
                    "path": spec["path"],
                    "gzip": spec.get("gzip", args.gzip),
                    "expect": expected or spec.get("expect", [200]),
                }
                args.specs.append(merged)
        else:
            args.specs = [{
                "method": args.method or "GET",
                "path": "/home",
                "gzip": args.gzip,
                "expect": [int(c) for c in args.expect_status.split(",") if c] or [200],
            }]
    else:
        args.specs, _ = build_specs(args)

    if args.concurrency < 1 or args.duration <= 0 or args.rate < 0:
        parser.error("duration, rate, and concurrency values are invalid")
    if args.requests <= 0:
        if args.rate <= 0:
            parser.error("--requests is required when --rate=0")
        args.requests = int(math.ceil(args.rate * args.duration))
    return args


def main(argv=None):
    args = parse_args(argv)
    server = None
    metrics_file = args.server_metrics_file
    created_metrics_file = None
    monitor = None
    try:
        if not metrics_file:
            fd, metrics_file = tempfile.mkstemp(prefix="http_server_metrics_", suffix=".json")
            os.close(fd)
            created_metrics_file = metrics_file

        if args.start_server:
            if port_in_use(args.host, args.port):
                raise RuntimeError(
                    "a server is already listening on %s:%d; stop it or drop "
                    "--start-server" % (args.host, args.port))
            env = dict(os.environ)
            env["HTTP_SERVER_ACCESS_LOG"] = "0"
            env["HTTP_SERVER_METRICS_FILE"] = metrics_file
            server = subprocess.Popen(
                [args.server], cwd=os.getcwd(), env=env,
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            )
            wait_for_server(args.host, args.port, 5.0)
            if server.poll() is not None:
                raise RuntimeError(
                    "server exited during startup (exit code %s)" % server.returncode)
            server_pid = server.pid
        else:
            wait_for_server(args.host, args.port, 1.0)
            server_pid = args.server_pid

        monitor = ServerMonitor(server_pid)
        monitor.start()
        outcome = run_load(args, args.specs, slow_clients=args.slow_clients)
        monitor.stop()

        server_summary = monitor.summary()
        server_metrics = read_server_metrics(metrics_file)
        result = summarize(args, args.scenario or "custom", outcome,
                           server_summary, server_metrics)

        print("requests=%d responses=%d successful=%d failed=%d elapsed=%.3fs" %
              (args.requests, result["responses_received"], result["successful"],
               result["failed"], outcome["elapsed"]))
        print("throughput=%.2f req/s successful=%.2f req/s target=%.2f req/s mode=%s" %
              (result["throughput_rps"], result["successful_rps"], args.rate,
               result["mode"]))
        print("latency_ms min=%.2f p50=%.2f p95=%.2f p99=%.2f max=%.2f" %
              (result["latency_min_ms"], result["latency_p50_ms"],
               result["latency_p95_ms"], result["latency_p99_ms"],
               result["latency_max_ms"]))
        print("statuses=%s failures=%s" % (
            dict(sorted((k, v) for k, v in outcome["statuses"].items() if isinstance(k, int))),
            dict(sorted(outcome["errors"].items()))))
        print("connections=%d conn_rate=%.2f req_per_conn=%.2f server_cpu=%.2f%% "
              "server_rss=%dkB server_fds=%d queue_depth_max=%d rejected=%d" % (
                  outcome["connections"], result["connection_rate"],
                  result["requests_per_connection"], result["server_cpu_percent"],
                  result["server_rss_kb"], result["server_open_fds"],
                  result["server_queue_depth_max"], result["server_rejected_tasks"]))

        if args.log_file:
            try:
                log_result(args.log_file, result)
                print("result_log=%s" % args.log_file)
            except OSError as exc:
                print("FAIL: could not write result log: %s" % exc, file=sys.stderr)
                return 2

        if outcome["startup_error"]:
            print("FAIL: %s" % outcome["startup_error"], file=sys.stderr)
            return 1
        if args.diagnostic:
            return 0
        if result["failed"]:
            print("FAIL: %d requests failed" % result["failed"], file=sys.stderr)
            return 1
        if args.min_rps and result["successful_rps"] < args.min_rps:
            print("FAIL: throughput %.2f is below minimum %.2f req/s" %
                  (result["successful_rps"], args.min_rps), file=sys.stderr)
            return 1
        return 0
    except (OSError, RuntimeError) as exc:
        print("FAIL: %s" % exc, file=sys.stderr)
        return 2
    finally:
        if server is not None:
            server.send_signal(signal.SIGTERM)
            try:
                server.wait(timeout=3)
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait()
        if created_metrics_file and os.path.exists(created_metrics_file):
            os.remove(created_metrics_file)


if __name__ == "__main__":
    sys.exit(main())
